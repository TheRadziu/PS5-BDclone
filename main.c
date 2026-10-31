#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <ctype.h>
#include <stddef.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "web_ui.h"

// Notification structure for libkernel (standard in ps5-payload-sdk)
typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;

// Declarations of PS5 notification functions (declared weak for smooth linking)
extern int sceKernelSendNotificationRequest(int device, notify_request_t* req, size_t size, int blocking) __attribute__((weak));
extern int sceSysUtilSendSystemNotificationWithText(int type, const char* text) __attribute__((weak));

// Default paths
const char* DEFAULT_DISC_PATH = "/mnt/disc";
const char* DEFAULT_USB_BASE  = "/mnt/usb0";

// Supported optical disc media types
typedef enum {
    DISC_TYPE_UNKNOWN = 0,
    DISC_TYPE_BD_MOVIE,
    DISC_TYPE_PS3_GAME
} DiscType;

const char* DetectUsbMount(void);

// --- Logging subsystem for BDclone.log on USB ---
static FILE* g_logFile = NULL;
static char g_logBufferEarly[8192] = {0};
static size_t g_logBufferEarlyLen = 0;

void InitLog(const char* usbMount) {
    if (g_logFile) return;
    char logPath[512];
    snprintf(logPath, sizeof(logPath), "%s/BDclone.log", usbMount);
    g_logFile = fopen(logPath, "a");
    if (g_logFile) {
        if (g_logBufferEarlyLen > 0) {
            fwrite(g_logBufferEarly, 1, g_logBufferEarlyLen, g_logFile);
            fflush(g_logFile);
            fsync(fileno(g_logFile));
            g_logBufferEarlyLen = 0;
        }
    }
}

void CloseLog(void) {
    if (g_logFile) {
        fflush(g_logFile);
        fsync(fileno(g_logFile));
        fclose(g_logFile);
        g_logFile = NULL;
    }
}

void LogMessage(const char* fmt, ...) {
    char timeStr[32];
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    if (tm_info) {
        strftime(timeStr, sizeof(timeStr), "[%Y-%m-%d %H:%M:%S] ", tm_info);
    } else {
        snprintf(timeStr, sizeof(timeStr), "[--:--:--] ");
    }

    char msgBuffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msgBuffer, sizeof(msgBuffer), fmt, args);
    va_end(args);

    if (g_logFile) {
        fprintf(g_logFile, "%s%s\n", timeStr, msgBuffer);
        fflush(g_logFile);
        fsync(fileno(g_logFile));
    } else {
        // Buffer into RAM if USB log file is not opened yet
        size_t needed = strlen(timeStr) + strlen(msgBuffer) + 2;
        if (sizeof(g_logBufferEarly) - g_logBufferEarlyLen > needed) {
            g_logBufferEarlyLen += snprintf(g_logBufferEarly + g_logBufferEarlyLen,
                                           sizeof(g_logBufferEarly) - g_logBufferEarlyLen,
                                           "%s%s\n", timeStr, msgBuffer);
        }
    }
}

// Sends on-screen system notification on PS5 (and writes to log)
void SendNotification(const char* message) {
    LogMessage("[Notify] %s", message);
    if (sceKernelSendNotificationRequest) {
        notify_request_t req;
        memset(&req, 0, sizeof(req));
        snprintf(req.message, sizeof(req.message), "%s", message);
        sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
    } else if (sceSysUtilSendSystemNotificationWithText) {
        sceSysUtilSendSystemNotificationWithText(222, message);
    }
}

// ============================================================================
// --- Embedded Real-Time Web Server & Status Monitor ---
// ============================================================================

typedef struct {
    char state[64];         // "WAITING_DISC", "ANALYZING", "COPYING", "FINISHED", "ERROR"
    char title[256];        // Disc title, e.g. "Rocksmith [BLES01216]"
    char mediaType[64];     // "PS3 Game disc" / "Blu-ray Movie disc"
    char encryption[128];   // "PS3 Disc Encryption (DKEY / 3k3y / Redump)", "ENCRYPTED (AACS)", "CLEAR"
    char mode[64];          // "Direct ISO clone (/dev/cd0) [DEFAULT]" or "Folder mode"
    char destination[512];  // Destination path on USB
    char usbMount[64];      // "/mnt/usb0"
    uint64_t usbTotalBytes;
    uint64_t usbFreeBytes;
    uint64_t usbUsedBytes;
    uint64_t bytesCopied;
    uint64_t totalBytes;
    double progressPct;
    double speedMBps;
    double avgSpeedMBps;
    int elapsedSeconds;
    int etaSeconds;
    char statusMessage[256];
    int isComplete;
    int hasError;
} WebStatus;

static pthread_mutex_t g_webMutex = PTHREAD_MUTEX_INITIALIZER;
static WebStatus g_webStatus = {
    .state = "WAITING_DISC",
    .statusMessage = "Waiting for optical disc insertion...",
    .mediaType = "---",
    .title = "Waiting for disc...",
    .encryption = "---",
    .mode = "Direct ISO clone (/dev/cd0) [DEFAULT]",
    .destination = "---",
    .usbMount = "---",
    .usbTotalBytes = 0,
    .usbFreeBytes = 0,
    .usbUsedBytes = 0,
    .bytesCopied = 0,
    .totalBytes = 0,
    .progressPct = 0.0,
    .speedMBps = 0.0,
    .avgSpeedMBps = 0.0,
    .elapsedSeconds = 0,
    .etaSeconds = 0,
    .isComplete = 0,
    .hasError = 0
};

static volatile int g_webServerRunning = 1;
static int g_webPort = 8081;
static char g_consoleIp[64] = "127.0.0.1";
static pthread_t g_webThread;

static void EscapeJsonString(char* dst, const char* src, size_t maxLen) {
    if (!dst || maxLen == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t d = 0;
    while (*src && d < maxLen - 1) {
        if (*src == '"' || *src == '\\') {
            if (d < maxLen - 2) {
                dst[d++] = '\\';
                dst[d++] = *src;
            } else break;
        } else if (*src == '\n' || *src == '\r' || *src == '\t') {
            dst[d++] = ' ';
        } else {
            dst[d++] = *src;
        }
        src++;
    }
    dst[d] = '\0';
}

static void WebUpdateState(const char* state, const char* msg) {
    pthread_mutex_lock(&g_webMutex);
    if (state) snprintf(g_webStatus.state, sizeof(g_webStatus.state), "%s", state);
    if (msg) snprintf(g_webStatus.statusMessage, sizeof(g_webStatus.statusMessage), "%s", msg);
    pthread_mutex_unlock(&g_webMutex);
}

static void WebUpdateDiscInfo(const char* title, const char* mediaType, const char* enc, const char* mode, const char* dest, uint64_t totalBytes) {
    pthread_mutex_lock(&g_webMutex);
    if (title) {
        char cleanTitle[256];
        snprintf(cleanTitle, sizeof(cleanTitle), "%s", title);
        size_t len = strlen(cleanTitle);
        if (len > 10 && strcmp(cleanTitle + len - 10, ".encrypted") == 0) {
            cleanTitle[len - 10] = '\0';
        }
        snprintf(g_webStatus.title, sizeof(g_webStatus.title), "%s", cleanTitle);
    }
    if (mediaType) snprintf(g_webStatus.mediaType, sizeof(g_webStatus.mediaType), "%s", mediaType);
    if (enc) snprintf(g_webStatus.encryption, sizeof(g_webStatus.encryption), "%s", enc);
    if (mode) snprintf(g_webStatus.mode, sizeof(g_webStatus.mode), "%s", mode);
    if (dest) snprintf(g_webStatus.destination, sizeof(g_webStatus.destination), "%s", dest);
    if (totalBytes > 0) g_webStatus.totalBytes = totalBytes;
    pthread_mutex_unlock(&g_webMutex);
}

static void WebSetUsbDisconnected(void) {
    pthread_mutex_lock(&g_webMutex);
    snprintf(g_webStatus.usbMount, sizeof(g_webStatus.usbMount), "---");
    g_webStatus.usbTotalBytes = 0;
    g_webStatus.usbFreeBytes = 0;
    g_webStatus.usbUsedBytes = 0;
    pthread_mutex_unlock(&g_webMutex);
}

static void WebUpdateUsbStats(const char* usbMount) {
    if (!usbMount || usbMount[0] == '\0' || strcmp(usbMount, "---") == 0) {
        WebSetUsbDisconnected();
        return;
    }
    struct statvfs vfs;
    if (statvfs(usbMount, &vfs) == 0 && vfs.f_blocks > 0 && vfs.f_frsize > 0) {
        uint64_t total = (uint64_t)vfs.f_blocks * (uint64_t)vfs.f_frsize;
        uint64_t freeBytes = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
        uint64_t used = (total > freeBytes) ? (total - freeBytes) : 0;
        
        pthread_mutex_lock(&g_webMutex);
        snprintf(g_webStatus.usbMount, sizeof(g_webStatus.usbMount), "%s", usbMount);
        g_webStatus.usbTotalBytes = total;
        g_webStatus.usbFreeBytes = freeBytes;
        g_webStatus.usbUsedBytes = used;
        pthread_mutex_unlock(&g_webMutex);
    } else {
        WebSetUsbDisconnected();
    }
}

static void WebUpdateProgress(uint64_t copied, uint64_t total, double speedMBs, double avgSpeedMBs, int elapsedSec, int etaSec) {
    pthread_mutex_lock(&g_webMutex);
    g_webStatus.bytesCopied = copied;
    if (total > 0) {
        g_webStatus.totalBytes = total;
        g_webStatus.progressPct = ((double)copied * 100.0) / (double)total;
        if (g_webStatus.progressPct > 100.0) g_webStatus.progressPct = 100.0;
    }
    g_webStatus.speedMBps = speedMBs;
    g_webStatus.avgSpeedMBps = avgSpeedMBs;
    g_webStatus.elapsedSeconds = elapsedSec;
    g_webStatus.etaSeconds = etaSec;
    pthread_mutex_unlock(&g_webMutex);
}

static void WebSetComplete(int isSuccess, const char* msg) {
    pthread_mutex_lock(&g_webMutex);
    if (isSuccess) {
        g_webStatus.isComplete = 1;
        g_webStatus.progressPct = 100.0;
        snprintf(g_webStatus.state, sizeof(g_webStatus.state), "FINISHED");
        if (msg) snprintf(g_webStatus.statusMessage, sizeof(g_webStatus.statusMessage), "%s", msg);
    } else {
        g_webStatus.hasError = 1;
        snprintf(g_webStatus.state, sizeof(g_webStatus.state), "ERROR");
        if (msg) snprintf(g_webStatus.statusMessage, sizeof(g_webStatus.statusMessage), "%s", msg);
    }
    pthread_mutex_unlock(&g_webMutex);
}

static void DetectConsoleIp(char* outIp, size_t maxLen) {
    strncpy(outIp, "127.0.0.1", maxLen);
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s >= 0) {
        struct sockaddr_in target;
        memset(&target, 0, sizeof(target));
        target.sin_family = AF_INET;
        target.sin_addr.s_addr = inet_addr("1.1.1.1");
        target.sin_port = htons(53);
        if (connect(s, (struct sockaddr*)&target, sizeof(target)) == 0) {
            struct sockaddr_in local;
            socklen_t localLen = sizeof(local);
            if (getsockname(s, (struct sockaddr*)&local, &localLen) == 0) {
                inet_ntop(AF_INET, &local.sin_addr, outIp, maxLen);
            }
        }
        close(s);
    }
}

