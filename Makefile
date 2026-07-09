# =============================================================================
# Blue Notes — Makefile
#
# Builds the Blue Notes application (a GTK3 + SQLite notes app written in
# plain C).  Requires GTK3 and SQLite3, discovered via pkg-config.
#
# On macOS with MacPorts:
#     sudo port install pkgconf gtk3 +quartz
#
# Targets:
#     make          — build the `blue_notes` binary
#     make clean    — remove build artifacts (including dist/)
#     make run      — build and launch the app
#     make app      — macOS .app bundle → dist/BlueNotes-<version>.app
#                     (needs the macOS sips/iconutil tools; the bundle
#                     still depends on the MacPorts GTK libraries)
#     make deb      — Debian package → dist/blue_notes_<version>_<arch>.deb
#                     (needs dpkg-deb; build ON a Debian/Ubuntu machine —
#                     the packaged binary is whatever `make` produced)
#     make rpm      — RPM package → dist/blue_notes-<version>-1.<arch>.rpm
#                     (needs rpmbuild; same caveat as deb)
# =============================================================================

# Semantic version — the single source: it is baked into the binary
# (ON_VERSION, shown in the About dialog) and into every package filename.
VERSION  := 1.5.2

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
            -DON_VERSION='"$(VERSION)"' \
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
            src/cli.c \
            src/db.c \
            src/ipc.c \
            src/serialize.c \
            src/editor_window.c \
            src/library_window.c \
            src/search_window.c \
            src/settings_window.c \
            src/export.c

# Object files derived from the source list (build/ mirrors src/).
OBJS     := $(SRCS:src/%.c=build/%.o)

# The final executable name.
BIN      := blue_notes

# Default target: build the application binary.
all: $(BIN)

# Link all object files into the final binary.
$(BIN): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

