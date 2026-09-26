# Sonic R Nintendo 3DS build (devkitARM + libctru + citro3d)
#
#   export DEVKITPRO=/opt/devkitpro DEVKITARM=$DEVKITPRO/devkitARM   (or ~/devkitpro)
#   make            -> SonicR.3dsx (+ SonicR.smdh embedded)
#   make deploy-azahar / run-azahar   -> copy into / launch from the Azahar flatpak SD
#
# Game logic is shared with the SDL build (../sdl/src); 3ds/src replaces the
# renderer, platform, sound and network transport. Same layout as ../dc.

ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO not set. export DEVKITPRO=<devkitpro root> DEVKITARM=$$DEVKITPRO/devkitARM)
endif
DEVKITARM ?= $(DEVKITPRO)/devkitARM
CTRULIB   ?= $(DEVKITPRO)/libctru
TOOLS     := $(DEVKITPRO)/tools/bin

CC      := $(DEVKITARM)/bin/arm-none-eabi-gcc
AS      := $(DEVKITARM)/bin/arm-none-eabi-as
NM      := $(DEVKITARM)/bin/arm-none-eabi-nm
PICASSO := $(TOOLS)/picasso
BIN2S   := $(TOOLS)/bin2s
_3DSXTOOL := $(TOOLS)/3dsxtool
SMDHTOOL  := $(TOOLS)/smdhtool

SRCDIR     := src
SDL_SRCDIR := ../sdl/src
BUILDDIR   := build
TARGET     := SonicR

APP_TITLE       := Sonic R
APP_DESCRIPTION := Sonic R for Nintendo 3DS (stereoscopic)
APP_AUTHOR      := sonic-r-3d
APP_ICON        := icon.png

# Shared files the 3DS replaces (GL/SDL renderer, SDL platform + sound). The
# matchmaker (BearSSL HTTPS) is compiled in like on the Dreamcast.
SDL_REPLACED := \
    $(SDL_SRCDIR)/render_gl.c \
    $(SDL_SRCDIR)/r_gl_backend.c \
    $(SDL_SRCDIR)/r_compose_gl.c \
    $(SDL_SRCDIR)/r_capture.c \
    $(SDL_SRCDIR)/r_soft_backend.c \
    $(SDL_SRCDIR)/platform/platform_sdl.c \
    $(SDL_SRCDIR)/sound/music_sdl.c \
    $(SDL_SRCDIR)/sound/sfx_sdl.c \
    $(SDL_SRCDIR)/sound/music_rwops.c \
    $(SDL_SRCDIR)/sound/adp.c \
    $(SDL_SRCDIR)/net/net_upnp.c

SDL_ALL_C := $(shell find $(SDL_SRCDIR) -name '*.c' -not -path '*/gles2/*')
SDL_C     := $(filter-out $(SDL_REPLACED), $(SDL_ALL_C))
DS_C      := $(shell find $(SRCDIR) -name '*.c')
PICA      := $(SRCDIR)/vshader.v.pica

SDL_OBJS := $(patsubst ../%.c,$(BUILDDIR)/%.o,$(SDL_C))
DS_OBJS  := $(patsubst %.c,$(BUILDDIR)/3ds/%.o,$(DS_C))
SHBIN    := $(BUILDDIR)/vshader.shbin
SHBIN_O  := $(BUILDDIR)/vshader.shbin.o
SHBIN_H  := $(BUILDDIR)/vshader_shbin.h
OBJS     := $(SDL_OBJS) $(DS_OBJS) $(SHBIN_O)

DEBUG_LOG ?= 1
ifeq ($(DEBUG_LOG),1)
  DEBUG_LOG_CFLAGS = -DSONICR_DEBUG_LOG
else
  DEBUG_LOG_CFLAGS =
endif

