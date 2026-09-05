#---------------------------------------------------------------------------
# SlotSync for Wii -- libogc homebrew, built with devkitPPC.
#
#   make            build boot.dol
#   make check      build and run the portable core's tests on this machine
#
# The portable core in core/ has no libogc dependency, so `make check` proves
# the protocol implementation against a real server without a console. See
# tests/Makefile.
#---------------------------------------------------------------------------
ifeq ($(strip $(DEVKITPPC)),)
$(error DEVKITPPC is not set. Install devkitPPC and export DEVKITPRO/DEVKITPPC)
endif

include $(DEVKITPPC)/wii_rules

TARGET   := boot
BUILD    := build
SOURCES  := source core
INCLUDES := source core

# -pipe keeps gcc off temporary files. Some Windows shells hand the
# toolchain a TMP it cannot write to, and this sidesteps that entirely.
CFLAGS   := -g -O2 -Wall -Wextra -pipe $(MACHDEP) $(INCLUDE)
CXXFLAGS := $(CFLAGS)
LDFLAGS  := -g $(MACHDEP) -Wl,-Map,$(notdir $@).map

# libfat provides the SD card filesystem; libwiiuse and libbte are the Wii
# remote; libogc is everything else.
LIBS     := -lfat -lwiiuse -lbte -logc -lm

LIBDIRS  := $(PORTLIBS) $(LIBOGC)

#---------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------
export OUTPUT   := $(CURDIR)/$(TARGET)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export OFILES := $(CFILES:.c=.o)

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib/wii)
export LD       := $(CC)

.PHONY: all clean check run

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

clean:
	@echo cleaning...
	@rm -rf $(BUILD) $(OUTPUT).elf $(OUTPUT).dol
	@$(MAKE) --no-print-directory -C tests clean

# The half that can be verified without a console.
check:
	@$(MAKE) --no-print-directory -C tests check

#---------------------------------------------------------------------------
else
#---------------------------------------------------------------------------
DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).dol: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)
#---------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------
