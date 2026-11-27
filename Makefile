# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# Objectspace: object core.
#
#   make          build the runtime, the core classes and the tests
#   make test     run the unit tests (with AddressSanitizer and UBSan if installed)
#   make check    tests + strict C11 core check + ThreadSanitizer loop run
#   make m2       M2 scenarios with real services on loopback filesystems (sudo)
#   make m3       M3 change-tracking scenarios, inotify and osd-watchd modes (sudo)
#   make m0       build the M0 prototypes (see proto/m0)
#   make g0       build the G0 GUI prototypes (see proto/g0; optimised, no sanitizers)
#   make gui      build osd-shell (optimised) and osd-shell-asan (sanitizers)
#   make session  everything the Objectspace session runs
#   make install  DESTDIR=... PREFIX=/usr (build with the same PREFIX); make uninstall
#   make install-check  install into a scratch root and run a session from it
#   make dist | srpm | rpm  the tarball and spec, the source RPM, the RPMs (build/rpm)
#   make clean

CC      ?= cc
# The version (VERSION), in every program (--version, osdd's ping).
OSD_VERSION := $(shell cat VERSION 2>/dev/null || echo unknown)
# Where make install puts things (v0.2-design.md 11.1); run from the tree,
# the programs find each other in build/ as before.
PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
LIBEXECDIR ?= $(PREFIX)/libexec/objectspace
LIBDIR     ?= $(PREFIX)/lib64
DATADIR    ?= $(PREFIX)/share
INCLUDEDIR ?= $(PREFIX)/include
MANDIR     ?= $(DATADIR)/man
SYSCONFDIR ?= /etc
USERUNITDIR   ?= $(PREFIX)/lib/systemd/user
SYSTEMUNITDIR ?= $(PREFIX)/lib/systemd/system
VFLAGS  := -DOSD_VERSION='"$(OSD_VERSION)"' -DOSD_PROTOCOL=3 -DOSD_PLUGINDIR='"$(LIBDIR)/objectspace/plugins"' -DOSD_ICONDIR='"$(DATADIR)/objectspace/icons"'
PYTHON  ?= python3
B       := build
GEN     := $(B)/gen
CFLAGS  ?= -O1 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
# Checked static calls (osdo_slot verifies the class) in these builds.
CFLAGS  += -DWPO_CHECKED $(VFLAGS)
# AddressSanitizer and UBSan when their runtimes are installed (Fedora:
# libasan, libubsan); otherwise a plain build. Override with SAN=.
SAN     ?= $(shell echo 'int main(void){return 0;}' | $(CC) -fsanitize=address,undefined -x c - \
             -o /dev/null 2>/dev/null && echo -fsanitize=address,undefined -fno-omit-frame-pointer)
INC     := -Iruntime/include -Iclasses -Iservice -I$(GEN)

