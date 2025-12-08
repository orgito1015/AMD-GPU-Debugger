CC      := gcc
CFLAGS  := -O2 -g -Wall -Wextra -std=gnu11 \
           $(shell pkg-config --cflags libdrm_amdgpu)
LDFLAGS := $(shell pkg-config --libs libdrm_amdgpu)

SRC := src/amdgpu_device.c src/bo.c src/regs.c src/spirv_compile.c src/debugger_main.c
OBJ := $(SRC:.c=.o)

all: hdb

hdb: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

clean:
	rm -f $(OBJ) hdb
