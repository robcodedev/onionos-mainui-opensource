# SPDX-License-Identifier: GPL-3.0-only
#
# Open MainUI build.
#
#   make              host development binary (build/MainUI-dev)
#   make check        build and run the whole test suite
#   make device       cross-compile the device launcher (build/onion/MainUI)
#   make check-device cross-compile the self-contained suites for the device
#   make check-asan   the host suites under ASan and UBSan, in build/asan
#   make format       clang-format every project source in place
#   make clean
#
# Every host binary links one static archive, so adding a source file to src/
# needs no change here. Override CC, OPT, SDL_CFLAGS or SDL_LIBS from the
# command line; CROSS_COMPILE and ONION_ROOT configure the device build.

# Local paths (ONION_ROOT, CROSS_COMPILE, SDL locations) belong in config.mk,
# which is not tracked. The leading dash keeps it optional.
-include config.mk

O          ?= build
CC         ?= cc
AR         ?= ar
OPT        ?= -O1 -g
STD        ?= -std=c11
WARN       ?= -Wall -Wextra -Wshadow -Wredundant-decls -Werror
PYTHON     ?= python3
VERSION    ?= 1.0.2

SDL_CFLAGS ?= $(shell sdl-config --cflags 2>/dev/null)
SDL_LIBS   ?= $(shell sdl-config --libs 2>/dev/null) -lSDL_image -lSDL_ttf

CPPFLAGS   += -Isrc -Ivendor/cjson -DCJSON_HIDE_SYMBOLS -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 \
              -DMAINUI_METADATA_TEST
# Applied in the compile rules, not in CPPFLAGS: the device build passes
# CPPFLAGS through a quoted sub-make argument, where nested quotes do not
# survive.
VERSION_DEF := -DMAINUI_VERSION='"$(VERSION)"'
CFLAGS     += $(STD) $(WARN) $(OPT) $(SDL_CFLAGS) -MMD -MP
# EXTRA_LIBS is the only part the device build replaces. LDLIBS itself is never
# overridden: a command-line override would discard this whole line, SDL
# included, and every SDL symbol would come out undefined at link time.
EXTRA_LIBS ?= -lsqlite3
LDLIBS     += $(SDL_LIBS) $(EXTRA_LIBS) -pthread -lm

# ---------------------------------------------------------------- sources

LIB_SRC    := $(filter-out src/app/main.c,$(sort $(shell find src -name '*.c'))) \
              vendor/cjson/cJSON.c
LIB_OBJ    := $(LIB_SRC:%.c=$(O)/obj/%.o)
FAULT_OBJ  := $(LIB_SRC:%.c=$(O)/fault/%.o)

# Suites that need no fixture on disk. Linked into one binary and also
# cross-compiled by check-device.
UNIT_SRC   := tests/main.c \
              tests/test_context.c tests/test_core.c \
              tests/test_input.c tests/test_launch.c tests/test_letter_jump.c \
              tests/test_menu.c tests/test_name_input.c tests/test_options.c \
              tests/test_screen_events.c tests/test_state.c tests/test_timing.c
UNIT_OBJ   := $(UNIT_SRC:%.c=$(O)/obj/%.o)

# Harnesses driven by tests/integration/*.py, which build an SD tree first and
# pass its path in. One binary each; add a name and the file is picked up.
FIXTURES   := allocation_bounds artwork boundaries cache catalog catalog_job desktop device \
              favorite_edit language library recent stock_settings
FIXTURE_BIN := $(FIXTURES:%=$(O)/fixture-%)

HOST_BIN   := $(O)/MainUI-dev $(O)/unit-tests $(O)/core-probe \
              $(O)/persistence-probe $(FIXTURE_BIN)

# ---------------------------------------------------------------- targets

.PHONY: all check check-unit check-integration check-host-deps device \
        check-device format clean help
.DEFAULT_GOAL := all

all: check-host-deps $(O)/MainUI-dev