static void SendJsonStatus(int clientSock) {
    // Dynamically refresh USB mount status in real-time
    const char* detectedUsb = DetectUsbMount();
    if (detectedUsb) {
        WebUpdateUsbStats(detectedUsb);
        InitLog(detectedUsb);
    } else {
        WebSetUsbDisconnected();
    }

    WebStatus st;
    pthread_mutex_lock(&g_webMutex);
    st = g_webStatus;
    pthread_mutex_unlock(&g_webMutex);

    char safeTitle[256];
    char safeDest[512];
    char safeMsg[256];
    char safeUsb[64];
    EscapeJsonString(safeTitle, st.title, sizeof(safeTitle));
    EscapeJsonString(safeDest, st.destination, sizeof(safeDest));
    EscapeJsonString(safeMsg, st.statusMessage, sizeof(safeMsg));
    EscapeJsonString(safeUsb, st.usbMount, sizeof(safeUsb));

    int elapsedM = st.elapsedSeconds / 60;
    int elapsedS = st.elapsedSeconds % 60;
    int etaM = st.etaSeconds / 60;
    int etaS = st.etaSeconds % 60;

    double copiedGB = (double)st.bytesCopied / (1024.0 * 1024.0 * 1024.0);
    double totalGB = (double)st.totalBytes / (1024.0 * 1024.0 * 1024.0);

    double usbTotalGB = (double)st.usbTotalBytes / (1024.0 * 1024.0 * 1024.0);
    double usbFreeGB  = (double)st.usbFreeBytes  / (1024.0 * 1024.0 * 1024.0);
    double usbUsedGB  = (double)st.usbUsedBytes  / (1024.0 * 1024.0 * 1024.0);
    double usbUsedPct = (st.usbTotalBytes > 0) ? ((double)st.usbUsedBytes * 100.0) / (double)st.usbTotalBytes : 0.0;

    char body[2560];
    int bodyLen = snprintf(body, sizeof(body),
        "{"
        "\"state\":\"%s\","
        "\"title\":\"%s\","
        "\"mediaType\":\"%s\","
        "\"encryption\":\"%s\","
        "\"mode\":\"%s\","
        "\"destination\":\"%s\","
        "\"usbMount\":\"%s\","
        "\"usbTotalBytes\":%llu,"
        "\"usbFreeBytes\":%llu,"
        "\"usbUsedBytes\":%llu,"
        "\"usbTotalGB\":%.2f,"
        "\"usbUsedGB\":%.2f,"
        "\"usbFreeGB\":%.2f,"
        "\"usbUsedPct\":%.1f,"
        "\"bytesCopied\":%llu,"
        "\"totalBytes\":%llu,"
        "\"copiedGB\":%.2f,"
        "\"totalGB\":%.2f,"
        "\"progressPct\":%.1f,"
        "\"speedMBps\":%.1f,"
        "\"avgSpeedMBps\":%.1f,"
        "\"elapsedSec\":%d,"
        "\"etaSec\":%d,"
        "\"elapsedStr\":\"%02dm %02ds\","
        "\"etaStr\":\"%02dm %02ds\","
        "\"statusMessage\":\"%s\","
        "\"isComplete\":%s,"
        "\"hasError\":%s"
        "}",
        st.state, safeTitle, st.mediaType, st.encryption, st.mode, safeDest,
        safeUsb,
        (unsigned long long)st.usbTotalBytes,
        (unsigned long long)st.usbFreeBytes,
        (unsigned long long)st.usbUsedBytes,
        usbTotalGB, usbUsedGB, usbFreeGB, usbUsedPct,
        (unsigned long long)st.bytesCopied, (unsigned long long)st.totalBytes,
        copiedGB, totalGB, st.progressPct, st.speedMBps, st.avgSpeedMBps,
        st.elapsedSeconds, st.etaSeconds,
        elapsedM, elapsedS, etaM, etaS,
        safeMsg,
        st.isComplete ? "true" : "false",
        st.hasError ? "true" : "false"
    );

    char header[256];
    int headerLen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "\r\n",
        bodyLen
    );

    send(clientSock, header, headerLen, 0);
    send(clientSock, body, bodyLen, 0);
}

static void SendHtmlPage(int clientSock) {
    size_t bodyLen = strlen(g_indexHtml);
    char header[256];
    int headerLen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "\r\n",
        bodyLen
    );

    send(clientSock, header, headerLen, 0);
    send(clientSock, g_indexHtml, bodyLen, 0);
}

static void* WebServerThread(void* arg) {
    int serverSock = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSock < 0) {
        LogMessage("[Web] [ERROR] Failed to create socket (errno=%d: %s)", errno, strerror(errno));
        return NULL;
    }

    int opt = 1;
    setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in servAddr;
    memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    servAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servAddr.sin_port = htons(g_webPort);

    if (bind(serverSock, (struct sockaddr*)&servAddr, sizeof(servAddr)) < 0) {
        LogMessage("[Web] [WARN] Cannot bind port %d (errno=%d). Trying port 8080...", g_webPort, errno);
        g_webPort = 8080;
        servAddr.sin_port = htons(g_webPort);
        if (bind(serverSock, (struct sockaddr*)&servAddr, sizeof(servAddr)) < 0) {
            LogMessage("[Web] [ERROR] Cannot bind port 8080 either. Web server aborted.");
            close(serverSock);
            return NULL;
        }
    }

    if (listen(serverSock, 16) < 0) {
        LogMessage("[Web] [ERROR] listen() failed (errno=%d: %s)", errno, strerror(errno));
        close(serverSock);
        return NULL;
    }

    // Set 1s timeout on accept so the thread can terminate gracefully
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(serverSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    LogMessage("[Web] Web server listening on http://%s:%d (and http://127.0.0.1:%d)", 
               g_consoleIp, g_webPort, g_webPort);

    while (g_webServerRunning) {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        int clientSock = accept(serverSock, (struct sockaddr*)&clientAddr, &clientLen);
        if (clientSock < 0) {
            continue;
        }

        struct timeval ctv;
        ctv.tv_sec = 3;
        ctv.tv_usec = 0;
        setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, &ctv, sizeof(ctv));
        setsockopt(clientSock, SOL_SOCKET, SO_SNDTIMEO, &ctv, sizeof(ctv));

        char reqBuf[2048] = {0};
        ssize_t reqLen = recv(clientSock, reqBuf, sizeof(reqBuf) - 1, 0);
        if (reqLen > 0) {
            reqBuf[reqLen] = '\0';
            if (strncmp(reqBuf, "GET /api/status", 15) == 0 || strncmp(reqBuf, "GET /status", 11) == 0) {
                SendJsonStatus(clientSock);
            } else {
                SendHtmlPage(clientSock);
            }
        }
        close(clientSock);
    }

    close(serverSock);
    LogMessage("[Web] Web server stopped.");
    return NULL;
}

