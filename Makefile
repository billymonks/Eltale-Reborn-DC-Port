# Quest 64 for Dreamcast. Run inside the toolchain container (scripts/dc.ps1 or scripts/dc.sh),
# with the KallistiOS environment sourced.
#
#   make           build build/quest64-dc.elf (builds N64Recomp and recompiles the ROM first)
#   make recomp    only regenerate build/recomp from quest64.us.z64
#   make cdi       also build build/quest64-dc.cdi (ELF + ROM) for emulators and CD-R
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
COMMON_FLAGS := $(INCLUDES) -fno-strict-aliasing -fwrapv -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable
RECOMP_OPT   ?= -O2
CFLAGS_GAME  := $(COMMON_FLAGS) $(RECOMP_OPT) -include $(DECLS_H)
# Extra runtime defines, e.g. RT_DEFINES=-DQ64DC_AUTOPRESS for headless boot tests (run
# `make clean` when changing them).
RT_DEFINES   ?=
CFLAGS_RT    := $(COMMON_FLAGS) -O2 -Wall $(RT_DEFINES)

.PHONY: all recomp elf cdi size clean distclean tools

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
CDI := $(BUILD)/quest64-dc.cdi

cdi: all
	@$(MAKE) --no-print-directory $(CDI)

$(CDI): $(TARGET) $(ROM)
	@mkdir -p $(BUILD)/disc
	cp $(ROM) $(BUILD)/disc/quest64.z64
	mkdcdisc -e $(TARGET) -f $(BUILD)/disc/quest64.z64 -n "Quest 64 DC" -a "Quest64-DC-Port" -o $@ --allow-overwrite

size: $(TARGET)
	sh-elf-size -A $(TARGET) | grep -E '^\.(text|rodata|data|bss)|Total'

clean:
	rm -rf $(RECOMP_DIR) $(OBJ_DIR) $(STUBS_C) $(DECLS_H) $(TARGET)

distclean:
	rm -rf $(BUILD)