# Fail with an actionable message instead of dying inside a header.
check-host-deps:
	@mkdir -p $(O)
	@printf '#include <sqlite3.h>\nint main(void){return 0;}\n' > $(O)/.dep-probe.c
	@$(CC) $(O)/.dep-probe.c -lsqlite3 -o $(O)/.dep-probe 2>/dev/null || { \
	    echo "SQLite development files not found."; \
	    echo "  Debian/Ubuntu: sudo apt install libsqlite3-dev"; \
	    echo "  Fedora:        sudo dnf install sqlite-devel"; \
	    echo "This is the host preview. For the device binary run 'make device'."; \
	    rm -f $(O)/.dep-probe.c; exit 1; }
	@rm -f $(O)/.dep-probe.c $(O)/.dep-probe

check: check-unit check-integration

check-unit: $(O)/unit-tests
	$(O)/unit-tests

check-integration: $(HOST_BIN) $(O)/no-hardlinks.so
	ONION_ROOT='$(ONION_ROOT)' $(PYTHON) tests/integration/run.py --build-dir $(O)

format:
	@$(PYTHON) tools/format.py

help:
	@sed -n '3,14p' Makefile | sed 's/^# \?//'

clean:
	rm -rf $(O)

# ---------------------------------------------------------------- compile

$(O)/obj/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(VERSION_DEF) $(CFLAGS) -c $< -o $@

# Same sources with fault injection, for the persistence probe only. Keeping
# them in a separate archive keeps _Exit() out of every other binary.
$(O)/fault/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(VERSION_DEF) -DMAINUI_TEST_FAULTS $(CFLAGS) -c $< -o $@

$(O)/libmainui.a: $(LIB_OBJ)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(O)/libmainui-faults.a: $(FAULT_OBJ)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

# ---------------------------------------------------------------- link

$(O)/MainUI-dev: $(O)/obj/src/app/main.o $(O)/libmainui.a
	$(CC) $^ -o $@ $(LDFLAGS) $(LDLIBS)

$(O)/unit-tests: $(UNIT_OBJ) $(O)/libmainui.a
	$(CC) $^ -o $@ $(LDFLAGS) $(LDLIBS)

$(O)/core-probe: $(O)/obj/tests/probe.o $(O)/libmainui.a
	$(CC) $^ -o $@ $(LDFLAGS) $(LDLIBS)

$(O)/persistence-probe: $(O)/fault/tests/persistence_probe.o $(O)/libmainui-faults.a
	$(CC) $^ -o $@ $(LDFLAGS) $(LDLIBS)

$(O)/fixture-%: $(O)/obj/tests/test_%.o $(O)/libmainui.a
	$(CC) $^ -o $@ $(LDFLAGS) $(LDLIBS)

# ---------------------------------------------------------------- sanitizers

# The whole host suite with AddressSanitizer and UndefinedBehaviorSanitizer,
# built in its own tree so it never mixes with ordinary objects.
SANITIZE ?= -fsanitize=address,undefined -fno-sanitize-recover=undefined \
            -fno-omit-frame-pointer

.PHONY: check-asan
check-asan:
	@ASAN_OPTIONS=$${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1} \
	UBSAN_OPTIONS=$${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1} \
	$(MAKE) --no-print-directory O=$(O)/asan OPT='-O1 -g $(SANITIZE)' \
	    LDFLAGS='$(SANITIZE)' check

# ---------------------------------------------------------------- device

# Toolchain defaults match the Onion miyoomini image; see docs/BUILDING.md.
CROSS_COMPILE    ?= /opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-
ONION_ROOT       ?= /root/workspace/Onion
ONION_SDL_CFLAGS ?= -I/opt/miyoomini-toolchain/arm-linux-gnueabihf/libc/usr/include/SDL
ONION_SDL_LIBS   ?= -lSDL -lSDL_image -lSDL_ttf
ONION_ARCH       := -marm -mtune=cortex-a7 -march=armv7ve -mfpu=neon-vfpv4 -mfloat-abi=hard
# SQLite comes from the Onion runtime instead of vendor/, so the device image
# stays small. libshmvar provides the bootloader variables that About reads.
ONION_CPPFLAGS   := -Isrc -Ivendor/cjson -DCJSON_HIDE_SYMBOLS -D_FILE_OFFSET_BITS=64 \
                    -D_GNU_SOURCE -DMAINUI_ONION \
                    -I$(ONION_ROOT)/include -I$(ONION_ROOT)/include/sqlite3