// Case-insensitive substring search helper
static const char* StrCaseStr(const char* haystack, const char* needle) {
    if (!haystack || !needle) return NULL;
    size_t needleLen = strlen(needle);
    if (needleLen == 0) return haystack;
    while (*haystack) {
        if (strncasecmp(haystack, needle, needleLen) == 0) {
            return haystack;
        }
        haystack++;
    }
    return NULL;
}

// Sanitizes a string to ensure valid folder and file names on exFAT/FAT32/NTFS
static void SanitizeFilename(char* dst, const char* src, size_t maxLen) {
    if (!dst || !src || maxLen == 0) return;

    // Skip leading spaces, whitespace and separators
    while (*src && (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n' || *src == '_')) {
        src++;
    }

    size_t d = 0;
    while (*src && d < maxLen - 1) {
        unsigned char c = (unsigned char)*src;
        // Characters forbidden in file/directory names
        if (c < 32 || c == '/' || c == '\\' || c == ':' || c == '*' || 
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == 127) {
            // Replace with underscore, avoiding consecutive underscores
            if (d > 0 && dst[d - 1] != '_') {
                dst[d++] = '_';
            }
        } else {
            dst[d++] = c;
        }
        src++;
    }
    dst[d] = '\0';

    // Trim trailing spaces, periods, and underscores
    while (d > 0 && (dst[d - 1] == ' ' || dst[d - 1] == '_' || dst[d - 1] == '\t' || dst[d - 1] == '.')) {
        dst[--d] = '\0';
    }
}

// Attempts to extract movie title from BDMV XML metadata (e.g. bdmt_eng.xml)
static int TryGetTitleFromXml(const char* xmlPath, char* outTitle, size_t maxLen) {
    FILE* f = fopen(xmlPath, "rb");
    if (!f) return 0;

    char buffer[16384];
    size_t readBytes = fread(buffer, 1, sizeof(buffer) - 1, f);
    fclose(f);
    if (readBytes == 0) return 0;
    buffer[readBytes] = '\0';

    // Search for <di:name> or <name> tags
    const char* startTag = StrCaseStr(buffer, "<di:name>");
    size_t tagLen = 9;
    if (!startTag) {
        startTag = StrCaseStr(buffer, "<name>");
        tagLen = 6;
    }

    if (startTag) {
        const char* valStart = startTag + tagLen;
        // Skip CDATA if present
        if (StrCaseStr(valStart, "<![CDATA[") == valStart) {
            valStart += 9;
        }

        const char* valEnd = StrCaseStr(valStart, "</");
        if (valEnd) {
            const char* cdataEnd = strstr(valStart, "]]>");
            if (cdataEnd && cdataEnd < valEnd) {
                valEnd = cdataEnd;
            }

            size_t titleLen = valEnd - valStart;
            if (titleLen > 0 && titleLen < 256) {
                char rawTitle[256];
                memcpy(rawTitle, valStart, titleLen);
                rawTitle[titleLen] = '\0';

                char clean[256];
                SanitizeFilename(clean, rawTitle, sizeof(clean));
                if (strlen(clean) > 0) {
                    strncpy(outTitle, clean, maxLen - 1);
                    outTitle[maxLen - 1] = '\0';
                    return 1;
                }
            }
        }
    }
    return 0;
}

// Scans META/DL directory for BDMV disc metadata XML files
static int TryGetTitleFromMetaDir(const char* discMount, char* outTitle, size_t maxLen) {
    const char* subDirs[] = {
        "BDMV/META/DL",
        "bdmv/meta/dl",
        "BDMV/meta/dl",
        "META/DL",
        "meta/dl",
        NULL
    };

    char metaDir[1024];
    for (int i = 0; subDirs[i] != NULL; i++) {
        snprintf(metaDir, sizeof(metaDir), "%s/%s", discMount, subDirs[i]);
        DIR* dir = opendir(metaDir);
        if (!dir) continue;

        struct dirent* entry;
        char candidate[1024];

        // 1. Priority: bdmt_eng.xml or any bdmt_*.xml
        while ((entry = readdir(dir)) != NULL) {
            if (StrCaseStr(entry->d_name, "bdmt_") && StrCaseStr(entry->d_name, ".xml")) {
                snprintf(candidate, sizeof(candidate), "%s/%s", metaDir, entry->d_name);
                if (TryGetTitleFromXml(candidate, outTitle, maxLen)) {
                    closedir(dir);
                    return 1;
                }
            }
        }
        rewinddir(dir);

        // 2. Fallback: any XML file in META/DL folder
        while ((entry = readdir(dir)) != NULL) {
            if (StrCaseStr(entry->d_name, ".xml")) {
                snprintf(candidate, sizeof(candidate), "%s/%s", metaDir, entry->d_name);
                if (TryGetTitleFromXml(candidate, outTitle, maxLen)) {
                    closedir(dir);
                    return 1;
                }
            }
        }
        closedir(dir);
    }
    return 0;
}

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;           // 0x46535000 ("\0PSF")
    uint32_t version;         // 0x00000101
    uint32_t key_table_start;
    uint32_t data_table_start;
    uint32_t tables_entries;
} SfoHeader;

typedef struct {
    uint16_t key_offset;
    uint16_t param_fmt;
    uint32_t param_len;
    uint32_t param_max_len;
    uint32_t data_offset;
} SfoEntry;
#pragma pack(pop)

// Parses PS3 PARAM.SFO file to extract TITLE and TITLE_ID
static int TryGetTitleFromSfo(const char* sfoPath, char* outTitle, size_t maxLen) {
    FILE* f = fopen(sfoPath, "rb");
    if (!f) return 0;

    uint8_t buffer[16384];
    size_t readBytes = fread(buffer, 1, sizeof(buffer), f);
    fclose(f);

    if (readBytes < sizeof(SfoHeader)) return 0;

    SfoHeader* hdr = (SfoHeader*)buffer;
    if (hdr->magic != 0x46535000) { // "\0PSF"
        return 0;
    }

    if (hdr->key_table_start >= readBytes || hdr->data_table_start >= readBytes) {
        return 0;
    }

    char gameTitle[256] = {0};
    char titleId[64] = {0};

    size_t entriesOffset = sizeof(SfoHeader);
    for (uint32_t i = 0; i < hdr->tables_entries; i++) {
        size_t entryOffset = entriesOffset + (i * sizeof(SfoEntry));
        if (entryOffset + sizeof(SfoEntry) > readBytes) break;

        SfoEntry* entry = (SfoEntry*)(buffer + entryOffset);
        size_t keyPos = hdr->key_table_start + entry->key_offset;
        size_t dataPos = hdr->data_table_start + entry->data_offset;

        if (keyPos < readBytes && dataPos < readBytes) {
            const char* key = (const char*)(buffer + keyPos);
            if (strcmp(key, "TITLE") == 0) {
                size_t len = entry->param_len;
                if (dataPos + len > readBytes) len = readBytes - dataPos;
                if (len > sizeof(gameTitle) - 1) len = sizeof(gameTitle) - 1;
                memcpy(gameTitle, buffer + dataPos, len);
                gameTitle[len] = '\0';
            } else if (strcmp(key, "TITLE_ID") == 0) {
                size_t len = entry->param_len;
                if (dataPos + len > readBytes) len = readBytes - dataPos;
                if (len > sizeof(titleId) - 1) len = sizeof(titleId) - 1;
                memcpy(titleId, buffer + dataPos, len);
                titleId[len] = '\0';
            }
        }
    }

    char cleanTitle[256] = {0};
    char cleanId[64] = {0};
    SanitizeFilename(cleanTitle, gameTitle, sizeof(cleanTitle));
    SanitizeFilename(cleanId, titleId, sizeof(cleanId));

    if (strlen(cleanTitle) > 0 && strlen(cleanId) > 0) {
        snprintf(outTitle, maxLen, "%s [%s]", cleanTitle, cleanId);
        return 1;
    } else if (strlen(cleanTitle) > 0) {
        snprintf(outTitle, maxLen, "%s", cleanTitle);
        return 1;
    } else if (strlen(cleanId) > 0) {
        snprintf(outTitle, maxLen, "%s", cleanId);
        return 1;
    }

    return 0;
}

