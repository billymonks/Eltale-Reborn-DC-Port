# Quest 64 for Dreamcast. Run inside the toolchain container (scripts/dc.ps1 or scripts/dc.sh),
# with the KallistiOS environment sourced.
#
#   make           build build/quest64-dc.elf (builds N64Recomp and recompiles the ROM first)
#   make recomp    only regenerate build/recomp from quest64.us.z64
#   make cdi       also build build/quest64-dc.cdi (ELF + ROM) for emulators and CD-R
#   make test-cdi TEST_SCRIPT=tests/<script>.txt
#                  build build/quest64-dc-test.cdi, which also plays a test script
#   make size      section sizes of the ELF
#   make clean     remove build/ except the N64Recomp build
#
# Modded builds: add MOD=<mods> (e.g. MOD=eltale or MOD=eltale,gameplay) to any of the above to build
# the game with N64Recomp mods statically recompiled in, as build/quest64-<mods>.elf/.cdi with its
# own disc title, serial and VMU save. Stage the mods first on the host with scripts\mod.ps1 (which
# also runs this) or scripts/stage_mods.py. MOD_CONFIG=eltale.time_log=Off,... overrides mod.toml
# config defaults.

ROM        := quest64.us.z64
BUILD      := build
TOOLS      := $(BUILD)/tools
N64RECOMP  := $(TOOLS)/N64Recomp/N64Recomp
RECOMP_DIR := $(BUILD)/recomp
OBJ_DIR    := $(BUILD)/obj
TARGET     := $(BUILD)/quest64-dc.elf
GAME_CONFIG := config/quest64.us.toml

