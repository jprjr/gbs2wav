.PHONY: all clean release

include thirdparty/SameBoy/version.mk

SRCS = src/gbs2wav.c
OBJS = $(SRCS:.c=.o)

GB_SRCS = \
	thirdparty/SameBoy/Core/apu.c \
	thirdparty/SameBoy/Core/camera.c \
	thirdparty/SameBoy/Core/cheats.c \
	thirdparty/SameBoy/Core/debugger.c \
	thirdparty/SameBoy/Core/display.c \
	thirdparty/SameBoy/Core/gb.c \
	thirdparty/SameBoy/Core/joypad.c \
	thirdparty/SameBoy/Core/mbc.c \
	thirdparty/SameBoy/Core/memory.c \
	thirdparty/SameBoy/Core/printer.c \
	thirdparty/SameBoy/Core/random.c \
	thirdparty/SameBoy/Core/rewind.c \
	thirdparty/SameBoy/Core/rumble.c \
	thirdparty/SameBoy/Core/save_state.c \
	thirdparty/SameBoy/Core/sgb.c \
	thirdparty/SameBoy/Core/sm83_cpu.c \
	thirdparty/SameBoy/Core/sm83_disassembler.c \
	thirdparty/SameBoy/Core/symbol_hash.c \
	thirdparty/SameBoy/Core/timing.c \
	thirdparty/SameBoy/Core/workboy.c
GB_OBJS = $(GB_SRCS:.c=.o)


EXE=

CFLAGS = -I. -Wall -Wextra -fPIC -O3 -g
LDFLAGS = -lm

GB_CFLAGS = -g -O3 -Ithirdparty/SameBoy -Wall -Wextra -fPIC -std=gnu11 -D_GNU_SOURCE -DGB_INTERNAL -DGB_VERSION='"$(VERSION)"' -D_USE_MATH_DEFINES

all: gbs2wav$(EXE)

gbs2wav$(EXE): $(OBJS) $(GB_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

thirdparty/SameBoy/%.o: thirdparty/SameBoy/%.c
	$(CC) -o $@ -c $(GB_CFLAGS) $<

%.o: %.c
	$(CC) -o $@ -c $(CFLAGS) $<

clean:
	rm -f gbs2wav gbs2wav.exe $(OBJS) $(GB_OBJS)
