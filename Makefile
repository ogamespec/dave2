# Dangerous Dave 2 -- SDL3 port
# Prefer a self-contained SDL3 under the tree; fall back to the historical
# external prefix.
SDL3_PREFIX ?= $(firstword $(wildcard re/third_party/sdl3-install) \
                           $(wildcard ../re/third_party/sdl3-install) \
                           /mnt/c/Games/DD2/re/third_party/sdl3-install)
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -I$(SDL3_PREFIX)/include
LDFLAGS += -L$(SDL3_PREFIX)/lib
LDLIBS  += -lSDL3 -lm -lpthread -ldl

SRC = src/main.c src/platform.c src/dd2.c src/video.c src/input.c src/sound.c src/objects.c src/dave.c src/level.c src/star.c
OBJ = $(SRC:.c=.o)
BIN = dd2

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)

%.o: %.c src/dd2.h src/video.h src/input.h src/dd2_actorinfo.h src/dd2_sprtable.h src/dd2_tileattr.h
	$(CC) $(CFLAGS) -c -o $@ $<

# --- icon ------------------------------------------------------------------
# Regenerate the icon assets from the game's marker sprite (sprite 0x43).
icon: tools/mkicon.c
	$(CC) $(CFLAGS) -o /tmp/dd2_mkicon tools/mkicon.c src/dd2.c src/level.c \
	    src/objects.c src/dave.c src/platform.c src/video.c src/input.c \
	    src/sound.c $(LDFLAGS) $(LDLIBS)
	SDL_VIDEODRIVER=dummy /tmp/dd2_mkicon 0x43 icon/dd2
	python3 tools/mkicon.py

# Windows resource object (mingw-w64): embeds icon/dd2.ico into dd2.exe.
# Link it together with the objects:  mingw32-make WINDRES=windres ...
dd2_res.o: dd2.rc icon/dd2.ico
	$(WINDRES) dd2.rc -O coff -o $@

clean:
	rm -f $(OBJ) $(BIN) sweep dd2_res.o

# Headless smoke test: renders frames without a display and dumps a PNG-less
# BMP using the SDL "dummy" video driver.
DD2_DATA ?= /mnt/c/Games/DD2
test: $(BIN)
	SDL_VIDEODRIVER=dummy ./$(BIN) $(DD2_DATA)

# Star-engine stage test (tables 0x63D6 + INTRO.DD2 logo); see tests/startest.c.
startest: tests/startest.c src/star.c src/star.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/startest.c src/star.c src/dd2.c $(LDFLAGS) $(LDLIBS)

# All automated checks: the self-tests, the cross-level sweep, the level-flow
# test, the F4 gamepad stand and the scroll-coverage check (see tests/).
check: sweep flow startest joycfg cover
	./sweep
	./flow
	./startest
	./joycfg
	$(MAKE) --no-print-directory cover

# Vertical-scroll coverage: the tile renderer must paint the whole 320x200
# window, otherwise a black stripe appears at the bottom while the camera sits
# between tile rows (see tests/cover.py).
cover: $(BIN) tests/cover.py
	rm -rf /tmp/dd2_cover && mkdir -p /tmp/dd2_cover
	SDL_VIDEODRIVER=dummy ./$(BIN) $(DD2_DATA) --level 1 --frames 400 \
	    --walk 400 --jump 120 --record /tmp/dd2_cover >/dev/null 2>&1
	python3 tests/cover.py /tmp/dd2_cover

# Cross-level collision sweep (all 8 levels, scripted input).  Includes main.c
# so it drives the real dispatchers; fails if Dave leaves the map anywhere.
sweep: tests/sweep.c src/main.c
	$(CC) $(CFLAGS) -o $@ tests/sweep.c src/platform.c src/dd2.c src/video.c \
	    src/input.c src/sound.c src/objects.c src/dave.c src/level.c src/star.c \
	    $(LDFLAGS) $(LDLIBS)

# F4 joystick-calibration regression test (see tests/joycfg.c).  Uses an SDL3
# virtual gamepad, so it runs without hardware.
joycfg: tests/joycfg.c src/main.c
	$(CC) $(CFLAGS) -o $@ tests/joycfg.c src/platform.c src/dd2.c src/video.c \
	    src/input.c src/sound.c src/objects.c src/dave.c src/level.c src/star.c \
	    $(LDFLAGS) $(LDLIBS)

# End-to-end level-flow test: death -> game over -> restart (see tests/flow.c).
flow: tests/flow.c src/main.c
	$(CC) $(CFLAGS) -o $@ tests/flow.c src/platform.c src/dd2.c src/video.c \
	    src/input.c src/sound.c src/objects.c src/dave.c src/level.c src/star.c \
	    $(LDFLAGS) $(LDLIBS)

.PHONY: all clean test sweep flow startest joycfg cover icon
