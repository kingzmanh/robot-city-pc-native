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