# Compile each .c file into a .o in build/, recreating the directory if
# needed.  Every object depends on all headers for simplicity (the project
# is small enough that full rebuilds on header change are cheap), and on
# the Makefile so a VERSION bump recompiles the baked-in ON_VERSION.
build/%.o: src/%.c $(wildcard src/*.h) Makefile
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

# Build and launch the application.
run: $(BIN)
	./$(BIN)

# Remove all build artifacts.
clean:
	rm -rf build $(BIN) $(DIST)

# =============================================================================
# Optional packaging targets — everything lands in dist/.
# =============================================================================

DIST     := dist

# --- macOS .app bundle -------------------------------------------------------
# A minimal bundle around the binary: icons/ and the defaults ini sit next
# to the executable inside Contents/MacOS (the app resolves both relative
# to argv[0]).  vinyl.png becomes the bundle icon via sips + iconutil.
# The binary still links against the MacPorts GTK dylibs (absolute install
# names), so the bundle runs on this machine but is NOT self-contained.

APP_DIR  := $(DIST)/BlueNotes-$(VERSION).app
ICONSET  := $(DIST)/vinyl.iconset

app: $(BIN)
	@command -v iconutil >/dev/null || \
	  { echo "error: iconutil/sips not found — 'make app' is macOS-only"; \
	    exit 1; }
	rm -rf "$(APP_DIR)" "$(ICONSET)"
	mkdir -p "$(APP_DIR)/Contents/MacOS" "$(APP_DIR)/Contents/Resources" \
	         "$(ICONSET)"
	# The executable is named "Blue Notes": for NIB-less apps (the
	# gtkosx menubar is built programmatically) macOS titles the app
	# menu with the PROCESS name, not CFBundleName — the binary's
	# filename is the only lever.  argv[0]-relative lookups (icons,
	# ini) resolve by directory, so the rename is harmless.
	cp $(BIN) "$(APP_DIR)/Contents/MacOS/Blue Notes"
	cp -R icons "$(APP_DIR)/Contents/MacOS/icons"
	cp blue_notes.ini.defaults "$(APP_DIR)/Contents/MacOS/"
	find "$(APP_DIR)" -name .DS_Store -delete
	for sz in 16 32 128 256 512; do \
	  sips -z $$sz $$sz icons/vinyl.png \
	       --out "$(ICONSET)/icon_$${sz}x$${sz}.png" >/dev/null; \
	  dbl=$$((sz * 2)); \
	  sips -z $$dbl $$dbl icons/vinyl.png \
	       --out "$(ICONSET)/icon_$${sz}x$${sz}@2x.png" >/dev/null; \
	done
	iconutil -c icns -o "$(APP_DIR)/Contents/Resources/vinyl.icns" \
	         "$(ICONSET)"
	rm -rf "$(ICONSET)"
	printf '%s\n' \
	  '<?xml version="1.0" encoding="UTF-8"?>' \
	  '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	  '<plist version="1.0">' \
	  '<dict>' \
	  '  <key>CFBundleName</key><string>Blue Notes</string>' \
	  '  <key>CFBundleDisplayName</key><string>Blue Notes</string>' \
	  '  <key>CFBundleIdentifier</key><string>org.example.blue-notes</string>' \
	  '  <key>CFBundleExecutable</key><string>Blue Notes</string>' \
	  '  <key>CFBundleIconFile</key><string>vinyl</string>' \
	  '  <key>CFBundlePackageType</key><string>APPL</string>' \
	  '  <key>CFBundleShortVersionString</key><string>$(VERSION)</string>' \
	  '  <key>CFBundleVersion</key><string>$(VERSION)</string>' \
	  '  <key>NSHighResolutionCapable</key><true/>' \
	  '</dict>' \
	  '</plist>' \
	  > "$(APP_DIR)/Contents/Info.plist"
	@echo "built $(APP_DIR)"

# --- shared Linux package staging ---------------------------------------------
# Both deb and rpm install the whole app to /opt/blue_notes (the binary
# resolves icons/ and its defaults ini relative to argv[0]) and put a
# wrapper script on PATH that execs it by absolute path so that
# resolution works.  Per-user settings fall back to ~/.config/blue_notes/
# because /opt is not writable (see on_app_config_init).

PKGROOT  := $(DIST)/pkgroot

pkgroot: $(BIN)
	rm -rf $(PKGROOT)
	mkdir -p $(PKGROOT)/opt/blue_notes $(PKGROOT)/usr/bin \
	         $(PKGROOT)/usr/share/applications \
	         $(PKGROOT)/usr/share/icons/hicolor/512x512/apps
	cp $(BIN) $(PKGROOT)/opt/blue_notes/
	cp -R icons $(PKGROOT)/opt/blue_notes/icons
	cp blue_notes.ini.defaults $(PKGROOT)/opt/blue_notes/
	find $(PKGROOT) -name .DS_Store -delete
	printf '%s\n' \
	  '#!/bin/sh' \
	  '# Blue Notes finds icons/ and its defaults ini next to argv[0];' \
	  '# exec by absolute path so both resolve into /opt/blue_notes.' \
	  'exec /opt/blue_notes/blue_notes "$$@"' \
	  > $(PKGROOT)/usr/bin/blue_notes
	chmod 755 $(PKGROOT)/usr/bin/blue_notes
	printf '%s\n' \
	  '[Desktop Entry]' \
	  'Type=Application' \
	  'Name=Blue Notes' \
	  'Comment=Notes with folders, tags and rich text' \
	  'Exec=/usr/bin/blue_notes' \
	  'Icon=blue_notes' \
	  'Terminal=false' \
	  'Categories=Utility;Office;' \
	  > $(PKGROOT)/usr/share/applications/blue_notes.desktop
	cp icons/vinyl.png \
	   $(PKGROOT)/usr/share/icons/hicolor/512x512/apps/blue_notes.png

# --- Debian package ------------------------------------------------------------
# The control "Package:" field stays blue-notes: Debian policy (and
# dpkg-deb) forbid "_" in package names.  Every path and filename uses
# blue_notes.
DEB_ARCH := $(shell dpkg --print-architecture 2>/dev/null || \
                    uname -m | sed 's/x86_64/amd64/;s/aarch64/arm64/')
DEB_ROOT := $(DIST)/deb-root

deb: pkgroot
	@command -v dpkg-deb >/dev/null || \
	  { echo "error: dpkg-deb not found — build .deb on a Debian/Ubuntu machine"; \
	    exit 1; }
	rm -rf $(DEB_ROOT)
	cp -R $(PKGROOT) $(DEB_ROOT)
	mkdir -p $(DEB_ROOT)/DEBIAN
	printf '%s\n' \
	  'Package: blue-notes' \
	  'Version: $(VERSION)' \
	  'Section: editors' \
	  'Priority: optional' \
	  'Architecture: $(DEB_ARCH)' \
	  'Depends: libgtk-3-0 | libgtk-3-0t64, libsqlite3-0' \
	  'Maintainer: Ian Campbell <ian@camb.io>' \
	  'Description: Notes app with folders, tags and rich text' \
	  ' Apple Notes-style desktop notes application (GTK3 + SQLite).' \
	  > $(DEB_ROOT)/DEBIAN/control
	dpkg-deb --build --root-owner-group $(DEB_ROOT) \
	  $(DIST)/blue_notes_$(VERSION)_$(DEB_ARCH).deb

# --- RPM package ----------------------------------------------------------------
RPM_ARCH := $(shell uname -m)

rpm: pkgroot
	@command -v rpmbuild >/dev/null || \
	  { echo "error: rpmbuild not found — build .rpm on a Fedora/RHEL machine"; \
	    exit 1; }
	rm -rf $(DIST)/rpm
	mkdir -p $(DIST)/rpm/SPECS
	printf '%s\n' \
	  'Name: blue_notes' \
	  'Version: $(VERSION)' \
	  'Release: 1' \
	  'Summary: Notes app with folders, tags and rich text' \
	  'License: BSD-3-Clause' \
	  '%description' \
	  'Apple Notes-style desktop notes application (GTK3 + SQLite).' \
	  '%install' \
	  'cp -a $(abspath $(PKGROOT))/. %{buildroot}/' \
	  '%files' \
	  '/opt/blue_notes' \
	  '/usr/bin/blue_notes' \
	  '/usr/share/applications/blue_notes.desktop' \
	  '/usr/share/icons/hicolor/512x512/apps/blue_notes.png' \
	  > $(DIST)/rpm/SPECS/blue_notes.spec
	rpmbuild -bb --define "_topdir $(abspath $(DIST))/rpm" \
	  $(DIST)/rpm/SPECS/blue_notes.spec
	cp $(DIST)/rpm/RPMS/*/blue_notes-$(VERSION)-1.*.rpm $(DIST)/

.PHONY: all run clean app pkgroot deb rpm
