# =============================================================================
# Orange Notes — Makefile
#
# Builds the Orange Notes application (a GTK3 + SQLite notes app written in
# plain C).  Requires GTK3 and SQLite3, discovered via pkg-config.
#
# On macOS with MacPorts:
#     sudo port install pkgconf gtk3 +quartz
#
# Targets:
#     make          — build the `orange_notes` binary
#     make clean    — remove build artifacts
#     make run      — build and launch the app
# =============================================================================

# The compiler to use.  clang is the system compiler on macOS.
CC       := cc

# pkg-config binary.  MacPorts installs into /opt/local/bin, which may not be
# on PATH in every shell, so fall back to the absolute path if needed.
PKGCONF  := $(shell command -v pkg-config 2>/dev/null || echo /opt/local/bin/pkg-config)

# Compiler flags:
#   -std=c11        — use the C11 language standard
#   -Wall -Wextra   — enable a broad set of warnings
#   -g              — include debug symbols
#   plus the include paths for GTK3 and SQLite3 from pkg-config.
CFLAGS   := -std=c11 -Wall -Wextra -g \
            $(shell $(PKGCONF) --cflags gtk+-3.0 sqlite3)

# Linker flags: the GTK3 and SQLite3 libraries from pkg-config, plus libm.
LDFLAGS  := $(shell $(PKGCONF) --libs gtk+-3.0 sqlite3) -lm

# Optional macOS menu-bar integration (MacPorts: gtk-osx-application-gtk3).
# When present, the Settings window offers moving the menu into the native
# macOS menu bar; without it the option shows as unavailable.
HAVE_GTKOSX := $(shell $(PKGCONF) --exists gtk-mac-integration-gtk3 && echo 1)
ifeq ($(HAVE_GTKOSX),1)
CFLAGS  += -DHAVE_GTKOSX $(shell $(PKGCONF) --cflags gtk-mac-integration-gtk3)
LDFLAGS += $(shell $(PKGCONF) --libs gtk-mac-integration-gtk3)
endif

# All C source files that make up the application.
SRCS     := src/main.c \
            src/app.c \
            src/db.c \
            src/serialize.c \
            src/editor_window.c \
            src/library_window.c \
            src/search_window.c \
            src/settings_window.c \
            src/export.c

# Object files derived from the source list (build/ mirrors src/).
OBJS     := $(SRCS:src/%.c=build/%.o)

# The final executable name.
BIN      := orange_notes

# Default target: build the application binary.
all: $(BIN)

# Link all object files into the final binary.
$(BIN): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

# Compile each .c file into a .o in build/, recreating the directory if
# needed.  Every object depends on all headers for simplicity (the project
# is small enough that full rebuilds on header change are cheap).
build/%.o: src/%.c $(wildcard src/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

# Build and launch the application.
run: $(BIN)
	./$(BIN)

# Remove all build artifacts.
clean:
	rm -rf build $(BIN)

.PHONY: all run clean