ARCH   := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS := $(ARCH) -mword-relocations -ffunction-sections -fdata-sections \
          -std=gnu11 -O3 -g -fno-common -fsingle-precision-constant \
          -Wall -Wno-unused -Wno-parentheses -Wno-strict-aliasing -Wno-unused-result \
          -Wno-misleading-indentation -Wno-array-bounds -Wno-attribute-warning \
          -Wno-maybe-uninitialized -Wno-type-limits -Wno-stringop-overflow \
          -Wno-unused-but-set-variable -Wno-implicit-fallthrough -Wno-pointer-sign \
          -Wno-stringop-truncation -Wno-incompatible-pointer-types -Wno-int-conversion \
          -I$(SRCDIR) -I$(SDL_SRCDIR) -I$(BUILDDIR) -I$(CTRULIB)/include \
          -D__3DS__ -DSONICR_SDL -DSONICR_3DS -DSONICR_FLOAT32 -DSONICR_MATCHMAKER -DBLURRY \
          -DDATA_DIR='"."' $(DEBUG_LOG_CFLAGS)
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -Wl,--gc-sections -Wl,-Map,$(BUILDDIR)/$(TARGET).map
LIBS    := -L$(CTRULIB)/lib -lcitro3d -lctru -lm

.PHONY: all clean cia deploy-azahar run-azahar

all: $(TARGET).3dsx

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# Shader: picasso -> .shbin -> bin2s -> object + header (vshader_shbin, vshader_shbin_size)
$(SHBIN): $(PICA) | $(BUILDDIR)
	$(PICASSO) -o $@ $<

$(SHBIN_O) $(SHBIN_H) &: $(SHBIN)
	$(BIN2S) -a 4 -H $(SHBIN_H) $(SHBIN) | $(AS) -o $(SHBIN_O)

# Every 3ds/src object may include the shader header.
$(DS_OBJS): $(SHBIN_H)

$(BUILDDIR)/sdl/%.o: ../sdl/%.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) -MMD -MP $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/3ds/%.o: %.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) -MMD -MP $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/$(TARGET).elf: $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -o $@
	$(NM) -CSn $@ > $(BUILDDIR)/$(TARGET).lst

$(BUILDDIR)/$(TARGET).smdh: $(APP_ICON) Makefile | $(BUILDDIR)
	$(SMDHTOOL) --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $(APP_ICON) $@

$(TARGET).3dsx: $(BUILDDIR)/$(TARGET).elf $(BUILDDIR)/$(TARGET).smdh
	$(_3DSXTOOL) $< $@ --smdh=$(BUILDDIR)/$(TARGET).smdh

# --- CIA (installable title) ---------------------------------------------------
# makerom (Project_CTR) and bannertool are not part of devkitPro; put them in
# $(DEVKITPRO)/tools/bin or set MAKEROM / BANNERTOOL. The banner is optional.
MAKEROM    ?= $(TOOLS)/makerom
BANNERTOOL ?= $(TOOLS)/bannertool
CIA_RSF    := cia/SonicR.rsf
BANNER     := $(BUILDDIR)/banner.bnr

$(BANNER): cia/banner.png cia/banner.wav | $(BUILDDIR)
	@if [ -x "$(BANNERTOOL)" ]; then $(BANNERTOOL) makebanner -i cia/banner.png -a cia/banner.wav -o $@; \
	else echo "bannertool not found; building the CIA without a banner"; : > $@; fi

$(TARGET).cia: $(BUILDDIR)/$(TARGET).elf $(BUILDDIR)/$(TARGET).smdh $(CIA_RSF) $(BANNER)
	$(MAKEROM) -f cia -o $@ -elf $< -rsf $(CIA_RSF) -icon $(BUILDDIR)/$(TARGET).smdh \
	    $(if $(shell [ -s $(BANNER) ] && echo y),-banner $(BANNER),) -exefslogo -target t

cia: $(TARGET).cia

clean:
	rm -rf $(BUILDDIR) $(TARGET).3dsx $(TARGET).cia

# --- Azahar (flatpak) helpers ------------------------------------------------
AZAHAR_SD ?= $(HOME)/.var/app/org.azahar_emu.Azahar/data/azahar-emu/sdmc
AZAHAR_APPDIR := $(AZAHAR_SD)/3ds/SonicR

deploy-azahar: $(TARGET).3dsx
	mkdir -p $(AZAHAR_APPDIR)
	cp -f $(TARGET).3dsx $(AZAHAR_APPDIR)/
	@[ -e $(AZAHAR_SD)/3ds/dspfirm.cdc ] || : > $(AZAHAR_SD)/3ds/dspfirm.cdc
	@echo "deployed to $(AZAHAR_APPDIR)"

run-azahar: deploy-azahar
	flatpak run --env=QT_QPA_PLATFORM=xcb org.azahar_emu.Azahar -w $(AZAHAR_APPDIR)/$(TARGET).3dsx

-include $(OBJS:.o=.d)