RUNTIME_C   := $(wildcard runtime/src/*.c)
RUNTIME_CXX := $(wildcard runtime/src/*.cpp)
RUNTIME_H   := $(wildcard runtime/src/*.h)
STUBS_C     := $(BUILD)/runtime_stubs.c
DECLS_H     := $(BUILD)/runtime_decls.h

CDI          := $(BUILD)/quest64-dc.cdi
TEST_CDI     := $(BUILD)/quest64-dc-test.cdi
DISC_DIR     := $(BUILD)/disc
TEST_DISC_DIR := $(BUILD)/test-disc
DISC_TITLE   := QUEST 64
DISC_SERIAL  := Q64-DC
TEST_TITLE   := QUEST 64 TEST
TEST_SERIAL  := Q64-DC-T

MOD ?=
comma := ,
empty :=
space := $(empty) $(empty)
MODS := $(subst $(comma),$(space),$(MOD))

ifneq ($(MOD),)
MOD_NAME     := $(subst $(comma),-,$(MOD))
VBUILD       := $(BUILD)/mod-$(MOD_NAME)
RECOMP_DIR   := $(VBUILD)/recomp
OBJ_DIR      := $(VBUILD)/obj
STUBS_C      := $(VBUILD)/runtime_stubs.c
DECLS_H      := $(VBUILD)/runtime_decls.h
TARGET       := $(BUILD)/quest64-$(MOD_NAME).elf
GAME_CONFIG  := $(VBUILD)/quest64.toml
MODGEN       := $(VBUILD)/modgen
MODRECOMP    := $(TOOLS)/modrecomp/modrecomp
MOD_STAGE    := $(BUILD)/mods
MOD_INPUTS   := $(foreach m,$(MODS),$(addprefix $(MOD_STAGE)/$(m)/,mod_syms.bin mod_binary.bin mod.toml))
# Mod code, data and recomp_alloc heap, at 0x80410000 after the game's 4 MB and the runtime's 64 KB.
MOD_RAM_SIZE ?= 0x200000
MOD_CONFIG   ?=

# Short codes for the disc serial and VMU file: ELT, GP, ELTGP, ...
mod_code = $(if $(filter eltale,$(1)),ELT,$(if $(filter gameplay,$(1)),GP,$(shell echo $(1) | cut -c1-3 | tr a-z A-Z)))
MOD_CODE     := $(subst $(space),,$(foreach m,$(MODS),$(call mod_code,$(m))))
MOD_TITLE    ?= $(if $(filter eltale,$(MODS)),Eltale Reborn,QUEST 64 $(MOD_CODE))
MOD_DESC     ?= $(if $(filter eltale,$(MODS)),Eltale Reborn,Quest 64 $(MOD_CODE))

CDI          := $(BUILD)/quest64-$(MOD_NAME).cdi
TEST_CDI     := $(BUILD)/quest64-$(MOD_NAME)-test.cdi
DISC_DIR     := $(VBUILD)/disc
TEST_DISC_DIR := $(VBUILD)/test-disc
DISC_TITLE   := $(MOD_TITLE)
DISC_SERIAL  := Q64-$(MOD_CODE)
TEST_TITLE   := $(MOD_TITLE) TEST
TEST_SERIAL  := Q64-T$(MOD_CODE)
MOD_DEFINES  := -DMOD_RAM_SIZE=$(MOD_RAM_SIZE) '-DVMU_FILE_NAME="Q64$(MOD_CODE).PAK"' '-DVMU_DESC="$(MOD_DESC)"'
endif

INCLUDES := -Iruntime/include -I$(RECOMP_DIR)

# rdram is read through casts of every width, so strict aliasing would miscompile the generated code.
# KOS builds with -fno-builtin, which makes every sqrtf, fabsf and small memcpy a library call;
# without errno, sqrtf is the FPU's fsqrt, as exact as the N64's sqrt.s.
COMMON_FLAGS := $(INCLUDES) -fno-strict-aliasing -fwrapv -fbuiltin -fno-math-errno -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable
RECOMP_OPT   ?= -O2
CFLAGS_GAME  := $(COMMON_FLAGS) $(RECOMP_OPT) -include $(DECLS_H)
# Extra runtime defines (run `make clean` when changing them).
RT_DEFINES   ?=
CFLAGS_RT    := $(COMMON_FLAGS) -O2 -Wall $(RT_DEFINES) $(MOD_DEFINES)

.PHONY: all recomp elf cdi test-cdi size clean distclean tools FORCE

all: recomp
	@$(MAKE) --no-print-directory elf

tools: $(N64RECOMP)

$(N64RECOMP): scripts/build_n64recomp.sh tools/patches/n64recomp-gpr32.patch tools/patches/n64recomp-return-hooks.patch
	scripts/build_n64recomp.sh $(TOOLS)/N64Recomp

recomp: $(RECOMP_DIR)/funcs.h

$(RECOMP_DIR)/funcs.h: $(N64RECOMP) $(GAME_CONFIG) $(ROM)
	@mkdir -p $(RECOMP_DIR)
	$(N64RECOMP) $(GAME_CONFIG)

$(ROM):
	@echo "Put your Quest 64 (USA) ROM at $(ROM) (big-endian .z64)." && false

ifneq ($(MOD),)
# The mods' recompiler: N64Recomp's libraries (patched by build_n64recomp.sh) plus tools/modrecomp.
$(MODRECOMP): $(N64RECOMP) tools/modrecomp/main.cpp tools/modrecomp/CMakeLists.txt
	cmake -S tools/modrecomp -B $(TOOLS)/modrecomp -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build $(TOOLS)/modrecomp --target modrecomp

$(MOD_STAGE)/%/mod_syms.bin $(MOD_STAGE)/%/mod_binary.bin $(MOD_STAGE)/%/mod.toml:
	@echo "No staged mod in $(MOD_STAGE)/$*: run scripts\\mod.ps1 -Mod $(MOD) (or python3 scripts/stage_mods.py $(MOD)) on the host first." && false

# Re-run modrecomp when MOD_RAM_SIZE or MOD_CONFIG change.
$(shell mkdir -p $(MODGEN) && (echo '$(MOD_RAM_SIZE) $(MOD_CONFIG)' | cmp -s - $(MODGEN)/options || echo '$(MOD_RAM_SIZE) $(MOD_CONFIG)' > $(MODGEN)/options))

# modrecomp only rewrites outputs whose contents change, so the stamp stands for all of them.
$(MODGEN)/stamp: $(MODRECOMP) $(MOD_INPUTS) Quest64Syms/dump.toml $(MODGEN)/options
	$(MODRECOMP) Quest64Syms/dump.toml 0x80410000 $(MOD_RAM_SIZE) $(MODGEN) $(addprefix $(MOD_STAGE)/,$(MODS)) \
		$(foreach s,$(subst $(comma),$(space),$(MOD_CONFIG)),--set $(s))
	@touch $@

# The game's config plus the mods' patches and hooks. Only rewritten when they change, so a mod
# change that keeps the same hooks doesn't regenerate the game's code.
$(GAME_CONFIG): config/quest64.us.toml $(MODGEN)/stamp scripts/gen_mod_config.py
	python3 scripts/gen_mod_config.py config/quest64.us.toml $(MODGEN)/mods_hooks.json $@ $(RECOMP_DIR)

CFLAGS_GAME += -include $(MODGEN)/mods_hooks.h
STUBS_DIRS  := runtime/mod $(MODGEN)
endif

# Everything below needs the generated sources, so it's only evaluated by the `elf` sub-make.
RECOMP_C   := $(wildcard $(RECOMP_DIR)/funcs_*.c)
GAME_OBJS  := $(patsubst $(RECOMP_DIR)/%.c,$(OBJ_DIR)/recomp/%.o,$(RECOMP_C)) $(OBJ_DIR)/recomp/lookup.o
RT_OBJS    := $(patsubst runtime/src/%.c,$(OBJ_DIR)/runtime/%.o,$(RUNTIME_C)) \
              $(patsubst runtime/src/%.cpp,$(OBJ_DIR)/runtime/%.o,$(RUNTIME_CXX)) \
              $(OBJ_DIR)/runtime/runtime_stubs.o
ifneq ($(MOD),)
RT_OBJS    += $(patsubst runtime/mod/%.c,$(OBJ_DIR)/modrt/%.o,$(wildcard runtime/mod/*.c))
MOD_OBJS   := $(patsubst $(MODGEN)/%.c,$(OBJ_DIR)/mods/%.o,$(wildcard $(MODGEN)/*.c))
endif

elf: $(TARGET)

$(TARGET): $(GAME_OBJS) $(RT_OBJS) $(MOD_OBJS)
	kos-c++ -o $@ $^ -lm
	@sh-elf-size $@

$(STUBS_C) $(DECLS_H) &: $(RECOMP_DIR)/funcs.h $(RUNTIME_C) $(RUNTIME_CXX) scripts/gen_stubs.py $(if $(MOD),$(MODGEN)/stamp $(wildcard runtime/mod/*.c))
	python3 scripts/gen_stubs.py $(RECOMP_DIR) runtime/src $(STUBS_C) $(DECLS_H) $(STUBS_DIRS)

ifneq ($(MOD),)
$(OBJ_DIR)/modrt/%.o: runtime/mod/%.c runtime/mod/modrt.h runtime/include/recomp.h runtime/include/runtime.h $(RUNTIME_H)
	@mkdir -p $(dir $@)
	kos-cc $(CFLAGS_RT) -Iruntime/mod -c $< -o $@

$(OBJ_DIR)/mods/%.o: $(MODGEN)/%.c runtime/mod/mod_recomp.h runtime/mod/modrt.h runtime/include/recomp.h runtime/include/runtime.h
	@mkdir -p $(dir $@)
	kos-cc $(COMMON_FLAGS) $(RECOMP_OPT) -Iruntime/mod -c $< -o $@
endif

$(OBJ_DIR)/recomp/%.o: $(RECOMP_DIR)/%.c runtime/include/recomp.h $(DECLS_H)
	@mkdir -p $(dir $@)
	kos-cc $(CFLAGS_GAME) -c $< -o $@

# lookup.cpp is plain C despite its name; compiling it as C keeps get_entrypoint_address unmangled.
$(OBJ_DIR)/recomp/lookup.o: $(RECOMP_DIR)/lookup.cpp runtime/include/recomp.h
	@mkdir -p $(dir $@)
	kos-cc -x c $(CFLAGS_RT) -c $< -o $@

$(OBJ_DIR)/runtime/%.o: runtime/src/%.c runtime/include/recomp.h runtime/include/runtime.h $(RUNTIME_H)
	@mkdir -p $(dir $@)
	kos-cc $(CFLAGS_RT) -c $< -o $@

$(OBJ_DIR)/runtime/%.o: runtime/src/%.cpp runtime/include/recomp.h runtime/include/runtime.h $(RUNTIME_H)
	@mkdir -p $(dir $@)
	kos-c++ $(CFLAGS_RT) -c $< -o $@

$(OBJ_DIR)/runtime/runtime_stubs.o: $(STUBS_C) runtime/include/runtime.h
	@mkdir -p $(dir $@)
	kos-cc $(CFLAGS_RT) -c $< -o $@

# A self-booting disc with the ELF and the ROM, for emulators and CD-R/ODE testing. It contains
# the game, so it's for your own use only.

# Cover art: your own assets/cover.png (never committed) becomes 0GDTEX.PVR in the disc root,
# which Flycast and GDEMU menus show. Without it the discs have no cover. Modded discs use
# assets/cover-<first mod>.png if there is one, else assets/cover.png.
ifeq ($(MOD),)
COVER := assets/cover.png
GDTEX := $(if $(wildcard $(COVER)),$(BUILD)/0GDTEX.PVR)

$(BUILD)/0GDTEX.PVR: $(COVER) scripts/make_gdtex.py
	python3 scripts/make_gdtex.py $< $@
else
COVER := $(firstword $(wildcard assets/cover-$(firstword $(MODS)).png assets/cover.png))
GDTEX := $(if $(COVER),$(VBUILD)/0GDTEX.PVR)

$(VBUILD)/0GDTEX.PVR: $(COVER) scripts/make_gdtex.py
	python3 scripts/make_gdtex.py $< $@
endif

# `make test-cdi TEST_SCRIPT=tests/<script>.txt` builds a separate disc, $(TEST_CDI), that also
# carries a scripted-input test (see runtime/src/test.c). The normal disc never has one.
TEST_SCRIPT ?=

cdi:
	$(if $(TEST_SCRIPT),@echo "TEST_SCRIPT is for make test-cdi; make cdi builds the clean disc." && false)
	@$(MAKE) --no-print-directory all
	@$(MAKE) --no-print-directory $(CDI)

test-cdi:
	$(if $(TEST_SCRIPT),,@echo "make test-cdi needs TEST_SCRIPT=tests/<script>.txt" && false)
	@$(MAKE) --no-print-directory all
	@$(MAKE) --no-print-directory $(TEST_CDI)

$(CDI): $(TARGET) $(ROM) $(GDTEX)
	@mkdir -p $(DISC_DIR)
	cmp -s $(ROM) $(DISC_DIR)/quest64.z64 || cp $(ROM) $(DISC_DIR)/quest64.z64
	mkdcdisc -e $(TARGET) -f $(DISC_DIR)/quest64.z64 $(addprefix -f ,$(GDTEX)) \
		-n "$(DISC_TITLE)" -a "Quest64-DC-Port" -s "$(DISC_SERIAL)" -o $@ --allow-overwrite

# Rebuilt every time, so a changed script takes effect. Its own serial keeps its VMU apart in Flycast.
$(TEST_CDI): $(TARGET) $(ROM) $(GDTEX) FORCE
	@mkdir -p $(TEST_DISC_DIR)
	cmp -s $(ROM) $(TEST_DISC_DIR)/quest64.z64 || cp $(ROM) $(TEST_DISC_DIR)/quest64.z64
	$(if $(MODS),python3 scripts/test_syms.py $(TEST_SCRIPT) $(MODGEN)/mods_layout.txt $(MOD_STAGE) > $(TEST_DISC_DIR)/q64test.txt,cp $(TEST_SCRIPT) $(TEST_DISC_DIR)/q64test.txt)
	mkdcdisc -e $(TARGET) -f $(TEST_DISC_DIR)/quest64.z64 -f $(TEST_DISC_DIR)/q64test.txt \
		$(addprefix -f ,$(GDTEX)) -n "$(TEST_TITLE)" -a "Quest64-DC-Port" -s "$(TEST_SERIAL)" -o $@ --allow-overwrite

FORCE:

size: $(TARGET)
	sh-elf-size -A $(TARGET) | grep -E '^\.(text|rodata|data|bss)|Total'

clean:
	rm -rf $(RECOMP_DIR) $(OBJ_DIR) $(STUBS_C) $(DECLS_H) $(TARGET) $(if $(MOD),$(MODGEN) $(GAME_CONFIG))

distclean:
	rm -rf $(BUILD)
