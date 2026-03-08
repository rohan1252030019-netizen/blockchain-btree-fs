# ═══════════════════════════════════════════════════════════════════════════════
# SentinelStorage v3  —  Kali Linux / Debian / Ubuntu  (Flat-Folder Build)
# ═══════════════════════════════════════════════════════════════════════════════
#
# ✅ All .c files + befs.h live in THIS directory — no src/ subfolder.
# ✅ build/ is created automatically — no pre-existing folder needed.
# ✅ Raylib is found via pkg-config OR a manual path fallback.
# ✅ Works with apt-installed libraylib-dev AND source-compiled raylib.
#
# QUICK START (choose the path that matches your system):
#
#   Path A  — apt has libraylib-dev (Ubuntu 22.04 / Kali 2023.4+):
#       sudo apt install libraylib-dev
#       make
#
#   Path B  — apt does NOT have libraylib-dev (older Kali, Debian 11):
#       make raylib-src        ← clones + builds raylib 4.5 to /usr/local
#       make
#
#   Path C  — already have libraylib.so but pkg-config doesn't find it:
#       make RAYLIB_PATH=/usr  ← or wherever your libraylib.so lives
#       make
#
# OTHER TARGETS:
#   make run          compile + launch GUI
#   make cli          compile + CLI
#   make test         10-key integration test
#   make audit        run audit on befs.db
#   make chaos        inject corruption + re-audit
#   make raylib-check show where raylib is (or isn't)
#   make clean        delete build/ and binary
# ═══════════════════════════════════════════════════════════════════════════════

CC      := gcc
CSTD    := -std=c99
WARN    := -Wall -Wextra -Wno-unused-parameter
OPT     := -O2
TARGET  := sentinelstorage
BUILDDIR:= build

# ── All source files in current directory ──────────────────────────────────────
SRCS := main.c disk_io.c blockchain.c btree.c gui.c
OBJS := $(SRCS:%.c=$(BUILDDIR)/%.o)

# ── Include path: current dir (befs.h + raylib.h both live here or system) ─────
INCLUDES := -I.

# ── Raylib: try pkg-config, then RAYLIB_PATH override, then bare flag ──────────
# You can override from command line: make RAYLIB_PATH=/usr/local
RAYLIB_PATH ?=

ifneq ($(shell pkg-config --exists raylib 2>/dev/null && echo yes),)
    # pkg-config knows about raylib
    RAYLIB_CFLAGS := $(shell pkg-config --cflags raylib)
    RAYLIB_LFLAGS := $(shell pkg-config --libs   raylib)
else ifneq ($(RAYLIB_PATH),)
    # Manual path given on command line
    RAYLIB_CFLAGS := -I$(RAYLIB_PATH)/include
    RAYLIB_LFLAGS := -L$(RAYLIB_PATH)/lib -lraylib
else ifneq ($(wildcard /usr/local/lib/libraylib.so /usr/local/lib/libraylib.a),)
    # Found in /usr/local (source-compiled)
    RAYLIB_CFLAGS := -I/usr/local/include
    RAYLIB_LFLAGS := -L/usr/local/lib -lraylib
else ifneq ($(wildcard /usr/lib/x86_64-linux-gnu/libraylib.so /usr/lib/x86_64-linux-gnu/libraylib.a),)
    # Found in Debian/Kali multiarch path
    RAYLIB_CFLAGS :=
    RAYLIB_LFLAGS := -L/usr/lib/x86_64-linux-gnu -lraylib
else
    # Last resort: hope the linker finds it
    RAYLIB_CFLAGS :=
    RAYLIB_LFLAGS := -lraylib
endif

# ── System libs required by Raylib on Linux ────────────────────────────────────
SYS_LIBS := -lssl -lcrypto \
             -lm -lpthread -ldl -lrt \
             -lX11 -lXrandr -lXi -lXcursor -lXinerama \
             -lGL

ALL_LIBS := $(RAYLIB_LFLAGS) $(SYS_LIBS)

# ══════════════════════════════════════════════════════════════════════════════
#  Default target
# ══════════════════════════════════════════════════════════════════════════════

.PHONY: all run cli test audit chaos clean deps raylib-check raylib-src help

all: $(BUILDDIR) $(TARGET)
	@echo ""
	@echo "  ┌──────────────────────────────────────────────────┐"
	@echo "  │  SentinelStorage v3  —  Build SUCCESSFUL  ✓      │"
	@echo "  ├──────────────────────────────────────────────────┤"
	@echo "  │  ./sentinelstorage              GUI mode         │"
	@echo "  │  ./sentinelstorage --cli        CLI mode         │"
	@echo "  │  ./sentinelstorage --audit      Audit report     │"
	@echo "  └──────────────────────────────────────────────────┘"

# ── Link ───────────────────────────────────────────────────────────────────────
$(TARGET): $(OBJS)
	$(CC) $^ -o $@ $(ALL_LIBS)
	@echo "  [link] $@"

