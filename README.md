# BDclone for PS5

## Actual human note
Its completely vibecoded for my own needs but it works. You're welcome to take this code and adapt it.
As I don't have a PC Blu-ray drive I previously used a PS3 with multiMAN to create ISOs (~9 MB/s) and transferred them over ethernet to PC to decrypt with MakeMKV.
This PS5 payload dumps optical discs directly to external USB storage as either a 1:1 bit-perfect UDF 2.50 ISO image (default) or a directory dump.

---

## Key Features

* **Dual Media Support (Blu-ray Movies & PS3 Games)**:  
  * **Blu-ray Movies**: Automatically detects `BDMV/` structure, parses XML metadata for formatted movie title, detects AACS encryption, and tags accordingly.
  * **PS3 Games**: Automatically detects `PS3_GAME/` and `PS3_DISC.SFB`, parses `PARAM.SFO` to extract official game `TITLE` and `TITLE_ID` (e.g. `The Last of Us [BCES01584]`), and dumps 1:1 playable images for RPCS3 / PS3 / webMAN.
* **Direct 1:1 Bit-Perfect ISO Creation (`/dev/cd0`) [DEFAULT]**:  
  By default, BDclone reads sectors directly from the optical drive block device `/dev/cd0`. This preserves the authentic factory **UDF 2.50 filesystem** (Volume Recognition Sequence `BEA01`, Anchor Volume Descriptor Pointer at sector 256, Logical Volume Integrity Descriptors, and Metadata Partition). The resulting `.iso` is immediately recognized by **MakeMKV**, **VLC**, Windows Explorer, and ImgBurn as a genuine Blu-ray Video disc.
* **Automated Disc Encryption Detection & Tagging**:  
  Prior to copying, BDclone automatically inspects the primary feature movie stream (`BDMV/STREAM/*.m2ts`) by sampling 192-byte M2TS packets (checking sync byte `0x47` at offset 4 and `transport_scrambling_control` bits).
  * **Commercial Encrypted Discs (AACS)**: The output ISO or directory is automatically tagged with `.encrypted` (e.g. `<Title>.encrypted.iso` or `<Title>.encrypted/`).
  * **Unencrypted / Clear Discs**: The output is named cleanly without any extra suffixes (e.g. `<Title>.iso` or `<Title>/`).
* **Dual Operation Modes**:
  * **Direct ISO Mode (DEFAULT)**: Automatically creates a 1:1 bit-perfect optical disc image: `<USB>/<Title>[.encrypted].iso`.
  * **Folder Mode (`dont_make_iso` Trigger)**: Creates `<USB>/<Title>[.encrypted]/` directory tree only when an empty trigger file named `dont_make_iso` is placed on the USB root (`/mnt/usb0/dont_make_iso`).
* **Automatic Movie Title Detection**:  
  Parses BDMV XML metadata (`BDMV/META/DL/bdmt_*.xml`) for the disc's official movie name, automatically sanitizing illegal filename characters for exFAT/FAT32/NTFS. Defaults cleanly to `BD_DISC` if metadata is not present.
* **MakeMKV PC Integration**:  
  ISOs created from commercial discs (`<Title>.encrypted.iso`) contain all genuine factory AACS structures intact (`AACS/MKB_RO.inf`, unit keys, etc.). MakeMKV on PC opens the ISO directly in disc access mode, identifies the keys via its online/KEYDB database, and decrypts/rips the movie cleanly.
* **Safety & Space Pre-Check**:  
  Verifies available space on the USB drive via `statvfs` prior to copying. If the destination lacks sufficient space, the process halts safely without leaving broken, oversized files.
* **Progress Tracking & PS5 On-Screen Notifications**:  
  Displays native on-screen notifications for disc detection, encryption status, transfer progress (percentage, transferred GB, speed in MB/s, and ETA), and final completion.
* **Embedded Real-Time Web Monitor (`http://<PS5_IP>:8081`)**:  
  Runs a built-in lightweight HTTP status server on port `8081`. Open it in any web browser (phone, PC, or tablet on your local network) to monitor the copy process in real time on a PlayStation-styled dashboard:
  * Live percentage and animated progress bar
  * Data transferred (`X.XX / Y.YY GB`) and remaining data
  * Real-time transfer speed (`XX.X MB/s`) and average speed
  * Elapsed live timer and Estimated Time Remaining (ETA)
  * Live USB storage capacity monitor: used space, free space in GB, and capacity gauge bar
  * Disc title, media type, DRM encryption status, and destination path
  * Completely automated and buttonless — pure live monitor.
