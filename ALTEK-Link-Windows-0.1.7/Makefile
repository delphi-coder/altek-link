# Windows: mingw32-make ARCH=x64 MODE=release
ARCH ?= x64
MODE ?= release
CC = gcc
WINDRES = windres
ifeq ($(ARCH),x86)
ARCHFLAGS = -m32
else
ARCHFLAGS = -m64
endif
ifeq ($(MODE),debug)
MODEFLAGS = -O0 -g -DALTEK_DEBUG_UDP_LOG
CPPFLAGS = -Ibuild/config/debug -Iinclude -Ithird_party/openh264
CONFIG_DEP = build/config/debug/build_config.h
OBJROOT = build/debug/$(ARCH)
OUTDIR = bin/debug/$(ARCH)
STRIPFLAGS =
else
MODEFLAGS = -O2 -DNDEBUG
CPPFLAGS = -Iinclude -Ithird_party/openh264
CONFIG_DEP =
OBJROOT = build/release/$(ARCH)
OUTDIR = bin/$(ARCH)
STRIPFLAGS = -s
endif
SOURCES = $(wildcard src/*.c)
OBJECTS = $(patsubst src/%.c,$(OBJROOT)/%.o,$(SOURCES))
CFLAGS = $(ARCHFLAGS) -DALTEK_EXPECT_$(if $(filter x86,$(ARCH)),X86,X64) $(MODEFLAGS) -std=gnu90 -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation
LDLIBS = -lws2_32 -lsecur32 -lcrypt32 -ladvapi32 -liphlpapi -lole32 -loleaut32 -luuid -luser32 -lusp10 -lgdi32 -lcomdlg32 -lcomctl32 -lshell32 -lwinmm -lmsimg32 -lwindowscodecs -lmf -lmfplat -lmfuuid
all: $(OUTDIR)/AltekLink.exe
$(OBJROOT)/%.o: src/%.c include/link.h include/build_number.h include/ui.h include/json.h $(CONFIG_DEP)
	@if not exist "$(subst /,\,$(OBJROOT))" mkdir "$(subst /,\,$(OBJROOT))"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(OBJROOT)/app.res: assets/app.rc assets/app.ico assets/app.manifest include/build_number.h
	@if not exist "$(subst /,\,$(OBJROOT))" mkdir "$(subst /,\,$(OBJROOT))"
	$(WINDRES) -Iassets -i assets/app.rc -o $@ -O coff $(if $(filter x86,$(ARCH)),--target=pe-i386,--target=pe-x86-64)
$(OUTDIR)/AltekLink.exe: $(OBJECTS) $(OBJROOT)/app.res
	@if not exist "$(subst /,\,$(OUTDIR))" mkdir "$(subst /,\,$(OUTDIR))"
	$(CC) $(ARCHFLAGS) -mwindows -static-libgcc -static-libstdc++ $(STRIPFLAGS) -Wl,--major-os-version,6,--minor-os-version,1,--major-subsystem-version,6,--minor-subsystem-version,1 -o $@ $^ $(LDLIBS)
.PHONY: all