# ── Compile each .c → build/%.o  ───────────────────────────────────────────────
# The | build (order-only prerequisite) means: create build/ if missing,
# but don't re-compile everything just because build/'s timestamp changes.
$(BUILDDIR)/%.o: %.c befs.h | $(BUILDDIR)
	$(CC) $(CSTD) $(WARN) $(OPT) $(INCLUDES) $(RAYLIB_CFLAGS) -c $< -o $@
	@echo "  [cc] $<"

# ── Create build directory (order-only target) ─────────────────────────────────
$(BUILDDIR):
	@mkdir -p $(BUILDDIR)
	@echo "  [mkdir] $(BUILDDIR)/"

# ══════════════════════════════════════════════════════════════════════════════
#  Convenience targets
# ══════════════════════════════════════════════════════════════════════════════

run: all
	./$(TARGET)

cli: all
	./$(TARGET) --cli befs.db

audit: all
	./$(TARGET) --audit befs.db

chaos: all
	@printf 'chaos\naudit\nexit\n' | ./$(TARGET) --cli befs.db

TESTDB := sentinel_test.db
test: all
	@echo "═══ Integration Test ═══"
	@rm -f $(TESTDB) $(TESTDB).bak
	@printf 'insert alpha   record_one\n\
insert beta    record_two\n\
insert gamma   record_three\n\
insert delta   record_four\n\
insert epsilon record_five\n\
insert zeta    record_six\n\
insert eta     record_seven\n\
insert theta   record_eight\n\
insert iota    record_nine\n\
insert kappa   record_ten\n\
insert alpha   DUPLICATE_should_fail\n\
search gamma\n\
search nonexistent\n\
delete delta\n\
height\n\
stats\n\
audit\n\
chaos\n\
audit\n\
exit\n' | ./$(TARGET) --cli $(TESTDB)
	@echo "═══ Test Complete ═══"

# ══════════════════════════════════════════════════════════════════════════════
#  Dependency installation
# ══════════════════════════════════════════════════════════════════════════════

deps:
	@echo "Installing system dependencies (requires sudo)..."
	sudo apt-get update -qq
	sudo apt-get install -y \
	    gcc make git \
	    libssl-dev \
	    libasound2-dev \
	    libx11-dev libxrandr-dev libxi-dev \
	    libxcursor-dev libxinerama-dev \
	    libxkbcommon-dev \
	    libgl1-mesa-dev
	@echo "Done. Now run: make raylib-src"

# ── Build Raylib 4.5 from source → /usr/local ──────────────────────────────────
RAYLIB_TAG := 4.5.0
raylib-src:
	@echo "Building Raylib $(RAYLIB_TAG) from source..."
	@which git > /dev/null || (echo "ERROR: git not found. Run: sudo apt install git" && exit 1)
	@rm -rf /tmp/raylib_build
	git clone --depth 1 --branch $(RAYLIB_TAG) \
	    https://github.com/raysan5/raylib.git /tmp/raylib_build
	$(MAKE) -C /tmp/raylib_build/src \
	    PLATFORM=PLATFORM_DESKTOP \
	    RAYLIB_LIBTYPE=SHARED \
	    -j$(shell nproc 2>/dev/null || echo 2)
	sudo $(MAKE) -C /tmp/raylib_build/src install \
	    PLATFORM=PLATFORM_DESKTOP \
	    RAYLIB_LIBTYPE=SHARED
	sudo ldconfig
	@echo ""
	@echo "Raylib installed to /usr/local/lib/"
	@echo "Now run: make"

# ══════════════════════════════════════════════════════════════════════════════
#  Diagnostic
# ══════════════════════════════════════════════════════════════════════════════

raylib-check:
	@echo ""
	@echo "══ Raylib Installation Diagnostic ══"
	@echo ""
	@echo "1. pkg-config:"
	@pkg-config --exists raylib 2>/dev/null \
	    && echo "   ✓ pkg-config finds raylib" \
	    && pkg-config --cflags --libs raylib \
	    || echo "   ✗ pkg-config does NOT find raylib"
	@echo ""
	@echo "2. .so / .a files on disk:"
	@find /usr /usr/local /lib -name 'libraylib*' 2>/dev/null \
	    | head -20 \
	    || echo "   (none found)"
	@echo ""
	@echo "3. raylib.h header:"
	@find /usr /usr/local -name 'raylib.h' 2>/dev/null \
	    | head -5 \
	    || echo "   (none found)"
	@echo ""
	@echo "4. Current Makefile will use:"
	@echo "   RAYLIB_CFLAGS = $(RAYLIB_CFLAGS)"
	@echo "   RAYLIB_LFLAGS = $(RAYLIB_LFLAGS)"
	@echo ""
	@echo "══ Fix Options ══"
	@echo "   A) sudo apt install libraylib-dev    (if available)"
	@echo "   B) make raylib-src                   (build from source)"
	@echo "   C) make RAYLIB_PATH=/your/path       (manual path)"

# ══════════════════════════════════════════════════════════════════════════════
#  Clean
# ══════════════════════════════════════════════════════════════════════════════

clean:
	rm -rf $(BUILDDIR)/ $(TARGET) $(TESTDB) $(TESTDB).bak
	@echo "Cleaned."

help:
	@echo "Targets: all run cli test audit chaos deps raylib-src raylib-check clean"