ONION_OPT        := -O2 -g $(ONION_ARCH) -ffunction-sections -fdata-sections
ONION_LDFLAGS    := -L$(ONION_ROOT)/lib -Wl,--gc-sections \
                    -Wl,-rpath,/mnt/SDCARD/.tmp_update/lib:/mnt/SDCARD/miyoo/lib
ONION_EXTRA_LIBS := $(ONION_ROOT)/lib/libsqlite3.so -lshmvar

# Recursive invocation so every rule above is reused with a different toolchain
# and its own object tree.
ONION_MAKE = $(MAKE) --no-print-directory O=$(O)/onion \
    CC='$(CROSS_COMPILE)gcc' AR='$(CROSS_COMPILE)ar' \
    OPT='$(ONION_OPT)' WARN='-Wall -Wextra -Wshadow -Wredundant-decls -Wstack-usage=8192' \
    CPPFLAGS='$(ONION_CPPFLAGS)' SDL_CFLAGS='$(ONION_SDL_CFLAGS)' \
    SDL_LIBS='$(ONION_SDL_LIBS)' LDFLAGS='$(ONION_LDFLAGS)' \
    EXTRA_LIBS='$(ONION_EXTRA_LIBS)' VERSION='$(VERSION)'

device:
	@command -v $(CROSS_COMPILE)gcc >/dev/null 2>&1 || { \
	    echo "Cross compiler not found: $(CROSS_COMPILE)gcc"; \
	    echo "Set CROSS_COMPILE to the toolchain prefix, e.g."; \
	    echo "  make device CROSS_COMPILE=/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-"; \
	    echo "or put it in config.mk. See docs/BUILDING.md."; exit 1; }
	@for lib in libsqlite3.so libshmvar.so; do \
	    test -f "$(ONION_ROOT)/lib/$$lib" || { \
	        echo "ONION_ROOT=$(ONION_ROOT) has no lib/$$lib"; \
	        echo "Point ONION_ROOT at an Onion checkout whose libraries are built."; \
	        exit 1; }; \
	done
	@test -f "$(ONION_ROOT)/include/sqlite3/sqlite3.h" || { \
	    echo "ONION_ROOT=$(ONION_ROOT) has no include/sqlite3/sqlite3.h"; exit 1; }
	@$(ONION_MAKE) $(O)/onion/MainUI

check-device:
	@$(ONION_MAKE) $(O)/onion/unit-tests
	@echo "Copy $(O)/onion/unit-tests onto the device and run it there."

# Only ever built through ONION_MAKE, where $(O) is already build/onion. Keeps
# an unstripped copy, then checks the stripped image is a 32-bit ARM hard-float
# ELF with no build-machine paths left in DT_NEEDED.
$(O)/MainUI: $(O)/obj/src/app/main.o $(O)/libmainui.a
	$(CC) $^ -o $@ $(LDFLAGS) $(LDLIBS)
	cp -p $@ $@.unstripped
	$(CROSS_COMPILE)strip --strip-unneeded $@
	$(PYTHON) tools/verify_elf.py $@
	@echo "Device binary: $$(stat -c%s $@) bytes (unstripped $$(stat -c%s $@.unstripped))"

-include $(shell find $(O) -name '*.d' 2>/dev/null)

# Linux regressions: forbid hard links on the host; CI also exercises real vfat.
.PHONY: check-no-hardlinks check-vfat
$(O)/no-hardlinks.so: tests/no_hardlinks.c
	@mkdir -p $(@D)
	$(CC) $(STD) $(WARN) -shared -fPIC $< -o $@

check-no-hardlinks: $(HOST_BIN) $(O)/no-hardlinks.so
	LD_PRELOAD='$(abspath $(O)/no-hardlinks.so)' ONION_ROOT='$(ONION_ROOT)' $(PYTHON) tests/integration/run.py --build-dir $(O) fat_delete persistence favorite_edit

check-vfat: $(HOST_BIN) $(O)/no-hardlinks.so
	sh tests/vfat.sh $(O)