* **Copy State Flag Files**:  
  Creates `<Title>[.encrypted].copying` on the USB root during transfer, and renames it to `<Title>[.encrypted].finished` on success.
* **Persistent USB Logging**:  
  Detailed logs with timestamps are written to `/mnt/usb0/BDclone.log`.

---

## Technical Architecture & Findings

### 1. Direct Mount vs Block Device (`/dev/cd0`)
Hardware tests comparing `/dev/cd0` against `/mnt/disc` revealed that both sources return **100% byte-for-byte identical data** across all sectors:
* `index.bdmv` (LBA 1344): 100% exact match
* `MovieObject.bdmv` (LBA 1345): 100% exact match
* Main Movie Stream `00023.m2ts` (LBA 51648): 100% exact match

There is no SATA bus scrambling. Cloning `/dev/cd0` directly gives a 1:1 replica of the physical disc with all factory UDF 2.50 structures.

### 2. Optical Drive Performance
Streaming sequential 4 MB blocks directly from `/dev/cd0` engages the optical drive's CAV mode, achieving ~8.5 MB/s transfer speed without per-file seek latency or VFS overhead.

---

## Project Structure

```text
BDmovie/
├── main.c              # Core daemon payload (BDclone.elf)
├── web_ui.h            # Embedded dark-mode web status dashboard UI
├── Makefile            # Build configuration for prospero-clang / Clang
└── README.md           # Documentation
```

---

## Building

### Requirements
* **ps5-payload-sdk** (recommended, located at `/opt/ps5-payload-sdk` in WSL/Linux)  
  *OR* standard Clang toolchain supporting `--target=x86_64-pc-freebsd12-elf`.

### Build Commands

```bash
# Build payload (BDclone.elf)
make clean
make

# Send payload to PS5 (port 9021 by default)
make send PS5_HOST=192.168.1.150
```

---

## Usage Guide

### 1. Preparing the USB Drive
* Format an external USB drive to **exFAT** (required to support files larger than 4 GB).
* Connect the USB drive to any USB port on the PS5.

### 2. Operation Modes

* **Direct ISO Image Mode (DEFAULT)**:  
  No extra files needed! Just insert the disc and run BDclone. It will clone `/dev/cd0` directly into a single UDF 2.50 / ISO optical disc image:
  ```text
  # Blu-ray Movies:
  /mnt/usb0/<MovieTitle>[.encrypted].iso

  # PS3 Games:
  /mnt/usb0/<GameTitle> [<TITLE_ID>].iso
  ```
  *(Note: PS3 game ISOs dumped directly from `/dev/cd0` preserve the standard 1:1 Redump/optical sector encryption. For PC emulation in RPCS3 or decryption via `PS3Dec`, place the matching 32-hex `.dkey` disc key in RPCS3's disc key folder or pass it to `PS3Dec`.)*

* **Folder Mode (`dont_make_iso` Trigger)**:  
  If you want a folder dump instead of an ISO, create an empty file named `dont_make_iso` (no file extension) in the root of your USB drive:
  ```text
  /mnt/usb0/dont_make_iso
  ```
  When this file is present, BDclone will dump the disc as a directory tree:
  ```text
  /mnt/usb0/<MovieTitle>[.encrypted]/
  ├── BDMV/
  │   ├── index.bdmv
  │   ├── MovieObject.bdmv
  │   ├── PLAYLIST/
  │   ├── CLIPINF/
  │   └── STREAM/
  │       └── 00000.m2ts
  └── CERTIFICATE/
  ```

### 3. Running the Payload
1. Ensure your PS5 is running an ELF loader (e.g. `elfldr` listening on port `9021`).
2. Send `BDclone.elf` to the console:
   ```bash
   make send PS5_HOST=<YOUR_PS5_IP>
   ```
3. Notifications will appear on screen:
   > **BDclone Web: http://<PS5_IP>:8081**  
   > **BDclone Daemon: Waiting for disc insertion...**
4. *(Optional)* Open `http://<PS5_IP>:8081` in your browser (phone or PC) to monitor real-time cloning progress.
5. Insert your Blu-ray or PS3 disc into the drive.
6. The daemon will automatically:
   * Detect the disc mount at `/mnt/disc`.
   * Extract the movie/game title.
   * Analyze encryption status.
   * Verify USB storage capacity.
   * Create the `<Title>[.encrypted].copying` flag file on the USB root.
   * Stream data to USB with live updates in the Web UI (every second) and on-screen TV notifications (every 1 GB / 30s).
   * Rename flag to `<Title>[.encrypted].finished` when complete.
7. A completion notification will show the total data copied, elapsed time, and average transfer speed (the Web UI stays active for 60 seconds before clean exit).
