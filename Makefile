# UnitV Pro - PS4 (OpenOrbis Toolchain)
# Build: export OO_PS4_TOOLCHAIN=/caminho/OpenOrbis/PS4Toolchain && make

# Package metadata.
TITLE       := UnitV Pro
VERSION     := 22.00
TITLE_ID    := UNTV00001
CONTENT_ID  := IV0000-UNTV00001_00-UNITVPROPS400000

# Libraries linked into the ELF.
# FFmpeg compilado por scripts/build-ffmpeg.sh
FFMPEG      ?= ffmpeg-ps4

LIBS        := -L$(FFMPEG)/lib -lavformat -lavcodec -lswresample -lswscale -lavutil \
               -lc -lkernel -lc++ -lSceUserService -lSceVideoOut -lSceAudioOut -lScePad \
               -lSceSysmodule -lSceFreeType -lSDL2 -lSceNet -lSceNetCtl -lSceSsl -lSceHttp \
               -lSceImeDialog -lSceCommonDialog -lSceSystemService

EXTRAFLAGS  := -O2 -Ithird_party -Isrc -isystem $(FFMPEG)/include -Wno-unused-command-line-argument

# Asset and module directories.
ASSETS      := $(wildcard assets/fonts/*) $(wildcard assets/images/*)
LIBMODULES  := $(wildcard sce_module/*)

# Root vars
TOOLCHAIN   := $(OO_PS4_TOOLCHAIN)
SRCDIR      := src
INTDIR      := build/obj

CPPFILES    := $(wildcard $(SRCDIR)/*.cpp)
OBJS        := $(patsubst $(SRCDIR)/%.cpp, $(INTDIR)/%.o, $(CPPFILES))

# libc++ precisa vir antes dos headers C (musl) para o <cmath> funcionar
CXXFLAGS    := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -c $(EXTRAFLAGS) -std=gnu++17 -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include/c++/v1 -isystem $(TOOLCHAIN)/include
LDFLAGS     := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr -L$(TOOLCHAIN)/lib $(LIBS) $(TOOLCHAIN)/lib/crt1.o

_unused     := $(shell mkdir -p $(INTDIR))

UNAME_S     := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
	CCX     := clang++
	LD      := ld.lld
	CDIR    := linux
endif
ifeq ($(UNAME_S),Darwin)
	CCX     := /usr/local/opt/llvm/bin/clang++
	LD      := /usr/local/opt/llvm/bin/ld.lld
	CDIR    := macos
endif

# Pode ser sobrescrito, ex.: make PKGTOOL="dotnet $$OO_PS4_TOOLCHAIN/bin/windows/PkgTool.Core.dll"
PKGTOOL     ?= $(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core

all: $(CONTENT_ID).pkg

$(CONTENT_ID).pkg: pkg.gp4
	$(PKGTOOL) pkg_build $< .

pkg.gp4: eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png sce_sys/pic1.png $(LIBMODULES) $(ASSETS)
	$(TOOLCHAIN)/bin/$(CDIR)/create-gp4 -out $@ --content-id=$(CONTENT_ID) --files "$^"

sce_sys/param.sfo: Makefile
	$(PKGTOOL) sfo_new $@
	$(PKGTOOL) sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(PKGTOOL) sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
	$(PKGTOOL) sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 2
	$(PKGTOOL) sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(PKGTOOL) sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(PKGTOOL) sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(PKGTOOL) sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(PKGTOOL) sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(PKGTOOL) sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(PKGTOOL) sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

eboot.bin: $(OBJS)
	$(LD) $(OBJS) -o build/UnitVPro.elf $(LDFLAGS)
	$(TOOLCHAIN)/bin/$(CDIR)/create-fself -in=build/UnitVPro.elf -out=build/UnitVPro.oelf --eboot "eboot.bin" --paid 0x3800000000000011

$(INTDIR)/%.o: $(SRCDIR)/%.cpp $(wildcard $(SRCDIR)/*.h)
	$(CCX) $(CXXFLAGS) -o $@ $<

clean:
	rm -rf build eboot.bin pkg.gp4 sce_sys/param.sfo $(CONTENT_ID).pkg

.PHONY: all clean
