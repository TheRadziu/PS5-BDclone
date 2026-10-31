# Default PS5 Payload SDK path in WSL / Linux
PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk

TARGET := BDclone.elf
SRCS   := main.c
OBJS   := $(SRCS:.c=.o)

# Toolchain environment configuration
ifneq ($(wildcard $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk),)
    # 1. Mode with official ps5-payload-sdk
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
    CFLAGS  += -O2 -Wall -std=c11
    LDFLAGS += -Wl,--unresolved-symbols=ignore-all
else
    # 2. Fallback mode: standard Clang in WSL
    # Required packages: sudo apt install clang lld llvm make
    CC      := clang
    CFLAGS  := --target=x86_64-pc-freebsd12-elf -fPIC -O2 -Wall -std=c11
    LDFLAGS := --target=x86_64-pc-freebsd12-elf -fuse-ld=lld -shared -Wl,--unresolved-symbols=ignore-all
endif

# Main default target
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

# Helper target: send payload over network to PS5 (elfldr on port 9021)
# Usage example: make send PS5_HOST=192.168.1.150
PS5_HOST ?= 192.168.1.100
PS5_PORT ?= 9021

send: $(TARGET)
	@echo "Sending $(TARGET) to $(PS5_HOST):$(PS5_PORT)..."
	nc -q 1 -w 3 $(PS5_HOST) $(PS5_PORT) < $(TARGET) || nc -w 3 $(PS5_HOST) $(PS5_PORT) < $(TARGET)

.PHONY: all clean send
