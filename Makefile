CC      := gcc
CFLAGS  := -O2 -g -Wall -Wextra -std=gnu11 \
           $(shell pkg-config --cflags libdrm_amdgpu 2>/dev/null || echo "")
LDFLAGS := $(shell pkg-config --libs libdrm_amdgpu 2>/dev/null || echo "-ldrm_amdgpu")

SRC := src/amdgpu_device.c src/bo.c src/regs.c src/spirv_compile.c src/debugger_main.c
OBJ := $(SRC:.c=.o)

all: hdb

hdb: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) hdb

.PHONY: all clean
