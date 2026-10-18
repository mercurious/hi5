# HI5 — FM glitch beat generator for Miyoo Mini Plus / OnionOS
#
#   make          cross-compile dist/HI5/hi5 for the Miyoo (ARMv7 hard-float, glibc 2.28)
#   make test     build the host harness and render screenshots + audio into build/test
#   make zip      package dist/HI5 as build/HI5.zip for copying to SD:/App/
#   make install SD=/run/media/$USER/<card>   copy the app onto a mounted SD card

ZIG    ?= $(HOME)/.local/opt/zig-aarch64-linux-0.14.1/zig
TARGET := arm-linux-gnueabihf.2.28
SDL_INC := third_party/SDL-1.2.15-headers
SDL_LIB := third_party/onion-lib/libSDL-1.2.so.0

SRC    := src/engine.c src/ui.c src/gfx.c src/sync.c
HDR    := src/engine.h src/ui.h src/gfx.h src/sync.h src/font_data.h
CFLAGS := -O2 -std=gnu11 -Wall -Wextra -fno-math-errno
APP    := dist/HI5

all: $(APP)/hi5 $(APP)/icon.png

src/font_data.h: tools/mkfont.py
	python3 tools/mkfont.py /usr/share/fonts/google-noto/NotoSansMono-Bold.ttf $@

$(APP)/icon.png: tools/mkicon.py
	python3 tools/mkicon.py $@

# Onion's MI_AO/MI_GFX-backed SDL 1.2 (LGPL), linked against but not committed
$(SDL_LIB):
	curl -sSL --create-dirs -o $@ https://raw.githubusercontent.com/OnionUI/Onion/main/static/build/.tmp_update/lib/parasyte/libSDL-1.2.so.0

$(APP)/hi5: $(SRC) src/main_sdl.c $(HDR) $(SDL_LIB)
	$(ZIG) cc -target $(TARGET) -mcpu=cortex_a7 $(CFLAGS) -I$(SDL_INC) -o $@ \
		$(SRC) src/main_sdl.c $(SDL_LIB) -lm -lpthread -Wl,--allow-shlib-undefined -s

build/hi5_test: $(SRC) src/main_test.c $(HDR)
	@mkdir -p build
	cc $(CFLAGS) -o $@ $(SRC) src/main_test.c -lm

test: build/hi5_test
	@mkdir -p build/test
	build/hi5_test build/test $(APP)/samples
	cd build/test && for f in *.ppm; do magick $$f $${f%.ppm}.png && rm $$f; done

zip: all
	@mkdir -p build
	rm -f build/HI5.zip
	cd dist && zip -r ../build/HI5.zip HI5 -x 'HI5/hi5.sav' 'HI5/log.txt'

install: all
	@test -n "$(SD)" || (echo "usage: make install SD=/path/to/mounted/sdcard" && exit 1)
	@test -d "$(SD)/App" || (echo "$(SD)/App not found — is this the OnionOS card?" && exit 1)
	mkdir -p "$(SD)/App/HI5"
	cp -r $(APP)/. "$(SD)/App/HI5/"
	sync

clean:
	rm -rf build $(APP)/hi5

.PHONY: all test zip install clean