RUNTIME := runtime/src/osdo.c runtime/src/osdo_loop_posix.c runtime/src/osdo_plugin_posix.c
OSDI     := $(wildcard classes/*.osdi)
CLASSES := $(wildcard classes/*.c) service/attrs.c $(patsubst classes/%.osdi,$(GEN)/%.reg.c,$(OSDI))

# Test class TBase: version 1.1 is "installed"; plug-ins are built against
# 1.0 (older, must load), 1.2 (newer, refused) and 2.0 (other major, refused).
TV      := $(B)/tests

all: $(B)/osdd $(B)/osdctl $(B)/osd $(B)/osd-watchd $(B)/osd-plughost $(TV)/test_runtime $(TV)/test_loop $(TV)/test_attrs $(TV)/test_db $(TV)/test_cbor $(TV)/test_launch $(TV)/test_magic $(TV)/test_ui_damage plugins

$(GEN)/.stamp: $(OSDI) tools/osdc.py
	$(PYTHON) tools/osdc.py -I classes -o $(GEN) $(OSDI)
	@touch $@

$(GEN)/%.reg.c: $(GEN)/.stamp ;

$(B)/gen-%/.stamp: tests/versions/%/TBase.osdi tests/plugins/TChild.osdi tools/osdc.py
	$(PYTHON) tools/osdc.py -o $(B)/gen-$* $<
	$(PYTHON) tools/osdc.py -I tests/versions/$* -o $(B)/gen-$*/plugin tests/plugins/TChild.osdi
	@touch $@

$(TV)/test_runtime: tests/test_runtime.c $(RUNTIME) $(GEN)/.stamp $(B)/gen-v11/.stamp tests/TBase_v11.c $(CLASSES) $(wildcard classes/*.h)
	@mkdir -p $(TV)
	$(CC) $(CFLAGS) $(SAN) $(INC) -I$(B)/gen-v11 -rdynamic -o $@ tests/test_runtime.c $(RUNTIME) \
	    $(CLASSES) tests/TBase_v11.c $(B)/gen-v11/TBase.reg.c -ldl -lpthread

$(TV)/test_loop: tests/test_loop.c runtime/src/osdo_loop_posix.c runtime/include/osdo_loop.h
	@mkdir -p $(TV)
	$(CC) $(CFLAGS) $(SAN) $(INC) -o $@ tests/test_loop.c runtime/src/osdo_loop_posix.c -lpthread

$(TV)/test_attrs: tests/test_attrs.c service/attrs.c service/attrs.h
	@mkdir -p $(TV)
	$(CC) $(CFLAGS) $(SAN) -Iservice -Itests -o $@ tests/test_attrs.c service/attrs.c

$(TV)/test_db: tests/test_db.c service/db.c service/db.h service/attrs.c
	@mkdir -p $(TV)
	$(CC) $(CFLAGS) $(SAN) -Iservice -Itests -o $@ tests/test_db.c service/db.c service/attrs.c -lsqlite3 -lpthread

$(TV)/test_ui_damage: tests/test_ui_damage.c gui/ui_damage.c gui/ui_damage.h
	@mkdir -p $(TV)
	$(CC) $(CFLAGS) $(SAN) -Igui -Itests -o $@ tests/test_ui_damage.c gui/ui_damage.c

$(TV)/test_magic: tests/test_magic.c classes/osd_magic.c classes/osd_magic.h
	@mkdir -p $(TV)
	$(CC) $(CFLAGS) $(SAN) $(INC) -Itests -o $@ tests/test_magic.c classes/osd_magic.c

$(TV)/test_launch: tests/test_launch.c service/launch.c service/launch.h classes/osd_magic.c
	@mkdir -p $(TV)
	$(CC) $(CFLAGS) $(SAN) $(INC) -Itests -o $@ tests/test_launch.c service/launch.c classes/osd_magic.c

$(TV)/test_cbor: tests/test_cbor.c service/cbor.c service/cbor.h
	@mkdir -p $(TV)
	$(CC) $(CFLAGS) $(SAN) -Iservice -Itests -o $@ tests/test_cbor.c service/cbor.c

SERVICE := service/osdd.c service/osdd_meta.c service/osdd_events.c service/osdd_settings.c service/osdd_jobs.c service/osdd_open.c service/osdd_layout.c service/osdd_drives.c service/osdd_setup.c service/osdd_ops.c service/osdd_props.c service/osdd_actions.c service/osdd_backing.c service/osdd_workarea.c service/defaults.c service/objects.c service/tracker.c service/shadows.c service/listing.c service/find.c service/launch.c service/trash.c service/jobs.c service/plugins.c service/db.c service/env.c service/volumes.c service/udisks.c service/programs.c service/messages.c service/startup.c service/cbor.c

$(B)/osdd: $(SERVICE) $(RUNTIME) $(GEN)/.stamp $(wildcard service/*.h classes/*.c classes/*.h)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(SAN) $(INC) -rdynamic -o $@ $(SERVICE) $(RUNTIME) $(CLASSES) $(LDFLAGS) -lsqlite3 -ldl -lpthread -lsystemd

$(B)/osd-plughost: service/plughost.c service/cbor.c $(RUNTIME) $(GEN)/.stamp $(wildcard classes/*.c classes/*.h)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(SAN) $(INC) -rdynamic -o $@ service/plughost.c service/cbor.c $(RUNTIME) $(CLASSES) $(LDFLAGS) -ldl -lpthread

$(B)/osd-watchd: service/watchd.c service/cbor.c service/cbor.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(SAN) -Iservice -o $@ service/watchd.c service/cbor.c $(LDFLAGS)

# (with the icon packages' code: `osdctl icon` works on files, roadmap 0.4)
# osd: Objectspace for scripts (docs/cli-design.md 2), plain text
$(B)/osd: service/osd.c service/cbor.c service/cbor.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(SAN) -Iservice -o $@ service/osd.c service/cbor.c $(LDFLAGS) -lsystemd

$(B)/osdctl: service/osdctl.c service/cbor.c service/cbor.h gui/shell/osdicon.c gui/shell/osdicon.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(SAN) -Iservice -Igui/shell $$(pkg-config --cflags cairo zlib librsvg-2.0) -o $@ service/osdctl.c service/cbor.c gui/shell/osdicon.c $(LDFLAGS) $$(pkg-config --libs cairo zlib librsvg-2.0)

# M5: a plug-in for osd-plughost, and one built against a newer OsdFolder
# (1.1) than the one installed, which must be refused.
M5PLUG := $(TV)/m5plugins/testfolder.so $(TV)/m5future/future.so

$(TV)/m5plugins/testfolder.so: tests/plugins/TestFolder.c tests/plugins/TestFolder.osdi $(GEN)/.stamp
	@mkdir -p $(TV)/m5plugins $(B)/gen-testfolder
	$(PYTHON) tools/osdc.py -I classes -o $(B)/gen-testfolder tests/plugins/TestFolder.osdi
	$(CC) $(CFLAGS) $(SAN) -fPIC -shared $(INC) -I$(B)/gen-testfolder \
	    -o $@ tests/plugins/TestFolder.c $(B)/gen-testfolder/TestFolder.reg.c

$(TV)/m5future/future.so: tests/plugins/TestFolder.c tests/plugins/TestFolder.osdi $(GEN)/.stamp
	@mkdir -p $(TV)/m5future $(B)/gen-future/classes $(B)/gen-future/out
	cp classes/*.osdi $(B)/gen-future/classes/
	sed -i 's/^version 1.0$$/version 1.1/' $(B)/gen-future/classes/OsdFolder.osdi
	$(PYTHON) tools/osdc.py -I $(B)/gen-future/classes -o $(B)/gen-future/out $(B)/gen-future/classes/*.osdi tests/plugins/TestFolder.osdi
	$(CC) $(CFLAGS) $(SAN) -fPIC -shared -Iruntime/include -Iclasses -Iservice -I$(B)/gen-future/out \
	    -o $@ tests/plugins/TestFolder.c $(B)/gen-future/out/TestFolder.reg.c

PLUGINS := $(M5PLUG) $(TV)/plugins/tchild_v10.so $(TV)/plugins/tchild_v12.so $(TV)/plugins/tchild_v20.so \
           $(TV)/plugins/myfolder.so $(TV)/sample/osdimagefile.so
plugins: $(PLUGINS)

$(TV)/plugins/tchild_%.so: tests/plugins/TChild.c $(B)/gen-%/.stamp
	@mkdir -p $(TV)/plugins
	$(CC) $(CFLAGS) $(SAN) -fPIC -shared -Iruntime/include -Iservice -I$(B)/gen-$*/plugin -I$(B)/gen-$* \
	    -o $@ tests/plugins/TChild.c $(B)/gen-$*/plugin/TChild.reg.c

# The SDK's sample plug-in (sdk/sample), as its users build it.
$(TV)/sample/osdimagefile.so: sdk/sample/OsdImageFile.c sdk/sample/OsdImageFile.osdi $(GEN)/.stamp
	@mkdir -p $(TV)/sample $(B)/gen-sample
	$(PYTHON) tools/osdc.py -I classes -o $(B)/gen-sample sdk/sample/OsdImageFile.osdi
	$(CC) $(CFLAGS) $(SAN) -fPIC -shared $(INC) -I$(B)/gen-sample \
	    -o $@ sdk/sample/OsdImageFile.c $(B)/gen-sample/OsdImageFile.reg.c

$(TV)/plugins/myfolder.so: tests/plugins/MyFolder.c tests/plugins/MyFolder.osdi $(GEN)/.stamp
	@mkdir -p $(TV)/plugins $(B)/gen-myfolder
	$(PYTHON) tools/osdc.py -I classes -o $(B)/gen-myfolder tests/plugins/MyFolder.osdi
	$(CC) $(CFLAGS) $(SAN) -fPIC -shared $(INC) -I$(B)/gen-myfolder \
	    -o $@ tests/plugins/MyFolder.c $(B)/gen-myfolder/MyFolder.reg.c

# (unit-test: the unit tests only, for the RPM's %check: the scenarios
# below need a user session's runtime folder and bus)
unit-test: all
	$(TV)/test_runtime $(TV)/plugins
	$(TV)/test_loop
	$(TV)/test_attrs $(TV)/attrs-tmp
	$(TV)/test_db $(TV)/db-tmp
	$(TV)/test_cbor
	$(TV)/test_launch $(TV)/launch-tmp
	$(TV)/test_magic
	$(TV)/test_ui_damage

test: unit-test
	tests/drives/run.sh
	tests/programs/run.sh
	tests/messages/run.sh
	tests/firststart/run.sh
	tests/openwith/run.sh
	tests/props/run.sh $(B)/props/last
	tests/plugins/run.sh
	tests/plugins/sdk.sh

# Everything that must stay true: the unit tests (with ASan/UBSan when
# installed), the core building as strict C11 with only the C library, and
# the loop under ThreadSanitizer when libtsan is installed.
CORE_LIBC := abort calloc free malloc memcpy realloc strchr strcmp strlen
TSAN_OK   := $(shell echo 'int main(void){return 0;}' | $(CC) -fsanitize=thread -x c - -o /dev/null 2>/dev/null && echo yes)

check: test
	@mkdir -p $(TV)
	$(CC) -std=c11 -Wall -Wextra -pedantic -Werror -Iruntime/include -c runtime/src/osdo.c -o $(TV)/osdo_c11.o
	@extra=$$(nm -u $(TV)/osdo_c11.o | awk '{print $$2}' | grep -vxE '$(subst $(eval) ,|,$(CORE_LIBC))'); \
	 if [ -n "$$extra" ]; then echo "core uses more than the C library subset: $$extra"; exit 1; fi; \
	 echo "core: strict C11, C library subset only"
	@if [ -d .git ]; then $(PYTHON) tools/spdx.py; else echo "not a git checkout: licence lines not checked"; fi
ifeq ($(TSAN_OK),yes)
	$(CC) -O1 -g -std=gnu11 -fsanitize=thread $(INC) -o $(TV)/test_loop_tsan tests/test_loop.c runtime/src/osdo_loop_posix.c -lpthread
	$(TV)/test_loop_tsan
else
	@echo "ThreadSanitizer not installed (Fedora: libtsan); loop race check skipped"
endif

# M2 scenarios against the loopback test bed (needs sudo; see proto/m0/testbed.sh).
m2: all
	proto/m0/testbed.sh down
	proto/m0/testbed.sh up
	tests/m2/m2.py; rc=$$?; proto/m0/testbed.sh down; exit $$rc

# M3 scenarios in both tracking modes (inotify, and osd-watchd as root).
m3: all
	proto/m0/testbed.sh down
	proto/m0/testbed.sh up
	tests/m3/m3.py; rc=$$?; proto/m0/testbed.sh down; exit $$rc

m0:
	$(MAKE) -C proto/m0 all

# G0: GUI prototypes. Optimised and without sanitizers, since they measure
# frame and sound timing. Needs cairo, harfbuzz, freetype, fontconfig,
# wayland, xkbcommon and pipewire development packages.
G0      := $(B)/g0
# OPTFLAGS: the distribution's (the RPM's %{optflags}), after ours.
G0FLAGS := -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation $(VFLAGS) $(OPTFLAGS)
XDGXML  := $(shell pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null)/stable/xdg-shell/xdg-shell.xml

$(G0)/xdg-shell-client-protocol.h: $(XDGXML)
	@mkdir -p $(G0)
	wayland-scanner client-header $< $@

$(G0)/xdg-shell-protocol.c: $(XDGXML)
	@mkdir -p $(G0)
	wayland-scanner private-code $< $@

VPXML   := $(shell pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null)/stable/viewporter/viewporter.xml
ACTXML  := $(shell pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null)/staging/xdg-activation/xdg-activation-v1.xml

$(G0)/xdg-activation-v1-client-protocol.h: $(ACTXML)
	@mkdir -p $(G0)
	wayland-scanner client-header $< $@

$(G0)/xdg-activation-v1-protocol.c: $(ACTXML)
	@mkdir -p $(G0)
	wayland-scanner private-code $< $@

TIXML   := $(shell pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null)/unstable/text-input/text-input-unstable-v3.xml

$(G0)/text-input-unstable-v3-client-protocol.h: $(TIXML)
	@mkdir -p $(G0)
	wayland-scanner client-header $< $@

$(G0)/text-input-unstable-v3-protocol.c: $(TIXML)
	@mkdir -p $(G0)
	wayland-scanner private-code $< $@

$(G0)/viewporter-client-protocol.h: $(VPXML)
	@mkdir -p $(G0)
	wayland-scanner client-header $< $@

$(G0)/viewporter-protocol.c: $(VPXML)
	@mkdir -p $(G0)
	wayland-scanner private-code $< $@

$(G0)/folderview: proto/g0/folderview.c service/cbor.c $(G0)/xdg-shell-client-protocol.h $(G0)/xdg-shell-protocol.c
	$(CC) $(G0FLAGS) -Iservice -I$(G0) -o $@ proto/g0/folderview.c service/cbor.c $(G0)/xdg-shell-protocol.c \
	    $$(pkg-config --cflags --libs cairo harfbuzz freetype2 fontconfig wayland-client xkbcommon) -lpthread -lm

$(G0)/soundlat: proto/g0/soundlat.c
	@mkdir -p $(G0)
	$(CC) $(G0FLAGS) -o $@ $< $$(pkg-config --cflags --libs libpipewire-0.3) -lm

g0: $(G0)/folderview $(G0)/soundlat

# The GUI: the widget set (gui/ui_*.c) and osd-shell. Optimised for timing; the
# -asan build is for finding memory errors.
UISRC   := gui/ui_app.c gui/ui_draw.c gui/ui_damage.c gui/ui_frame.c gui/ui_framepaint.c gui/ui_menu.c gui/ui_menupaint.c gui/ui_container.c gui/ui_layer.c gui/ui_entry.c gui/ui_mouse.c gui/ui_notebook.c gui/ui_dnd.c gui/ui_dialog.c gui/ui_textinput.c gui/ui_ibus.c gui/ui_a11y.c
SHELLSRC := gui/shell/osd-shell.c gui/shell/folderwin.c gui/shell/tree.c gui/shell/iconview.c gui/shell/properties.c gui/shell/menus.c gui/shell/dnd.c gui/shell/jobs.c gui/shell/window.c gui/shell/selftest.c gui/shell/setup.c gui/shell/desktop.c gui/shell/svc.c gui/shell/layout.c gui/shell/wallpaper.c gui/shell/events.c gui/shell/sound.c gui/shell/gnome.c gui/shell/winlist.c gui/shell/launcher.c gui/shell/session.c gui/shell/thebar.c gui/shell/trays.c gui/shell/menubar.c gui/shell/columns.c gui/shell/thumbs.c gui/shell/md5.c gui/shell/gallery.c gui/shell/notify.c gui/shell/messages.c gui/shell/tray.c gui/shell/findwin.c gui/shell/icons.c gui/shell/appicons.c gui/shell/iconpick.c gui/shell/filepick.c gui/shell/apppick.c gui/shell/fontpal.c gui/shell/colorpal.c gui/shell/screens.c gui/shell/workarea.c gui/shell/objmenu.c gui/shell/tooltip.c gui/shell/mime.c gui/shell/osdicon.c classes/osd_magic.c classes/osd_themes.c service/cbor.c $(G0)/xdg-shell-protocol.c $(G0)/viewporter-protocol.c $(G0)/xdg-activation-v1-protocol.c $(G0)/text-input-unstable-v3-protocol.c
UIPKG   := cairo harfbuzz freetype2 fontconfig wayland-client wayland-cursor xkbcommon libsystemd
UIDEPS  := $(UISRC) $(SHELLSRC) $(wildcard gui/*.h gui/shell/*.h) classes/osd_menu_ids.h $(G0)/xdg-shell-client-protocol.h $(G0)/viewporter-client-protocol.h $(G0)/xdg-activation-v1-client-protocol.h $(G0)/text-input-unstable-v3-client-protocol.h
UIFLAGS = -Igui -Igui/shell -Iservice -Iclasses -I$(G0) $(shell pkg-config --cflags $(UIPKG))
UILIBS  = $(shell pkg-config --libs $(UIPKG)) -lpthread -lm
# Sounds: PipeWire, and libsndfile when installed (Ogg, FLAC; else WAV only).
SNDFILE := $(shell pkg-config --exists sndfile 2>/dev/null && echo sndfile)
SHFLAGS = $(shell pkg-config --cflags libpipewire-0.3 $(SNDFILE) librsvg-2.0 zlib) $(if $(SNDFILE),-DHAVE_SNDFILE)
SHLIBS  = $(shell pkg-config --libs libpipewire-0.3 $(SNDFILE) librsvg-2.0 zlib) -lsystemd

$(B)/osd-shell: $(UIDEPS)
	$(CC) $(G0FLAGS) $(UIFLAGS) $(SHFLAGS) -o $@ $(UISRC) $(SHELLSRC) $(LDFLAGS) $(UILIBS) $(SHLIBS)

$(B)/osd-shell-asan: $(UIDEPS)
	$(CC) $(G0FLAGS) $(SAN) $(UIFLAGS) $(SHFLAGS) -o $@ $(UISRC) $(SHELLSRC) $(UILIBS) $(SHLIBS)

# Objectspace Terminal (docs/cli-design.md): the toolkit and libvterm.
TERMSRC := gui/terminal/osd-terminal.c gui/terminal/term_core.c gui/terminal/term_setup.c service/cbor.c $(G0)/xdg-shell-protocol.c $(G0)/viewporter-protocol.c $(G0)/xdg-activation-v1-protocol.c $(G0)/text-input-unstable-v3-protocol.c
TERMLIBS = $(shell pkg-config --libs vterm) -lutil
$(B)/osd-terminal: $(UIDEPS) $(TERMSRC) gui/terminal/term_core.h gui/terminal/term_setup.h
	$(CC) $(G0FLAGS) $(UIFLAGS) -Igui/terminal -o $@ $(UISRC) $(TERMSRC) $(LDFLAGS) $(UILIBS) $(TERMLIBS)

$(B)/osd-terminal-asan: $(UIDEPS) $(TERMSRC) gui/terminal/term_core.h
	$(CC) $(G0FLAGS) $(SAN) $(UIFLAGS) -Igui/terminal -o $@ $(UISRC) $(TERMSRC) $(UILIBS) $(TERMLIBS)

$(TV)/test_term: tests/test_term.c gui/terminal/term_core.c gui/terminal/term_core.h
	@mkdir -p $(TV)
	$(CC) $(G0FLAGS) $(SAN) $(UIFLAGS) -Igui/terminal -Itests -o $@ tests/test_term.c gui/terminal/term_core.c $(UILIBS) $(TERMLIBS)

# G5: the compositor (wlroots 0.20). It draws frames with the toolkit's
# frame code (gui/ui_framepaint.c), so it links the drawing part of it.
COMPPKG := wlroots-0.20 wayland-server xkbcommon pixman-1 libdrm cairo harfbuzz freetype2 fontconfig xcb libsystemd
# The lock screen checks passwords with PAM (pam-devel); without it, no locking.
COMPPAM := $(shell test -f /usr/include/security/pam_appl.h && echo -DHAVE_PAM)
COMPSRC := compositor/osd-compositor.c compositor/desktop.c compositor/lock.c compositor/xscale.c $(G0)/xdg-output-unstable-v1-protocol.c gui/ui_draw.c gui/ui_framepaint.c gui/ui_menupaint.c gui/ui_mouse.c compositor/idle.c compositor/keymap.c
DECOXML := $(shell pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
$(G0)/xdg-decoration-unstable-v1-client-protocol.h: $(DECOXML)
	@mkdir -p $(G0)
	wayland-scanner client-header $< $@
$(G0)/xdg-decoration-unstable-v1-protocol.c: $(DECOXML)
	@mkdir -p $(G0)
	wayland-scanner private-code $< $@

# xdg-output for XWayland alone (compositor/xscale.c): the server side.
XOUTXML := $(shell pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null)/unstable/xdg-output/xdg-output-unstable-v1.xml
$(G0)/xdg-output-unstable-v1-protocol.h: $(XOUTXML)
	@mkdir -p $(G0)
	wayland-scanner server-header $< $@
$(G0)/xdg-output-unstable-v1-protocol.c: $(XOUTXML)
	@mkdir -p $(G0)
	wayland-scanner private-code $< $@

# Everything the Objectspace session runs (tools/osd-session): after a pull, make session.
session: all $(B)/osd-shell $(B)/osd-terminal $(B)/osd-compositor icons
.PHONY: session

compositor: $(B)/osd-compositor $(TV)/ssd-client $(TV)/x11-client
# An X11 test client (XWayland).
$(TV)/x11-client: tests/comp/x11-client.c
	@mkdir -p $(TV)
	$(CC) $(G0FLAGS) -o $@ $< $$(pkg-config --cflags --libs xcb)
# A test client that asks for a server-side frame (tests/comp).
$(TV)/ssd-client: tests/comp/ssd-client.c $(G0)/xdg-shell-client-protocol.h $(G0)/xdg-shell-protocol.c $(G0)/xdg-decoration-unstable-v1-client-protocol.h $(G0)/xdg-decoration-unstable-v1-protocol.c
	@mkdir -p $(TV)
	$(CC) $(G0FLAGS) -I$(G0) -o $@ tests/comp/ssd-client.c $(G0)/xdg-shell-protocol.c $(G0)/xdg-decoration-unstable-v1-protocol.c $$(pkg-config --cflags --libs wayland-client)
$(B)/osd-compositor: $(COMPSRC) gui/ui.h compositor/comp.h $(G0)/xdg-output-unstable-v1-protocol.h
	@mkdir -p $(B)
	$(CC) $(G0FLAGS) -DWLR_USE_UNSTABLE -DHAVE_SD_BUS $(COMPPAM) -Igui -I$(G0) $$(pkg-config --cflags $(COMPPKG)) -o $@ $(COMPSRC) $(LDFLAGS) $$(pkg-config --libs $(COMPPKG)) $(if $(COMPPAM),-lpam) -lpthread -lm

$(TV)/test_ui_entry: tests/test_ui_entry.c gui/ui_entry.c gui/ui_draw.c gui/ui.h
	@mkdir -p $(TV)
	$(CC) $(G0FLAGS) $(SAN) $(UIFLAGS) -Itests -o $@ tests/test_ui_entry.c gui/ui_entry.c gui/ui_draw.c $(UILIBS)

$(TV)/test_ui_font: tests/test_ui_font.c gui/ui_draw.c gui/ui.h
	@mkdir -p $(TV)
	$(CC) $(G0FLAGS) $(SAN) $(UIFLAGS) -Itests -o $@ tests/test_ui_font.c gui/ui_draw.c $(UILIBS)

$(TV)/test_ui_mouse: tests/test_ui_mouse.c gui/ui_mouse.c gui/ui.h
	@mkdir -p $(TV)
	$(CC) $(G0FLAGS) $(SAN) $(UIFLAGS) -Itests -o $@ tests/test_ui_mouse.c gui/ui_mouse.c $(UILIBS)

$(TV)/test_layout: tests/test_layout.c gui/shell/layout.c gui/shell/layout.h
	@mkdir -p $(TV)
	$(CC) $(G0FLAGS) $(SAN) -Igui/shell -Itests -o $@ tests/test_layout.c gui/shell/layout.c -lm

$(TV)/test_osdicon: tests/test_osdicon.c gui/shell/osdicon.c gui/shell/osdicon.h
	@mkdir -p $(TV)
	$(CC) $(G0FLAGS) $(SAN) -Igui/shell -Itests $$(pkg-config --cflags cairo zlib librsvg-2.0) -o $@ tests/test_osdicon.c gui/shell/osdicon.c $$(pkg-config --libs cairo zlib librsvg-2.0)

# Icon themes (roadmap 0.4): Objectspace's isometric set as packages, a theme per
# variant (tools/icons/make_theme.py, with Pillow: objectspace-flat, the default,
# and objectspace-outline), and 0.2's pictures as the classic theme (packed by
# osdctl icon pack, which adds the standard sizes).
ICONTHEMES := $(B)/icon-themes
icons: $(ICONTHEMES)/.stamp
$(ICONTHEMES)/.stamp: tools/icons/make_iso.py tools/icons/make_theme.py $(wildcard data/icons/*.png) $(B)/osdctl
	$(PYTHON) tools/icons/make_theme.py $(ICONTHEMES)
	@mkdir -p $(ICONTHEMES)/classic
	for f in data/icons/*.png; do $(B)/osdctl icon pack "$$f" "$(ICONTHEMES)/classic/$$(basename "$$f" .png).osdicon" > /dev/null || exit 1; done
	printf '[Icon Theme]\nName=Classic\nComment=Objectspace 0.2 pictures\nVariant=classic\n' > $(ICONTHEMES)/classic/theme.ini
	@touch $@
.PHONY: icons

gui: $(B)/osd-shell $(B)/osd-shell-asan $(B)/osd-terminal $(B)/osd-terminal-asan $(TV)/test_term $(TV)/test_ui_entry $(TV)/test_ui_font $(TV)/test_ui_mouse $(TV)/test_layout $(TV)/test_osdicon $(TV)/test_md5

# GUI unit tests (need the GUI's -devel packages, unlike make test)
$(TV)/test_md5: tests/test_md5.c gui/shell/md5.c gui/shell/md5.h
	@mkdir -p $(TV)
	$(CC) $(G0FLAGS) $(SAN) -Igui/shell -Itests -o $@ tests/test_md5.c gui/shell/md5.c

gui-test: gui
	$(TV)/test_ui_damage
	$(TV)/test_ui_entry
	$(TV)/test_ui_font
	$(TV)/test_term
	$(TV)/test_ui_mouse
	$(TV)/test_layout
	$(TV)/test_osdicon
	$(TV)/test_md5

# The headless compositor tests (tests/comp), one after another.
COMP_TESTS := background clipboard colour-palette desktop-menu details hatching icon-sizes icons launcher launcher-close menu-emphasis folder-window search-place desktop-status arrange-offer rename-ext activation notify pointer restore schemes search session sounds system-menu themes terminal terminal-programs tray work-area osd-command trays menubar columns column-details pictures suspend screens desktop-screens x11-scale screens-object libadwaita
comp-test: session gui compositor
	@fails=; for t in $(COMP_TESTS); do echo "== $$t"; tests/comp/$$t.sh > $(TV)/comp-$$t.log 2>&1 || fails="$$fails $$t"; \
	  grep -E "^(ok|FAIL)" $(TV)/comp-$$t.log; done; \
	 if [ -n "$$fails" ]; then echo "comp-test failed:$$fails (logs: $(TV)/comp-*.log)"; exit 1; fi

# The looks, for the refactor (docs/object-properties-design.md 4): shots
# of one scene in each scheme; visual-baseline keeps them in build/visual/base,
# visual-check takes them again and compares (differences in build/visual/diff).
visual-baseline: session gui compositor
	rm -rf $(B)/visual/base && tests/visual/run.sh $(B)/visual/base
visual-check: session gui compositor
	rm -rf $(B)/visual/new $(B)/visual/diff && tests/visual/run.sh $(B)/visual/new
	python3 -I tests/visual/compare.py $(B)/visual/base $(B)/visual/new $(B)/visual/diff
.PHONY: comp-test visual-baseline visual-check

# make install DESTDIR=... PREFIX=/usr: the programs, the user service,
# the session on the login screen, the lock screen's PAM file and the
# GNOME Shell extension (which then starts osd-shell from the PATH).
# The commands' long names, links to the short ones (the owner's: osd for
# ObjectSpaceDesktop): short=long.
LONGNAMES := osd=objectspace osdctl=objectspace-ctl osd-shell=objectspace-shell osd-terminal=objectspace-terminal osd-session=objectspace-session
LONGS := $(foreach p,$(LONGNAMES),$(lastword $(subst =, ,$(p))))
INSTALLED := $(B)/osdd $(B)/osdctl $(B)/osd $(B)/osd-watchd $(B)/osd-plughost $(B)/osd-shell $(B)/osd-terminal $(B)/osd-compositor
EXT := gnome-extension/objectspace@sungsookhim.github.io
EXTDIR = $(DATADIR)/gnome-shell/extensions/objectspace@sungsookhim.github.io
subst = sed -e 's|@BINDIR@|$(BINDIR)|g' -e 's|@LIBEXECDIR@|$(LIBEXECDIR)|g'
install: $(INSTALLED) icons install-sdk
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(LIBEXECDIR) $(DESTDIR)$(LIBDIR)/objectspace/plugins
	install -m 755 $(B)/osd $(B)/osdctl $(B)/osd-shell $(B)/osd-terminal $(DESTDIR)$(BINDIR)/
	install -m 755 $(B)/osdd $(B)/osd-watchd $(B)/osd-plughost $(B)/osd-compositor $(DESTDIR)$(LIBEXECDIR)/
	$(subst) tools/osd-session > $(DESTDIR)$(BINDIR)/osd-session && chmod 755 $(DESTDIR)$(BINDIR)/osd-session
	install -d $(DESTDIR)$(USERUNITDIR) $(DESTDIR)$(SYSTEMUNITDIR) $(DESTDIR)$(DATADIR)/wayland-sessions $(DESTDIR)$(SYSCONFDIR)/pam.d
	$(subst) data/osdd.service.in > $(DESTDIR)$(USERUNITDIR)/osdd.service
	$(subst) service/osd-watchd.service > $(DESTDIR)$(SYSTEMUNITDIR)/osd-watchd.service
	$(subst) data/objectspace.desktop.in > $(DESTDIR)$(DATADIR)/wayland-sessions/objectspace.desktop
	install -d $(DESTDIR)$(DATADIR)/applications
	$(subst) data/io.github.sungsookhim.Objectspace.Terminal.desktop.in > $(DESTDIR)$(DATADIR)/applications/io.github.sungsookhim.Objectspace.Terminal.desktop
	install -m 644 data/osd-lock.pam $(DESTDIR)$(SYSCONFDIR)/pam.d/osd-lock
	install -d $(DESTDIR)$(DATADIR)/objectspace/icons
	install -m 644 data/icons/*.png $(DESTDIR)$(DATADIR)/objectspace/icons/
	for t in $(ICONTHEMES)/*/; do d=$(DESTDIR)$(DATADIR)/objectspace/icon-themes/$$(basename "$$t"); \
	    install -d "$$d" && install -m 644 "$$t"theme.ini "$$t"*.osdicon "$$d"/ || exit 1; done
	install -d $(DESTDIR)$(EXTDIR)/schemas
	install -m 644 $(EXT)/metadata.json $(EXT)/extension.js $(DESTDIR)$(EXTDIR)/
	install -m 644 $(EXT)/schemas/*.gschema.xml $(DESTDIR)$(EXTDIR)/schemas/
	glib-compile-schemas $(DESTDIR)$(EXTDIR)/schemas
	install -d $(DESTDIR)$(MANDIR)/man1
	for m in data/man/*.1; do sed -e 's|@VERSION@|$(OSD_VERSION)|' \
	    -e "s|@DATE@|$$(date -u -d @$${SOURCE_DATE_EPOCH:-$$(git log -1 --format=%ct 2>/dev/null || date +%s)} +%Y-%m-%d)|" \
	    $$m > $(DESTDIR)$(MANDIR)/man1/$$(basename $$m); done
	for p in $(LONGNAMES); do ln -sf $${p%%=*} $(DESTDIR)$(BINDIR)/$${p#*=}; \
	    ln -sf $${p%%=*}.1 $(DESTDIR)$(MANDIR)/man1/$${p#*=}.1; done

# The plug-in SDK (the objectspace-devel package; sdk/README.md): the headers a
# plug-in includes, the built-in classes' interfaces, osdc and the sample.
install-sdk: $(GEN)/.stamp
	install -d $(DESTDIR)$(INCLUDEDIR)/objectspace $(DESTDIR)$(DATADIR)/objectspace/sdk/classes $(DESTDIR)$(DATADIR)/objectspace/sdk/sample
	install -m 644 runtime/include/osdo.h classes/osd_classes.h classes/osd_state.h classes/osd_props.h \
	    service/attrs.h $(GEN)/*.h $(DESTDIR)$(INCLUDEDIR)/objectspace/
	install -m 644 classes/*.osdi $(DESTDIR)$(DATADIR)/objectspace/sdk/classes/
	install -m 755 tools/osdc.py $(DESTDIR)$(DATADIR)/objectspace/sdk/
	install -m 644 sdk/README.md $(DESTDIR)$(DATADIR)/objectspace/sdk/
	install -m 644 sdk/sample/OsdImageFile.osdi sdk/sample/OsdImageFile.c sdk/sample/Makefile $(DESTDIR)$(DATADIR)/objectspace/sdk/sample/
.PHONY: install-sdk

# make install into a scratch root, then a headless session from it
install-check: $(INSTALLED)
	tests/install/check.sh

uninstall:
	rm -f $(addprefix $(DESTDIR)$(BINDIR)/,osd osdctl osd-shell osd-terminal osd-session $(LONGS))
	rm -f $(addprefix $(DESTDIR)$(LIBEXECDIR)/,osdd osd-watchd osd-plughost osd-compositor)
	rm -f $(DESTDIR)$(USERUNITDIR)/osdd.service $(DESTDIR)$(SYSTEMUNITDIR)/osd-watchd.service
	rm -f $(DESTDIR)$(DATADIR)/wayland-sessions/objectspace.desktop $(DESTDIR)$(SYSCONFDIR)/pam.d/osd-lock $(DESTDIR)$(DATADIR)/applications/io.github.sungsookhim.Objectspace.Terminal.desktop
	rm -rf $(DESTDIR)$(EXTDIR) $(DESTDIR)$(DATADIR)/objectspace/icons $(DESTDIR)$(DATADIR)/objectspace/icon-themes
	rm -rf $(DESTDIR)$(DATADIR)/objectspace/sdk $(DESTDIR)$(INCLUDEDIR)/objectspace
	rm -f $(addprefix $(DESTDIR)$(MANDIR)/man1/,osd.1 osdctl.1 osd-shell.1 osd-session.1 osd-terminal.1 $(addsuffix .1,$(LONGS)))
	-rmdir $(DESTDIR)$(DATADIR)/objectspace 2>/dev/null
	-rmdir $(DESTDIR)$(LIBEXECDIR) $(DESTDIR)$(LIBDIR)/objectspace/plugins $(DESTDIR)$(LIBDIR)/objectspace 2>/dev/null

clean:
	rm -rf $(GEN) $(B)/gen-* $(TV)

# The RPM (v0.2-design 11.3): make dist (the tarball of HEAD and
# build/objectspace.spec), make srpm, make rpm (rpmbuild in build/rpm). The RPM's
# version is VERSION with "-" as "~" (0.2.0-dev sorts before 0.2.0); its
# release 1 for a release, the commit count for a development build.
RPMVER  := $(subst -,~,$(OSD_VERSION))
RPMREL  := $(if $(findstring -,$(OSD_VERSION)),$(shell git rev-list --count HEAD 2>/dev/null || echo 0),1)
DIST    := objectspace-$(RPMVER)
RPMTOP  := $(abspath $(B))/rpm
dist:
	@mkdir -p $(B)
	git archive --format=tar.gz --prefix=$(DIST)/ -o $(B)/$(DIST).tar.gz HEAD
	sed -e 's|@VERSION@|$(RPMVER)|g' -e 's|@RELEASE@|$(RPMREL)|g' \
	    -e "s|@DATE@|$$(LC_ALL=C date -d @$$(git log -1 --format=%ct) '+%a %b %d %Y')|" packaging/objectspace.spec.in > $(B)/objectspace.spec
	@echo "$(B)/$(DIST).tar.gz, $(B)/objectspace.spec (from HEAD: commit first)"
srpm: dist
	rpmbuild -bs --define "_topdir $(RPMTOP)" --define "_sourcedir $(abspath $(B))" $(B)/objectspace.spec
rpm: dist
	rpmbuild -ba --define "_topdir $(RPMTOP)" --define "_sourcedir $(abspath $(B))" $(B)/objectspace.spec
	@ls $(RPMTOP)/RPMS/*/*.rpm $(RPMTOP)/SRPMS/*.rpm

.PHONY: all plugins test check m0 m2 m3 g0 gui gui-test clean install uninstall install-check dist srpm rpm unit-test
