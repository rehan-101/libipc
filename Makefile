# ============================================================================
# libipc — Day 1: shared-memory substrate.
#
# No shared library yet; that is Day 6. Today we build one standalone binary
# from two translation units so the linker step is real and the object layout
# is inspectable.
#
# Note the absence of -lrt. shm_open/shm_unlink lived in librt through glibc
# 2.33; as of glibc 2.34 they were folded into libc proper, and 2.39 (Ubuntu
# 24.04) ships an empty librt stub kept only for old binaries. Linking -lrt
# here would be harmless but misleading.
# ============================================================================

CC      := gcc

# -std=c11        strict ISO C11. Note this hides POSIX declarations unless
#                 each .c defines _POSIX_C_SOURCE first, which they do.
# -Wall -Wextra   the real baseline.
# -Wpedantic      rejects GNU extensions, notably arithmetic on void* — which
#                 is exactly the kind of sloppiness that hides pointer bugs.
# -O2             the optimisation level at which memory-order annotations
#                 start to matter; at -O0 gcc reorders nothing and a missing
#                 release store looks perfectly correct.
# -g              DWARF, so gdb/pahole can report struct layout.
# -march=native   Haswell here: lets gcc use BMI2/AVX2 and, more relevantly
#                 later, assume the full x86-64 memory model. Non-portable
#                 binaries by design; this is a machine-local build.
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -O2 -g -march=native
CPPFLAGS:= -Iinclude

BUILD   := build
BIN     := day1_smoke

OBJS    := $(BUILD)/ipc_segment.o $(BUILD)/day1_smoke.o

.PHONY: all clean asm

all: $(BIN)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/ipc_segment.o: src/ipc_segment.c include/ipc.h | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BUILD)/day1_smoke.o: tests/day1_smoke.c include/ipc.h | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@

# Emit annotated assembly so the memory-order claims in the comments can be
# checked rather than believed. After `make asm`, look for the magic store:
#
#   grep -n -A4 -B4 'magic' build/ipc_segment.s
#   grep -c 'mfence\|lock' build/ipc_segment.s      # expect 0
#
# You should find a plain `movq` publishing IPC_MAGIC and no fence instruction
# anywhere — the release/acquire pair constrains gcc, and costs the CPU
# nothing on x86-64's TSO model.
asm: | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -S -fverbose-asm src/ipc_segment.c -o $(BUILD)/ipc_segment.s
	$(CC) $(CFLAGS) $(CPPFLAGS) -S -fverbose-asm tests/day1_smoke.c -o $(BUILD)/day1_smoke.s
	@echo "wrote $(BUILD)/ipc_segment.s $(BUILD)/day1_smoke.s"

clean:
	rm -rf $(BUILD) $(BIN)
	rm -f *.o *.s
