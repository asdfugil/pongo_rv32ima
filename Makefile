PONGO_SRC ?= ../PongoOS
CC ?= clang -arch arm64
CFLAGS +=  -I$(PONGO_SRC)/src/drivers -I$(PONGO_SRC)/include -I$(PONGO_SRC)/src/kernel
CFLAGS +=-I$(PONGO_SRC)/newlib/aarch64-none-darwin/include -Iinclude -I$(PONGO_SRC)/apple-include
CFLAGS += -Os -ffreestanding -nostdlib -U__nonnull -DTARGET_OS_OSX=0 -DTARGET_OS_MACCATALYST=0
CFLAGS += -D_GNU_SOURCE -DDER_TAG_SIZE=8 -fno-blocks
LDFLAGS += -Wl,-kext -Wl,-dead_strip -flto=thin -fno-stack-protector
SRC = $(wildcard src/*.c)
OBJS = $(patsubst src/%.c, obj/%.o, $(SRC))
EXE = mini-rv32ima

$(warning $(OBJS))

all: $(EXE)

obj/%.o: src/%.c
	$(CC) -c -nostdlibinc $(CFLAGS) -o $@ $<

$(EXE): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $(EXE)

clean:
	rm -f set_rootdev obj/*.o

.PHONY: all clean