// Attempts to extract Title ID from PS3_DISC.SFB
static int TryGetTitleFromSfb(const char* sfbPath, char* outTitle, size_t maxLen) {
    FILE* f = fopen(sfbPath, "rb");
    if (!f) return 0;

    uint8_t buffer[512];
    size_t readBytes = fread(buffer, 1, sizeof(buffer), f);
    fclose(f);

    if (readBytes < 0x30) return 0;

    // PS3_DISC.SFB has magic ".SFB" at offset 0
    if (buffer[0] == '.' && buffer[1] == 'S' && buffer[2] == 'F' && buffer[3] == 'B') {
        char rawId[32] = {0};
        memcpy(rawId, buffer + 0x20, 16);
        rawId[16] = '\0';

        char cleanId[64];
        SanitizeFilename(cleanId, rawId, sizeof(cleanId));
        if (strlen(cleanId) > 3) {
            snprintf(outTitle, maxLen, "%s", cleanId);
            return 1;
        }
    }
    return 0;
}

// Scans PS3 game disc structure for title and ID
static int TryGetTitleFromPs3Game(const char* discMount, char* outTitle, size_t maxLen) {
    const char* sfoCandidates[] = {
        "PS3_GAME/PARAM.SFO",
        "ps3_game/param.sfo",
        "PS3_GAME/param.sfo",
        "ps3_game/PARAM.SFO",
        "PARAM.SFO",
        "param.sfo",
        NULL
    };

    char sfoPath[1024];
    for (int i = 0; sfoCandidates[i] != NULL; i++) {
        snprintf(sfoPath, sizeof(sfoPath), "%s/%s", discMount, sfoCandidates[i]);
        if (TryGetTitleFromSfo(sfoPath, outTitle, maxLen)) {
            return 1;
        }
    }

    const char* sfbCandidates[] = {
        "PS3_DISC.SFB",
        "ps3_disc.sfb",
        NULL
    };

    char sfbPath[1024];
    for (int i = 0; sfbCandidates[i] != NULL; i++) {
        snprintf(sfbPath, sizeof(sfbPath), "%s/%s", discMount, sfbCandidates[i]);
        if (TryGetTitleFromSfb(sfbPath, outTitle, maxLen)) {
            return 1;
        }
    }

    return 0;
}

// Master disc title detection routine (supporting both Blu-ray movies and PS3 games)
void DetectDiscTitle(const char* discMount, DiscType discType, char* outTitle, size_t maxLen) {
    outTitle[0] = '\0';

    if (discType == DISC_TYPE_PS3_GAME) {
        LogMessage("[Title] Attempting to identify PS3 game title...");
        if (TryGetTitleFromPs3Game(discMount, outTitle, maxLen)) {
            LogMessage("[Title] Success: Identified PS3 game title: \"%s\"", outTitle);
            return;
        }
        snprintf(outTitle, maxLen, "PS3_GAME");
        LogMessage("[Title] Could not parse PARAM.SFO. Using default title: \"%s\"", outTitle);
        return;
    }

    // BD Movie
    LogMessage("[Title] Attempting to identify Blu-ray movie title...");
    if (TryGetTitleFromMetaDir(discMount, outTitle, maxLen)) {
        LogMessage("[Title] Success: Identified movie title from BDMV XML: \"%s\"", outTitle);
        return;
    }

    snprintf(outTitle, maxLen, "BD_DISC");
    LogMessage("[Title] No metadata XML detected. Using default title: \"%s\"", outTitle);
}

// Checks if directory exists
int DirectoryExists(const char* path) {
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

// Detects optical disc type and mount presence at /mnt/disc
DiscType FindDiscMount(char* outPath, size_t maxLen) {
    // 1. Check for Blu-ray movie
    if (DirectoryExists("/mnt/disc/BDMV") || DirectoryExists("/mnt/disc/bdmv")) {
        strncpy(outPath, "/mnt/disc", maxLen - 1);
        outPath[maxLen - 1] = '\0';
        return DISC_TYPE_BD_MOVIE;
    }
    // 2. Check for PS3 game
    if (DirectoryExists("/mnt/disc/PS3_GAME") || DirectoryExists("/mnt/disc/ps3_game") ||
        access("/mnt/disc/PS3_DISC.SFB", F_OK) == 0 || access("/mnt/disc/ps3_disc.sfb", F_OK) == 0) {
        strncpy(outPath, "/mnt/disc", maxLen - 1);
        outPath[maxLen - 1] = '\0';
        return DISC_TYPE_PS3_GAME;
    }
    return DISC_TYPE_UNKNOWN;
}

static int IsMountPoint(const char* path) {
    struct stat st1, st2;
    if (stat(path, &st1) != 0) return 0;
    char parent[512];
    snprintf(parent, sizeof(parent), "%s/..", path);
    if (stat(parent, &st2) != 0) return 0;
    return (st1.st_dev != st2.st_dev);
}

// Detects connected USB storage by checking active mount points (/mnt/usb0 and /mnt/usb1)
const char* DetectUsbMount(void) {
    struct statfs *buf = NULL;
    int n = getmntinfo(&buf, MNT_NOWAIT);
    if (n > 0 && buf) {
        for (int i = 0; i < n; i++) {
            if (strcmp(buf[i].f_mntonname, "/mnt/usb0") == 0) return "/mnt/usb0";
        }
        for (int i = 0; i < n; i++) {
            if (strcmp(buf[i].f_mntonname, "/mnt/usb1") == 0) return "/mnt/usb1";
        }
    }
    // Secondary check: verify filesystem device ID differs from parent directory
    if (IsMountPoint("/mnt/usb0")) return "/mnt/usb0";
    if (IsMountPoint("/mnt/usb1")) return "/mnt/usb1";
    return NULL;
}

// Searches for the largest .m2ts file in disc stream folder
static int FindLargestM2ts(const char* streamDir, char* outFileName, size_t maxLen) {
    DIR* dir = opendir(streamDir);
    if (!dir) return 0;

    struct dirent* entry;
    uint64_t maxSz = 0;
    char bestFile[256] = {0};

    while ((entry = readdir(dir)) != NULL) {
        const char* dot = strrchr(entry->d_name, '.');
        if (dot && strcasecmp(dot, ".m2ts") == 0) {
            char fpath[1024];
            snprintf(fpath, sizeof(fpath), "%s/%s", streamDir, entry->d_name);
            struct stat st;
            if (stat(fpath, &st) == 0 && S_ISREG(st.st_mode)) {
                if ((uint64_t)st.st_size > maxSz) {
                    maxSz = (uint64_t)st.st_size;
                    snprintf(bestFile, sizeof(bestFile), "%s", entry->d_name);
                }
            }
        }
    }
    closedir(dir);

    if (bestFile[0] != '\0') {
        snprintf(outFileName, maxLen, "%s", bestFile);
        return 1;
    }
    return 0;
}

// Detects whether the optical movie stream is encrypted (AACS ciphertext) or decrypted (clearstream).
// Returns 1 if encrypted, 0 if clear/unencrypted.
int DetectDiscEncryption(const char* discMount) {
    LogMessage("[Crypt] Analyzing disc video streams for encryption status...");

    const char* subDirs[] = {
        "BDMV/STREAM",
        "bdmv/stream",
        "BDMV/stream",
        "bdmv/STREAM",
        NULL
    };

    char streamDir[1024] = {0};
    int foundDir = 0;
    for (int i = 0; subDirs[i] != NULL; i++) {
        snprintf(streamDir, sizeof(streamDir), "%s/%s", discMount, subDirs[i]);
        if (DirectoryExists(streamDir)) {
            foundDir = 1;
            break;
        }
    }

    if (!foundDir) {
        LogMessage("[Crypt] [WARN] Stream folder not found at %s. Checking AACS directory existence.", discMount);
        char aacsDir[1024];
        snprintf(aacsDir, sizeof(aacsDir), "%s/AACS", discMount);
        if (DirectoryExists(aacsDir)) return 1;
        snprintf(aacsDir, sizeof(aacsDir), "%s/aacs", discMount);
        if (DirectoryExists(aacsDir)) return 1;
        return 0;
    }

    char targetFile[256] = {0};
    if (!FindLargestM2ts(streamDir, targetFile, sizeof(targetFile))) {
        LogMessage("[Crypt] [WARN] No .m2ts stream files found in %s", streamDir);
        return 0;
    }

    char fullStreamPath[1024];
    snprintf(fullStreamPath, sizeof(fullStreamPath), "%s/%s", streamDir, targetFile);

    struct stat st;
    if (stat(fullStreamPath, &st) != 0) {
        LogMessage("[Crypt] stat(%s) failed: %s", fullStreamPath, strerror(errno));
        return 0;
    }

    LogMessage("[Crypt] Testing primary movie stream: %s (size: %.2f GB)",
               targetFile, (double)st.st_size / (1024.0 * 1024.0 * 1024.0));

    int fd = open(fullStreamPath, O_RDONLY);
    if (fd < 0) {
        LogMessage("[Crypt] open(%s) failed: %s", fullStreamPath, strerror(errno));
        return 0;
    }

    // Sample 512 KB of the stream (2730 M2TS packets)
    const size_t sampleCap = 512 * 1024;
    uint8_t* sample = (uint8_t*)malloc(sampleCap);
    if (!sample) {
        close(fd);
        LogMessage("[Crypt] malloc failed for stream analysis buffer");
        return 0;
    }

    ssize_t bytesRead = read(fd, sample, sampleCap);
    close(fd);

    if (bytesRead < 192) {
        free(sample);
        LogMessage("[Crypt] Error: Read fewer than 192 bytes from stream!");
        return 0;
    }

    int numPackets = (int)(bytesRead / 192);
    int validSync = 0;
    int clearTsc = 0;

    for (int p = 0; p < numPackets; p++) {
        const uint8_t* pkt = sample + (p * 192);
        // BD M2TS packet: 4 bytes ATS + 188 bytes MPEG-2 TS
        // Byte 4: Sync byte (0x47)
        // Byte 7: [7:6] transport_scrambling_control (00 = clear)
        if (pkt[4] == 0x47) {
            validSync++;
            uint8_t tsc = (pkt[7] >> 6) & 0x03;
            if (tsc == 0x00) {
                clearTsc++;
            }
        }
    }

    free(sample);

    double syncPct = (numPackets > 0) ? ((double)validSync * 100.0 / numPackets) : 0.0;
    double clearPct = (validSync > 0) ? ((double)clearTsc * 100.0 / validSync) : 0.0;

    LogMessage("[Crypt] M2TS packet analysis (%d packets sampled):", numPackets);
    LogMessage("[Crypt]   Sync byte 0x47 match: %d / %d (%.1f%%)", validSync, numPackets, syncPct);
    LogMessage("[Crypt]   Scrambling bit == 0:  %d / %d (%.1f%%)", clearTsc, validSync, clearPct);

    if (syncPct >= 90.0 && clearPct >= 90.0) {
        LogMessage("[Crypt] Stream verdict: CLEAR / UNENCRYPTED (Valid MPEG-2 TS)");
        return 0; // Clear
    } else {
        LogMessage("[Crypt] Stream verdict: SCRAMBLED / ENCRYPTED CIPHERTEXT (AACS DRM)");
        return 1; // Encrypted
    }
}

// Calculates total directory size recursively
static uint64_t CalculateDirectorySize(const char* dirPath) {
    DIR* dir = opendir(dirPath);
    if (!dir) return 0;

    uint64_t total = 0;
    struct dirent* entry;
    char path[1024];
    struct stat st;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", dirPath, entry->d_name);
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                total += CalculateDirectorySize(path);
            } else if (S_ISREG(st.st_mode)) {
                total += (uint64_t)st.st_size;
            }
        }
    }

    closedir(dir);
    return total;
}

