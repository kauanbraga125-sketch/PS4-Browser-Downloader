# OpenOrbis PS4 Torrent Browser build
TITLE := PS4 Torrent Browser
TITLE_ID := PTBR00001
CONTENT_ID := UP0000-PTBR00001_00-0000000000000000
TOOLCHAIN ?= $(OO_PS4_TOOLCHAIN)
ifeq ($(strip $(TOOLCHAIN)),)
$(error Set OO_PS4_TOOLCHAIN to the OpenOrbis PS4 Toolchain directory)
endif
TOOLCHAIN := $(abspath $(TOOLCHAIN))
CC := clang
CXX := clang++
LD := ld.lld
INTDIR := build
ROOT := $(INTDIR)/pkg_root
OUTDIR := output
TRIPLE := x86_64-pc-freebsd12-elf
CFLAGS := -target $(TRIPLE) -fPIC -funwind-tables -O2 -Wall -Wextra -c -D_GNU_SOURCE -D_BSD_SOURCE -I$(TOOLCHAIN)/include -I$(TOOLCHAIN)/include/c++/v1 -Iinclude
CXXFLAGS := $(CFLAGS) -std=c++17 -fexceptions -fcxx-exceptions
LIBS := -lSceLibcInternal -lSceUserService -lScePad -lSceVideoOut -lSceNet -lSceNetCtl -lSceSysmodule -lScePosix -lkernel -lkernel_sys -lSDL2 -lc++ -lc++abi -lc++experimental -lm -lc
LDFLAGS := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr -L$(TOOLCHAIN)/lib --start-group $(LIBS) --end-group --undefined=__inet_aton
SOURCES := $(shell find src -name '*.cpp' -print)
OBJECTS := $(patsubst src/%.cpp,$(INTDIR)/%.o,$(SOURCES))
ELF := $(INTDIR)/app.elf
EBoot := $(ROOT)/eboot.bin
GP4 := $(INTDIR)/project.gp4
PKG := $(OUTDIR)/$(CONTENT_ID).pkg
.PHONY: all clean eboot.bin pkg
all: pkg
eboot.bin: $(EBoot)
pkg: $(PKG)
$(INTDIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $<
$(ELF): $(OBJECTS)
	$(LD) $(LDFLAGS) $(TOOLCHAIN)/lib/crt1.o $(TOOLCHAIN)/lib/crti.o $^ $(TOOLCHAIN)/lib/crtn.o -o $@
$(EBoot): $(ELF)
	@mkdir -p $(ROOT)/sce_sys
	$(TOOLCHAIN)/bin/linux/create-fself -in $< -out $@ --eboot --paid 0x3800000000000011
$(ROOT)/sce_sys/param.sfo: Makefile
	@mkdir -p $(ROOT)/sce_sys
	$(TOOLCHAIN)/bin/linux/PkgTool.Core sfo_new $@
	$(TOOLCHAIN)/bin/linux/PkgTool.Core sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(TOOLCHAIN)/bin/linux/PkgTool.Core sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '01.00'
	$(TOOLCHAIN)/bin/linux/PkgTool.Core sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/linux/PkgTool.Core sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(TOOLCHAIN)/bin/linux/PkgTool.Core sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(TOOLCHAIN)/bin/linux/PkgTool.Core sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(TOOLCHAIN)/bin/linux/PkgTool.Core sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(TOOLCHAIN)/bin/linux/PkgTool.Core sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '01.00'
$(ROOT)/sce_sys/icon0.png: tools/generate_icon.py
	@mkdir -p $(ROOT)/sce_sys
	python3 tools/generate_icon.py $@
$(GP4): $(EBoot) $(ROOT)/sce_sys/param.sfo $(ROOT)/sce_sys/icon0.png
	@mkdir -p $(INTDIR)
	@printf '%s\n' '<?xml version="1.0"?>' '<psproject fmt="gp4" version="1000"><volume><volume_type>pkg_ps4_app</volume_type><volume_id>PS4VOLUME</volume_id><package content_id="$(CONTENT_ID)" passcode="00000000000000000000000000000000" storage_type="digital50" app_type="full" /><chunk_info chunk_count="1" scenario_count="1"><chunks><chunk id="0" layer_no="0" label="Chunk #0" /></chunks><scenarios default_id="0"><scenario id="0" type="sp" initial_chunk_count="1" label="Scenario #0">0</scenario></scenarios></chunk_info></volume><files img_no="0"><file targ_path="eboot.bin" orig_path="pkg_root/eboot.bin" /><file targ_path="sce_sys/param.sfo" orig_path="pkg_root/sce_sys/param.sfo" /><file targ_path="sce_sys/icon0.png" orig_path="pkg_root/sce_sys/icon0.png" /></files><rootdir><dir targ_name="sce_sys"><file targ_name="param.sfo" /><file targ_name="icon0.png" /></dir></rootdir></psproject>' > $@
$(PKG): $(GP4)
	@mkdir -p $(OUTDIR)
	LD_LIBRARY_PATH="$(HOME)/openssl1.1/lib:$$LD_LIBRARY_PATH" DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 $(TOOLCHAIN)/bin/linux/PkgTool.Core pkg_build $(GP4) $(OUTDIR)
clean:
	rm -rf $(INTDIR) $(OUTDIR)
