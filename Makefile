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

ROM        := quest64.us.z64
BUILD      := build
TOOLS      := $(BUILD)/tools
N64RECOMP  := $(TOOLS)/N64Recomp/N64Recomp
RECOMP_DIR := $(BUILD)/recomp
OBJ_DIR    := $(BUILD)/obj
TARGET     := $(BUILD)/quest64-dc.elf

RUNTIME_C   := $(wildcard runtime/src/*.c)
RUNTIME_CXX := $(wildcard runtime/src/*.cpp)
RUNTIME_H   := $(wildcard runtime/src/*.h)
STUBS_C     := $(BUILD)/runtime_stubs.c
DECLS_H     := $(BUILD)/runtime_decls.h

INCLUDES := -Iruntime/include -I$(RECOMP_DIR)

# rdram is read through casts of every width, so strict aliasing would miscompile the generated code.
# KOS builds with -fno-builtin, which makes every sqrtf, fabsf and small memcpy a library call;
# without errno, sqrtf is the FPU's fsqrt, as exact as the N64's sqrt.s.
COMMON_FLAGS := $(INCLUDES) -fno-strict-aliasing -fwrapv -fbuiltin -fno-math-errno -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable
RECOMP_OPT   ?= -O2
CFLAGS_GAME  := $(COMMON_FLAGS) $(RECOMP_OPT) -include $(DECLS_H)
# Extra runtime defines (run `make clean` when changing them).
RT_DEFINES   ?=
CFLAGS_RT    := $(COMMON_FLAGS) -O2 -Wall $(RT_DEFINES)

.PHONY: all recomp elf cdi test-cdi size clean distclean tools FORCE

all: recomp
	@$(MAKE) --no-print-directory elf

tools: $(N64RECOMP)

$(N64RECOMP): scripts/build_n64recomp.sh tools/patches/n64recomp-gpr32.patch
	scripts/build_n64recomp.sh $(TOOLS)/N64Recomp

recomp: $(RECOMP_DIR)/funcs.h

$(RECOMP_DIR)/funcs.h: $(N64RECOMP) config/quest64.us.toml $(ROM)
	@mkdir -p $(RECOMP_DIR)
	$(N64RECOMP) config/quest64.us.toml

$(ROM):
	@echo "Put your Quest 64 (USA) ROM at $(ROM) (big-endian .z64)." && false

# Everything below needs the generated sources, so it's only evaluated by the `elf` sub-make.
RECOMP_C   := $(wildcard $(RECOMP_DIR)/funcs_*.c)
GAME_OBJS  := $(patsubst $(RECOMP_DIR)/%.c,$(OBJ_DIR)/recomp/%.o,$(RECOMP_C)) $(OBJ_DIR)/recomp/lookup.o
RT_OBJS    := $(patsubst runtime/src/%.c,$(OBJ_DIR)/runtime/%.o,$(RUNTIME_C)) \
              $(patsubst runtime/src/%.cpp,$(OBJ_DIR)/runtime/%.o,$(RUNTIME_CXX)) \
              $(OBJ_DIR)/runtime/runtime_stubs.o

elf: $(TARGET)

$(TARGET): $(GAME_OBJS) $(RT_OBJS)
	kos-c++ -o $@ $^ -lm
	@sh-elf-size $@

$(STUBS_C) $(DECLS_H) &: $(RECOMP_DIR)/funcs.h $(RUNTIME_C) $(RUNTIME_CXX) scripts/gen_stubs.py
	python3 scripts/gen_stubs.py $(RECOMP_DIR) runtime/src $(STUBS_C) $(DECLS_H)

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
CDI      := $(BUILD)/quest64-dc.cdi
TEST_CDI := $(BUILD)/quest64-dc-test.cdi

# Cover art: your own assets/cover.png (never committed) becomes 0GDTEX.PVR in the disc root,
# which Flycast and GDEMU menus show. Without it the discs have no cover.
COVER := assets/cover.png
GDTEX := $(if $(wildcard $(COVER)),$(BUILD)/0GDTEX.PVR)

$(BUILD)/0GDTEX.PVR: $(COVER) scripts/make_gdtex.py
	python3 scripts/make_gdtex.py $< $@

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
	@mkdir -p $(BUILD)/disc
	cmp -s $(ROM) $(BUILD)/disc/quest64.z64 || cp $(ROM) $(BUILD)/disc/quest64.z64
	mkdcdisc -e $(TARGET) -f $(BUILD)/disc/quest64.z64 $(addprefix -f ,$(GDTEX)) \
		-n "QUEST 64" -a "Quest64-DC-Port" -s "Q64-DC" -o $@ --allow-overwrite

# Rebuilt every time, so a changed script takes effect. Its own serial keeps its VMU apart in Flycast.
$(TEST_CDI): $(TARGET) $(ROM) $(GDTEX) FORCE
	@mkdir -p $(BUILD)/test-disc
	cmp -s $(ROM) $(BUILD)/test-disc/quest64.z64 || cp $(ROM) $(BUILD)/test-disc/quest64.z64
	cp $(TEST_SCRIPT) $(BUILD)/test-disc/q64test.txt
	mkdcdisc -e $(TARGET) -f $(BUILD)/test-disc/quest64.z64 -f $(BUILD)/test-disc/q64test.txt \
		$(addprefix -f ,$(GDTEX)) -n "QUEST 64 TEST" -a "Quest64-DC-Port" -s "Q64-DC-T" -o $@ --allow-overwrite

FORCE:

size: $(TARGET)
	sh-elf-size -A $(TARGET) | grep -E '^\.(text|rodata|data|bss)|Total'

clean:
	rm -rf $(RECOMP_DIR) $(OBJ_DIR) $(STUBS_C) $(DECLS_H) $(TARGET)

distclean:
	rm -rf $(BUILD)