// Statistics and transfer measurement structure
typedef struct {
    uint64_t totalBytesCopied;
    uint64_t lastReportedBytes;
    uint64_t totalDiscBytes;
    struct timespec startTime;
    struct timespec lastReportTime;
} CopyStats;

// Reports copy progress via notification and updates real-time web monitor
static void UpdateProgressReport(CopyStats* stats) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double totalTime = (double)(now.tv_sec - stats->startTime.tv_sec) + 
                       (double)(now.tv_nsec - stats->startTime.tv_nsec) / 1e9;
    double timeDelta = (double)(now.tv_sec - stats->lastReportTime.tv_sec) + 
                       (double)(now.tv_nsec - stats->lastReportTime.tv_nsec) / 1e9;
    uint64_t bytesDelta = stats->totalBytesCopied - stats->lastReportedBytes;

    double currentSpeedMBs = (timeDelta > 0.05) ? ((double)bytesDelta / (1024.0 * 1024.0)) / timeDelta : 0.0;
    double avgSpeedMBs = (totalTime > 0.05) ? ((double)stats->totalBytesCopied / (1024.0 * 1024.0)) / totalTime : 0.0;

    int elapsedSec = (int)totalTime;
    uint64_t remainingBytes = (stats->totalDiscBytes > stats->totalBytesCopied) ?
                              (stats->totalDiscBytes - stats->totalBytesCopied) : 0;
    int etaSec = (currentSpeedMBs > 0.1) ? (int)(((double)remainingBytes / (1024.0 * 1024.0)) / currentSpeedMBs) : 
                 (avgSpeedMBs > 0.1 ? (int)(((double)remainingBytes / (1024.0 * 1024.0)) / avgSpeedMBs) : 0);

    // Update real-time Web UI state on every single data block
    WebUpdateProgress(stats->totalBytesCopied, stats->totalDiscBytes,
                      currentSpeedMBs > 0.0 ? currentSpeedMBs : avgSpeedMBs,
                      avgSpeedMBs, elapsedSec, etaSec);

    // Update live USB storage space stats as files are written
    pthread_mutex_lock(&g_webMutex);
    char currentUsb[64];
    snprintf(currentUsb, sizeof(currentUsb), "%s", g_webStatus.usbMount);
    pthread_mutex_unlock(&g_webMutex);
    if (currentUsb[0] == '/') {
        WebUpdateUsbStats(currentUsb);
    }

    // Report progress to PS5 on-screen notification every 1 GB OR every 30 seconds (if at least 50 MB transferred)
    if (bytesDelta >= 1024ULL * 1024ULL * 1024ULL || 
        (timeDelta >= 30.0 && bytesDelta >= 50ULL * 1024ULL * 1024ULL)) {
        
        stats->lastReportedBytes = stats->totalBytesCopied;
        stats->lastReportTime = now;

        char notifyBuffer[128];
        double copiedGB = (double)stats->totalBytesCopied / (1024.0 * 1024.0 * 1024.0);

        if (stats->totalDiscBytes > 0) {
            double totalGB = (double)stats->totalDiscBytes / (1024.0 * 1024.0 * 1024.0);
            int percent = (int)((stats->totalBytesCopied * 100ULL) / stats->totalDiscBytes);
            int etaMin = etaSec / 60;

            if (etaMin > 0) {
                snprintf(notifyBuffer, sizeof(notifyBuffer), "Copy: %d%% (%.1f/%.1f GB) | %.1f MB/s | ETA: %dm", 
                         percent, copiedGB, totalGB, currentSpeedMBs, etaMin);
            } else {
                snprintf(notifyBuffer, sizeof(notifyBuffer), "Copy: %d%% (%.1f/%.1f GB) | %.1f MB/s", 
                         percent, copiedGB, totalGB, currentSpeedMBs);
            }
        } else {
            snprintf(notifyBuffer, sizeof(notifyBuffer), "Copy: %.1f GB | %.1f MB/s", copiedGB, currentSpeedMBs);
        }
        SendNotification(notifyBuffer);
    }
}

