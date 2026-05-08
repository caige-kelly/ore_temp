# Default to zig cc, but only if the user hasn't set CC themselves
# (via env var, `make CC=gcc`, etc.). The `origin` check is needed
# because Make has a built-in CC=cc that ?= won't override.
ifeq ($(origin CC),default)
  CC = zig cc
endif

CFLAGS  ?= -std=c17 -Wall -Isrc -g
LDFLAGS ?=

# Pick up Nix-provided linker flags when present; harmless when empty.
LDFLAGS += $(NIX_LDFLAGS)

TARGET = ore
# Build everything under src/, except:
#   - src/name_resolution/ (legacy resolver — dead code on disk)
#   - src/sema/type/ (legacy typecheck — broken, pending un-prune
#     cleanup; lives in its own subdir as a coherent module)
# The new sema layers (ids/, query/, scope/, modules/, resolve/) ARE
# built. Two finds: the first excludes all of src/sema and
# src/name_resolution; the second re-adds anything 2+ levels deep
# under src/sema, then we exclude src/sema/type/ from that.
SRCS := $(shell find src -name '*.c' -not -path 'src/sema/*' -not -path 'src/name_resolution/*') \
        $(shell find src/sema -mindepth 2 -name '*.c' -not -path 'src/sema/type/*')

FORMAT = clang-format
FORMAT_FLAGS = -i -style=file --fallback-style=LLVM

.PHONY: all clean test format

format:
	$(FORMAT) $(FORMAT_FLAGS) $(SRCS)

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $(TARGET)

clean:
	rm -f $(TARGET)

# asan's runtime library is platform-specific. macOS/clang ships it inside
# -fsanitize=address; Linux toolchains often need an explicit -lasan. Inject
# the linker flag from the env (e.g. NIX_LDFLAGS="-lasan") rather than
# hard-coding it here.
TEST_CFLAGS ?= $(CFLAGS) -fsanitize=address $(NIX_LDFLAGS)

test:
	@CC="$(CC)" TEST_CFLAGS="$(TEST_CFLAGS)" sh tools/test.sh