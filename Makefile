# Robot City PC Native - build from MSYS2 MINGW64:  make -j8
#
# src/gen/rc is ROBOTCTY.EXE (16-bit Windows 3.1) recompiled to C by
# tools/recomp16.py (make gen); src/rt is the PC runtime that replaces
# Windows 3.1 (KERNEL, USER, GDI, MMSYSTEM, WinG) under it.
CC    = gcc
B     = build
PY   ?= /c/Users/Msi/AppData/Local/Programs/Python/Python310/python.exe
EXE   = RobotCity.exe

$(shell mkdir -p $(B)/tmp)
export TMP  := $(shell cygpath -m "$(CURDIR)/$(B)/tmp")
export TEMP := $(TMP)

CFLAGS_GEN := -std=gnu11 -O2 -g0 -fwrapv -fno-strict-aliasing -w -Isrc/rt
CFLAGS_RT  := -std=gnu11 -O2 -g -Wall -Wno-unused-function -Wno-misleading-indentation -fwrapv -fno-strict-aliasing -Isrc/rt
LIBS       := -static -ld3d9 -lwinmm -lgdi32 -luser32 -lm

GEN_SRCS := $(wildcard src/gen/*/*.c)
RT_SRCS  := $(sort $(wildcard src/rt/*.c))
OBJS := $(patsubst src/%.c,$(B)/%.o,$(GEN_SRCS) $(RT_SRCS))
ICON_SRC := build/game.ico
RES_OBJ := $(if $(wildcard build/game.ico),$(B)/game_res.o,)

all: $(EXE)

$(EXE): $(OBJS) $(RES_OBJ)
	$(CC) -mwindows -o $@ $^ $(LIBS)

$(B)/game_res.o: src/rt/game.rc $(ICON_SRC)
	windres -I $(B) $< -o $@

$(B)/gen/%.o: src/gen/%.c src/rt/cpu.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_GEN) -I$(dir $<) -c $< -o $@

$(B)/rt/%.o: src/rt/%.c src/rt/rt.h src/rt/cpu.h src/rt/game.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_RT) -c $< -o $@

gen:
	$(PY) tools/recomp16.py gamedata/robotcty.exe re/robotcty/functions.tsv tools/native_rc.txt src/gen/rc rc
	$(PY) tools/gen_api16.py src/gen/rc/apis.txt src/rt/api_glue.c

clean:
	rm -rf $(B)/gen $(B)/rt $(EXE)

.PHONY: all gen clean

# ---- release: game exe without the game's icon (setup puts it in from the
# user's CD) and the setup program with that exe inside ----
release: build/release/game.exe build/setup.exe
	cp build/setup.exe "Robot City PC Setup.exe"

build/release/game.exe: $(OBJS)
	@mkdir -p build/release
	$(CC) -mwindows -o $@ $^ $(LIBS)
	strip $@

SETUP_SRCS := $(wildcard installer/*.c)
SETUP_LIBS := -lcomctl32 -lcomdlg32 -lole32 -luuid -lshell32 -ladvapi32

$(B)/setup_res.o: installer/setup.rc installer/setup.manifest build/release/game.exe
	windres -I installer $< -o $@

build/setup.exe: $(SETUP_SRCS) installer/setup.h installer/install.h src/rt/game.h $(B)/setup_res.o
	$(CC) -O2 -Wall -Wno-format-truncation -mwindows -static -Isrc/rt -o $@ $(SETUP_SRCS) $(B)/setup_res.o $(SETUP_LIBS)

.PHONY: release