// Copies a single file using a direct transfer buffer
static int CopyFile(const char* srcFile, const char* dstFile, char* buffer, size_t bufferSize, CopyStats* stats) {
    int inFd = open(srcFile, O_RDONLY);
    if (inFd < 0) {
        char err[128];
        snprintf(err, sizeof(err), "Read open error: %s", srcFile);
        SendNotification(err);
        LogMessage("[ERROR] Cannot open for reading: %s (errno=%d: %s)", srcFile, errno, strerror(errno));
        return -1;
    }

    int outFd = open(dstFile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (outFd < 0) {
        close(inFd);
        char err[128];
        snprintf(err, sizeof(err), "Write open error: %s", dstFile);
        SendNotification(err);
        LogMessage("[ERROR] Cannot open for writing: %s (errno=%d: %s)", dstFile, errno, strerror(errno));
        return -1;
    }

    ssize_t bytesRead;
    int copyResult = 0;

    while ((bytesRead = read(inFd, buffer, bufferSize)) > 0) {
        ssize_t totalWritten = 0;
        while (totalWritten < bytesRead) {
            ssize_t written = write(outFd, buffer + totalWritten, bytesRead - totalWritten);
            if (written <= 0) {
                LogMessage("[ERROR] USB write error: written=%zd, errno=%d (%s)", written, errno, strerror(errno));
                SendNotification("USB write error (out of space?)");
                copyResult = -1;
                break;
            }
            totalWritten += written;
        }

        if (copyResult != 0) break;

        stats->totalBytesCopied += (uint64_t)bytesRead;
        UpdateProgressReport(stats);
    }

    if (bytesRead < 0) {
        LogMessage("[ERROR] Disc read error on %s: errno=%d (%s)", srcFile, errno, strerror(errno));
        SendNotification("Disc read error!");
        copyResult = -1;
    }

    close(inFd);
    close(outFd);
    return copyResult;
}

// Recursively copies directory tree
static int CopyDirectoryRecursive(const char* srcDir, const char* dstDir, char* buffer, size_t bufferSize, CopyStats* stats) {
    DIR* dir = opendir(srcDir);
    if (!dir) {
        char err[128];
        snprintf(err, sizeof(err), "Cannot open directory: %s", srcDir);
        SendNotification(err);
        LogMessage("[ERROR] Cannot open directory: %s (errno=%d: %s)", srcDir, errno, strerror(errno));
        return -1;
    }

    int mkRes = mkdir(dstDir, 0777);
    if (mkRes != 0 && errno != EEXIST) {
        LogMessage("[WARN] mkdir(%s) returned ret=%d (errno=%d: %s)", dstDir, mkRes, errno, strerror(errno));
    }

    struct dirent* entry;
    char srcPath[1024];
    char dstPath[1024];
    struct stat st;
    int status = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        snprintf(srcPath, sizeof(srcPath), "%s/%s", srcDir, entry->d_name);
        snprintf(dstPath, sizeof(dstPath), "%s/%s", dstDir, entry->d_name);

        if (stat(srcPath, &st) != 0) {
            LogMessage("[WARN] stat(%s) failed (errno=%d: %s)", srcPath, errno, strerror(errno));
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            LogMessage("[Dir] Entering subdirectory: %s -> %s", srcPath, dstPath);
            if (CopyDirectoryRecursive(srcPath, dstPath, buffer, bufferSize, stats) != 0) {
                status = -1;
                break;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (CopyFile(srcPath, dstPath, buffer, bufferSize, stats) != 0) {
                status = -1;
                break;
            }
        }
    }

    closedir(dir);
    return status;
}

// Master disc copying function with space pre-check and status reporting
int CopyDisc(const char* srcDir, const char* dstDir, const char* usbMount, const char* fullTitle) {
    char notifyBuffer[128];
    LogMessage("[CopyDisc] Starting copy procedure from: %s to: %s", srcDir, dstDir);

    // Calculate total disc size
    uint64_t totalDiscBytes = CalculateDirectorySize(srcDir);
    LogMessage("[CopyDisc] Total disc content size: %llu bytes (%.2f GB)",
               (unsigned long long)totalDiscBytes, (double)totalDiscBytes / (1024.0 * 1024.0 * 1024.0));

    if (totalDiscBytes > 0) {
        double totalGB = (double)totalDiscBytes / (1024.0 * 1024.0 * 1024.0);
        snprintf(notifyBuffer, sizeof(notifyBuffer), "\"%s\" (%.2f GB). Starting copy...", fullTitle, totalGB);
    } else {
        snprintf(notifyBuffer, sizeof(notifyBuffer), "\"%s\": Starting disc copy...", fullTitle);
    }
    SendNotification(notifyBuffer);

    WebUpdateDiscInfo(NULL, NULL, NULL, "Directory copy (folder mode)", dstDir, totalDiscBytes);
    WebUpdateState("COPYING", "Copying disc files to USB folder...");

    // Check available space on USB drive
    struct statvfs vfs;
    if (statvfs(usbMount, &vfs) == 0) {
        uint64_t freeBytes = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
        double freeGB = (double)freeBytes / (1024.0 * 1024.0 * 1024.0);
        double needGB = (double)totalDiscBytes / (1024.0 * 1024.0 * 1024.0);
        LogMessage("[CopyDisc] statvfs(%s): free space: %.2f GB (%llu bytes), required: %.2f GB", 
                   usbMount, freeGB, (unsigned long long)freeBytes, needGB);

        if (totalDiscBytes > 0 && freeBytes < totalDiscBytes) {
            snprintf(notifyBuffer, sizeof(notifyBuffer), "Error: Insufficient USB space (Free: %.1f GB, Needed: %.1f GB)", freeGB, needGB);
            SendNotification(notifyBuffer);
            LogMessage("[FATAL] Insufficient space on USB drive!");
            return -1;
        }
    } else {
        LogMessage("[WARN] statvfs(%s) returned error errno=%d (%s)", usbMount, errno, strerror(errno));
    }

    // Allocate 2 MB copy buffer
    size_t bufferSize = 2 * 1024 * 1024;
    char* buffer = (char*)malloc(bufferSize);
    if (!buffer) {
        bufferSize = 64 * 1024; // 64 KB fallback
        buffer = (char*)malloc(bufferSize);
        if (!buffer) {
            SendNotification("Error: Not enough free RAM for copy buffer!");
            LogMessage("[FATAL] malloc failed to allocate memory for copy buffer!");
            return -1;
        }
    }
    LogMessage("[CopyDisc] Allocated copy buffer: %zu bytes (%.1f MB)", 
               bufferSize, (double)bufferSize / (1024.0 * 1024.0));

    CopyStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.totalDiscBytes = totalDiscBytes;
    clock_gettime(CLOCK_MONOTONIC, &stats.startTime);
    stats.lastReportTime = stats.startTime;

    int res = CopyDirectoryRecursive(srcDir, dstDir, buffer, bufferSize, &stats);

    free(buffer);

    if (res == 0) {
        struct timespec endTime;
        clock_gettime(CLOCK_MONOTONIC, &endTime);
        double totalDuration = (double)(endTime.tv_sec - stats.startTime.tv_sec) + 
                               (double)(endTime.tv_nsec - stats.startTime.tv_nsec) / 1e9;
        double avgSpeed = (totalDuration > 0.0) ? 
                          ((double)stats.totalBytesCopied / (1024.0 * 1024.0)) / totalDuration : 0.0;
        double finalGB = (double)stats.totalBytesCopied / (1024.0 * 1024.0 * 1024.0);
        int totalMin = (int)(totalDuration / 60.0);
        int totalSec = (int)totalDuration % 60;

        snprintf(notifyBuffer, sizeof(notifyBuffer), "\"%s\": copied %.2f GB in %dm %ds (avg %.1f MB/s)", 
                 fullTitle, finalGB, totalMin, totalSec, avgSpeed);
        SendNotification(notifyBuffer);
        LogMessage("[CopyDisc] Copy finished successfully: %.2f GB in %dm %ds (avg %.1f MB/s)",
                   finalGB, totalMin, totalSec, avgSpeed);
    } else {
        LogMessage("[ERROR] File copying aborted with error: %d", res);
    }

    return res;
}

// Direct 1:1 bit-perfect optical disc ISO image creation from /dev/cd0
int CopyDiscToIso(const char* discMount, const char* dstIsoPath, const char* usbMount, const char* fullTitle) {
    char notifyBuffer[128];
    LogMessage("[CopyIso] Starting bit-perfect optical disc clone to ISO...");
    LogMessage("[CopyIso]   Source device: /dev/cd0");
    LogMessage("[CopyIso]   Disc mount:    %s", discMount);
    LogMessage("[CopyIso]   Destination:   %s", dstIsoPath);

    // 1. Open optical device /dev/cd0
    int inFd = open("/dev/cd0", O_RDONLY);
    if (inFd < 0) {
        char err[128];
        snprintf(err, sizeof(err), "Cannot open optical device /dev/cd0!");
        SendNotification(err);
        LogMessage("[CopyIso] [FATAL] Cannot open /dev/cd0 (errno=%d: %s)", errno, strerror(errno));
        return -1;
    }

    // 2. Determine disc size from filesystem geometry
    uint64_t totalDiscBytes = 0;
    struct statvfs discVfs;
    if (statvfs(discMount, &discVfs) == 0 && discVfs.f_blocks > 0 && discVfs.f_frsize > 0) {
        totalDiscBytes = (uint64_t)discVfs.f_blocks * (uint64_t)discVfs.f_frsize;
        LogMessage("[CopyIso] statvfs(%s): blocks=%llu, frsize=%llu -> total: %llu bytes (%.2f GB)",
                   discMount, (unsigned long long)discVfs.f_blocks, (unsigned long long)discVfs.f_frsize,
                   (unsigned long long)totalDiscBytes, (double)totalDiscBytes / (1024.0 * 1024.0 * 1024.0));
    } else {
        LogMessage("[CopyIso] [WARN] statvfs(%s) failed (errno=%d: %s), falling back to directory scan",
                   discMount, errno, strerror(errno));
        totalDiscBytes = CalculateDirectorySize(discMount);
    }

    if (totalDiscBytes > 0) {
        double totalGB = (double)totalDiscBytes / (1024.0 * 1024.0 * 1024.0);
        snprintf(notifyBuffer, sizeof(notifyBuffer), "\"%s\" (%.2f GB). Creating ISO...", fullTitle, totalGB);
    } else {
        snprintf(notifyBuffer, sizeof(notifyBuffer), "\"%s\": Creating ISO...", fullTitle);
    }
    SendNotification(notifyBuffer);

    WebUpdateDiscInfo(NULL, NULL, NULL, "Direct 1:1 ISO clone (/dev/cd0) [DEFAULT]", dstIsoPath, totalDiscBytes);
    WebUpdateState("COPYING", "Cloning optical disc sectors to ISO...");

    // 3. Pre-check free space on USB drive
    struct statvfs usbVfs;
    if (statvfs(usbMount, &usbVfs) == 0) {
        uint64_t freeBytes = (uint64_t)usbVfs.f_bavail * (uint64_t)usbVfs.f_frsize;
        double freeGB = (double)freeBytes / (1024.0 * 1024.0 * 1024.0);
        double needGB = (double)totalDiscBytes / (1024.0 * 1024.0 * 1024.0);
        LogMessage("[CopyIso] statvfs(%s): free space: %.2f GB (%llu bytes), required: %.2f GB",
                   usbMount, freeGB, (unsigned long long)freeBytes, needGB);

        if (totalDiscBytes > 0 && freeBytes < totalDiscBytes) {
            snprintf(notifyBuffer, sizeof(notifyBuffer), "Error: Insufficient USB space for ISO (Free: %.1f GB, Needed: %.1f GB)", freeGB, needGB);
            SendNotification(notifyBuffer);
            LogMessage("[CopyIso] [FATAL] Insufficient space on USB drive for ISO file!");
            close(inFd);
            return -1;
        }
    } else {
        LogMessage("[CopyIso] [WARN] statvfs(%s) returned error errno=%d (%s)", usbMount, errno, strerror(errno));
    }

    // 4. Open destination ISO file on USB
    int outFd = open(dstIsoPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (outFd < 0) {
        char err[128];
        snprintf(err, sizeof(err), "Write open error: %s", dstIsoPath);
        SendNotification(err);
        LogMessage("[CopyIso] [FATAL] Cannot open destination ISO: %s (errno=%d: %s)", dstIsoPath, errno, strerror(errno));
        close(inFd);
        return -1;
    }

    // 5. Allocate 4 MB streaming buffer (2048 sectors of 2048 bytes)
    size_t bufferSize = 4 * 1024 * 1024;
    char* buffer = (char*)malloc(bufferSize);
    if (!buffer) {
        bufferSize = 1024 * 1024; // 1 MB fallback
        buffer = (char*)malloc(bufferSize);
        if (!buffer) {
            bufferSize = 64 * 1024;
            buffer = (char*)malloc(bufferSize);
            if (!buffer) {
                SendNotification("Error: Not enough free RAM for ISO buffer!");
                LogMessage("[CopyIso] [FATAL] malloc failed for ISO copy buffer!");
                close(inFd);
                close(outFd);
                unlink(dstIsoPath);
                return -1;
            }
        }
    }
    LogMessage("[CopyIso] Allocated streaming buffer: %zu bytes (%.1f MB)",
               bufferSize, (double)bufferSize / (1024.0 * 1024.0));

    CopyStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.totalDiscBytes = totalDiscBytes;
    clock_gettime(CLOCK_MONOTONIC, &stats.startTime);
    stats.lastReportTime = stats.startTime;

    int copyResult = 0;
    uint64_t remaining = totalDiscBytes;

    while (totalDiscBytes == 0 || remaining > 0) {
        size_t toRead = bufferSize;
        if (totalDiscBytes > 0 && toRead > remaining) {
            toRead = (size_t)remaining;
        }

        ssize_t bytesRead = read(inFd, buffer, toRead);
        if (bytesRead <= 0) {
            if (bytesRead == 0) {
                // Reached end of device/media
                LogMessage("[CopyIso] Reached end-of-file on /dev/cd0 (read returned 0)");
                break;
            }
            // Check if we already copied virtually all data sectors
            if (totalDiscBytes > 0 && stats.totalBytesCopied >= totalDiscBytes - (32 * 2048)) {
                LogMessage("[CopyIso] Read reached disc boundary with errno=%d, all sectors transferred", errno);
                break;
            }
            LogMessage("[CopyIso] [ERROR] Disc read error on /dev/cd0 at offset %llu: errno=%d (%s)",
                       (unsigned long long)stats.totalBytesCopied, errno, strerror(errno));
            SendNotification("Disc read error on /dev/cd0!");
            copyResult = -1;
            break;
        }

        ssize_t totalWritten = 0;
        while (totalWritten < bytesRead) {
            ssize_t written = write(outFd, buffer + totalWritten, bytesRead - totalWritten);
            if (written <= 0) {
                LogMessage("[CopyIso] [ERROR] USB write error: written=%zd, errno=%d (%s)",
                           written, errno, strerror(errno));
                SendNotification("USB write error (out of space?)");
                copyResult = -1;
                break;
            }
            totalWritten += written;
        }

        if (copyResult != 0) break;

        stats.totalBytesCopied += (uint64_t)bytesRead;
        if (totalDiscBytes > 0 && remaining >= (uint64_t)bytesRead) {
            remaining -= (uint64_t)bytesRead;
        } else {
            remaining = 0;
        }
        UpdateProgressReport(&stats);
    }

    free(buffer);
    close(inFd);

    if (copyResult == 0) {
        fsync(outFd);
    }
    close(outFd);

    if (copyResult == 0) {
        struct timespec endTime;
        clock_gettime(CLOCK_MONOTONIC, &endTime);
        double totalDuration = (double)(endTime.tv_sec - stats.startTime.tv_sec) + 
                               (double)(endTime.tv_nsec - stats.startTime.tv_nsec) / 1e9;
        double avgSpeed = (totalDuration > 0.0) ? 
                          ((double)stats.totalBytesCopied / (1024.0 * 1024.0)) / totalDuration : 0.0;
        double finalGB = (double)stats.totalBytesCopied / (1024.0 * 1024.0 * 1024.0);
        int totalMin = (int)(totalDuration / 60.0);
        int totalSec = (int)totalDuration % 60;

        snprintf(notifyBuffer, sizeof(notifyBuffer), "\"%s\": ISO created (%.2f GB) in %dm %ds (avg %.1f MB/s)", 
                 fullTitle, finalGB, totalMin, totalSec, avgSpeed);
        SendNotification(notifyBuffer);
        LogMessage("[CopyIso] ISO creation finished successfully: %.2f GB in %dm %ds (avg %.1f MB/s)",
                   finalGB, totalMin, totalSec, avgSpeed);
    } else {
        LogMessage("[CopyIso] [ERROR] ISO creation aborted with error: %d", copyResult);
        unlink(dstIsoPath);
    }

    return copyResult;
}

// Checks if dont_make_iso trigger file exists on USB root (/mnt/usb0 or /mnt/usb1)
int CheckDontMakeIsoTrigger(const char* usbMount) {
    char path[512];
    snprintf(path, sizeof(path), "%s/dont_make_iso", usbMount);
    if (access(path, F_OK) == 0) {
        LogMessage("[Main] Detected trigger file: %s (folder mode enabled)", path);
        return 1;
    }
    if (access("/mnt/usb0/dont_make_iso", F_OK) == 0) {
        LogMessage("[Main] Detected trigger file: /mnt/usb0/dont_make_iso (folder mode enabled)");
        return 1;
    }
    if (access("/mnt/usb1/dont_make_iso", F_OK) == 0) {
        LogMessage("[Main] Detected trigger file: /mnt/usb1/dont_make_iso (folder mode enabled)");
        return 1;
    }
    return 0;
}

int main(void) {
    char discPath[1024] = {0};

    // Early USB detection to initialize file logging immediately
    const char* earlyUsb = DetectUsbMount();
    if (earlyUsb) {
        InitLog(earlyUsb);
        WebUpdateUsbStats(earlyUsb);
    }

    // Start background Web Server status monitor on port 8081
    DetectConsoleIp(g_consoleIp, sizeof(g_consoleIp));
    pthread_create(&g_webThread, NULL, WebServerThread, NULL);

    LogMessage("==================================================");
    LogMessage("=== BDclone Daemon for PS5 started ===");
    LogMessage("==================================================");
    if (earlyUsb) {
        LogMessage("[Main] Detected USB drive: %s (BDclone.log initialized)", earlyUsb);
    } else {
        LogMessage("[Main] USB drive not mounted yet (buffering logs in RAM)");
    }

    char webNotify[256];
    if (strcmp(g_consoleIp, "127.0.0.1") != 0) {
        snprintf(webNotify, sizeof(webNotify), "BDclone Web: http://%s:%d", g_consoleIp, g_webPort);
    } else {
        snprintf(webNotify, sizeof(webNotify), "BDclone Web: port %d", g_webPort);
    }
    SendNotification(webNotify);

    // 1. Wait for optical disc (Blu-ray Movie or PS3 Game) to be inserted and mounted at /mnt/disc
    SendNotification("BDclone Daemon: Waiting for disc insertion...");
    LogMessage("[Main] Waiting for disc mount at /mnt/disc...");
    WebUpdateState("WAITING_DISC", "Waiting for optical disc insertion...");

    DiscType discType = DISC_TYPE_UNKNOWN;
    while ((discType = FindDiscMount(discPath, sizeof(discPath))) == DISC_TYPE_UNKNOWN) {
        const char* usb = DetectUsbMount();
        if (usb) {
            InitLog(usb);
            WebUpdateUsbStats(usb);
        } else {
            WebSetUsbDisconnected();
        }
        sleep(2); // Poll every 2 seconds
    }

    const char* typeDesc = (discType == DISC_TYPE_PS3_GAME) ? "PS3 Game disc" : "Blu-ray Movie disc";
    LogMessage("[Main] Success: Detected mounted %s at: %s", typeDesc, discPath);
    WebUpdateState("ANALYZING", "Optical disc detected. Reading metadata...");

    // 2. Detect connected USB storage (/mnt/usb0 or /mnt/usb1)
    const char* usbMount = DetectUsbMount();
    if (!usbMount) {
        SendNotification("Please connect USB drive (/mnt/usb0)!");
        LogMessage("[Main] Optical disc detected, but no USB storage found. Waiting for USB drive...");
        WebUpdateState("WAITING_USB", "Optical disc detected! Please connect USB storage drive...");
        while ((usbMount = DetectUsbMount()) == NULL) {
            sleep(2);
        }
        LogMessage("[Main] Success: USB drive detected at %s", usbMount);
        SendNotification("USB storage connected!");
        WebUpdateState("ANALYZING", "USB drive detected. Reading metadata...");
    }

    InitLog(usbMount); // Ensure log file is open
    WebUpdateUsbStats(usbMount);

    // 3. Detect disc title (from BDMV XML metadata or PS3 PARAM.SFO)
    char discTitle[256] = {0};
    DetectDiscTitle(discPath, discType, discTitle, sizeof(discTitle));

    // 4. Automated stream encryption verification (applies to Blu-ray Movies)
    int isEncrypted = 0;
    if (discType == DISC_TYPE_BD_MOVIE) {
        isEncrypted = DetectDiscEncryption(discPath);
        char statusNotify[256];
        if (isEncrypted) {
            snprintf(statusNotify, sizeof(statusNotify), "Movie: \"%s\" [ENCRYPTED - AACS]", discTitle);
        } else {
            snprintf(statusNotify, sizeof(statusNotify), "Movie: \"%s\" [CLEAR - UNENCRYPTED]", discTitle);
        }
        SendNotification(statusNotify);
    } else {
        char statusNotify[256];
        snprintf(statusNotify, sizeof(statusNotify), "PS3 Game: \"%s\"", discTitle);
        SendNotification(statusNotify);
        LogMessage("[Main] Media type: PS3 Game");
    }

    // Tag destination name with .encrypted if disc streams are encrypted
    char fullTitle[300];
    if (isEncrypted) {
        snprintf(fullTitle, sizeof(fullTitle), "%s.encrypted", discTitle);
    } else {
        snprintf(fullTitle, sizeof(fullTitle), "%s", discTitle);
    }

    // 5. Determine operation mode (ISO is DEFAULT, folder mode if 'dont_make_iso' trigger is present)
    int isFolderMode = CheckDontMakeIsoTrigger(usbMount);
    int isIsoMode = !isFolderMode;

    if (isFolderMode) {
        LogMessage("[Main] >>> 'dont_make_iso' trigger detected on USB root! <<<");
        LogMessage("[Main] Mode: Standard directory copy (folder mode)");
    } else {
        LogMessage("[Main] Mode: Direct bit-perfect ISO clone (/dev/cd0) [DEFAULT]");
    }

    char flagCopying[512];
    char flagFinished[512];
    char usbDstPath[512];

    snprintf(flagCopying, sizeof(flagCopying), "%s/%s.copying", usbMount, fullTitle);
    snprintf(flagFinished, sizeof(flagFinished), "%s/%s.finished", usbMount, fullTitle);

    if (isIsoMode) {
        snprintf(usbDstPath, sizeof(usbDstPath), "%s/%s.iso", usbMount, fullTitle);
    } else {
        snprintf(usbDstPath, sizeof(usbDstPath), "%s/%s", usbMount, fullTitle);
    }

    const char* encStatus;
    if (discType == DISC_TYPE_PS3_GAME) {
        encStatus = "PS3 Disc Encryption (DKEY / 3k3y / Redump)";
    } else if (isEncrypted) {
        encStatus = "ENCRYPTED (AACS - tagged .encrypted)";
    } else {
        encStatus = "CLEAR / UNENCRYPTED";
    }

    LogMessage("[Main] Path configuration:");
    LogMessage("[Main]   Media type:    %s", typeDesc);
    LogMessage("[Main]   Title:         %s", discTitle);
    LogMessage("[Main]   Encryption:    %s", encStatus);
    LogMessage("[Main]   Output name:   %s", fullTitle);
    LogMessage("[Main]   Copying flag:  %s", flagCopying);
    LogMessage("[Main]   Finished flag: %s", flagFinished);
    LogMessage("[Main]   Destination:   %s (%s)", usbDstPath, isIsoMode ? "ISO image file" : "directory");

    WebUpdateDiscInfo(discTitle, typeDesc, encStatus, isIsoMode ? "Direct ISO clone (/dev/cd0) [DEFAULT]" : "Directory copy (folder mode)", usbDstPath, 0);

    // 6. Create empty <fullTitle>.copying flag file on USB
    int flagFd = open(flagCopying, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (flagFd >= 0) {
        close(flagFd);
        LogMessage("[Main] Created empty flag file: %s (fd=%d)", flagCopying, flagFd);
    } else {
        LogMessage("[FATAL] Cannot create flag %s (errno=%d: %s)", flagCopying, errno, strerror(errno));
        SendNotification("Error: Failed to create flag file on USB!");
        CloseLog();
        return -1;
    }

    int copyResult;
    if (isIsoMode) {
        // 7a. Direct optical disc sector-to-ISO clone from /dev/cd0 (DEFAULT)
        LogMessage("[Main] Starting optical disc ISO clone: /dev/cd0 -> %s", usbDstPath);
        copyResult = CopyDiscToIso(discPath, usbDstPath, usbMount, fullTitle);
    } else {
        // 7b. Standard directory copy
        int mkRes = mkdir(usbDstPath, 0777);
        if (mkRes == 0) {
            LogMessage("[Main] Created target folder: %s", usbDstPath);
        } else if (errno == EEXIST) {
            LogMessage("[Main] Target folder already exists: %s", usbDstPath);
        } else {
            LogMessage("[Main] [WARN] mkdir(%s) failed: errno=%d (%s)", usbDstPath, errno, strerror(errno));
        }
        copyResult = CopyDisc(discPath, usbDstPath, usbMount, fullTitle);
    }

    if (copyResult == 0) {
        WebSetComplete(1, "Clone completed successfully!");

        // 8. Success: rename flag to <fullTitle>.finished
        int renRet = rename(flagCopying, flagFinished);
        if (renRet == 0) {
            LogMessage("[Main] Renamed flag: %s -> %s (SUCCESS)", flagCopying, flagFinished);
        } else {
            LogMessage("[Main] [WARN] rename(%s, %s) failed: errno=%d (%s)", 
                       flagCopying, flagFinished, errno, strerror(errno));
            unlink(flagCopying);
            int finFd = open(flagFinished, O_WRONLY | O_CREAT | O_TRUNC, 0777);
            if (finFd >= 0) close(finFd);
            LogMessage("[Main] Fallback: Created directly %s (finFd=%d)", flagFinished, finFd);
        }

        const char* mediaName = (discType == DISC_TYPE_PS3_GAME) ? "PS3 Game" : "Movie";
        char finishedNotify[256];
        if (isIsoMode) {
            snprintf(finishedNotify, sizeof(finishedNotify), "%s \"%s\": ISO copy completed successfully!", mediaName, fullTitle);
        } else {
            snprintf(finishedNotify, sizeof(finishedNotify), "%s \"%s\": copy completed successfully!", mediaName, fullTitle);
        }
        SendNotification(finishedNotify);

        LogMessage("==================================================");
        LogMessage("=== BDclone Completed with Full Success! ===");
        LogMessage("==================================================");
        LogMessage("[Web] Keeping live web status page active for 60 seconds before exit...");
        sleep(60);
    } else {
        WebSetComplete(0, "Clone aborted with error!");
        SendNotification(isIsoMode ? "Error: Disc ISO copy failed!" : "Error: Disc copy failed!");
        LogMessage("[FATAL] Copy failed with error: %d. Removing copying flag.", copyResult);
        unlink(flagCopying);
        if (isIsoMode) {
            unlink(usbDstPath);
        }
        sleep(15);
    }

    // Stop web server thread gracefully
    g_webServerRunning = 0;
    pthread_join(g_webThread, NULL);

    CloseLog();
    return copyResult;
}
