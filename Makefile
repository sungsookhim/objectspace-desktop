# WPS for Linux: object core.
#
#   make          build the runtime, the core classes and the tests
#   make test     run the unit tests (with AddressSanitizer and UBSan if installed)
#   make check    tests + strict C11 core check + ThreadSanitizer loop run
#   make m2       M2 scenarios with real services on loopback filesystems (sudo)
#   make m3       M3 change-tracking scenarios, inotify and wps-watchd modes (sudo)
#   make m0       build the M0 prototypes (see proto/m0)
#   make g0       build the G0 GUI prototypes (see proto/g0; optimised, no sanitizers)
#   make gui      build wps-shell (optimised) and wps-shell-asan (sanitizers)
#   make clean

CC      ?= cc
PYTHON  ?= python3
B       := build
GEN     := $(B)/gen
CFLAGS  ?= -O1 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
# Checked static calls (wpo_slot verifies the class) in these builds.
CFLAGS  += -DWPO_CHECKED
# AddressSanitizer and UBSan when their runtimes are installed (Fedora:
# libasan, libubsan); otherwise a plain build. Override with SAN=.
SAN     ?= $(shell echo 'int main(void){return 0;}' | $(CC) -fsanitize=address,undefined -x c - \
             -o /dev/null 2>/dev/null && echo -fsanitize=address,undefined -fno-omit-frame-pointer)
INC     := -Iruntime/include -Iclasses -Iservice -I$(GEN)

RUNTIME := runtime/src/wpo.c runtime/src/wpo_loop_posix.c runtime/src/wpo_plugin_posix.c
WPI     := $(wildcard classes/*.wpi)
CLASSES := $(wildcard classes/*.c) service/attrs.c $(patsubst classes/%.wpi,$(GEN)/%.reg.c,$(WPI))

# Test class TBase: version 1.1 is "installed"; plug-ins are built against
# 1.0 (older, must load), 1.2 (newer, refused) and 2.0 (other major, refused).
TV      := $(B)/tests

all: $(B)/wpsd $(B)/wpsctl $(B)/wps-watchd $(B)/wps-plughost $(TV)/test_runtime $(TV)/test_loop $(TV)/test_attrs $(TV)/test_db $(TV)/test_cbor $(TV)/test_launch $(TV)/test_ui_damage plugins

$(GEN)/.stamp: $(WPI) tools/wpoc.py
	$(PYTHON) tools/wpoc.py -I classes -o $(GEN) $(WPI)
	@touch $@

$(GEN)/%.reg.c: $(GEN)/.stamp ;

$(B)/gen-%/.stamp: tests/versions/%/TBase.wpi tests/plugins/TChild.wpi tools/wpoc.py
	$(PYTHON) tools/wpoc.py -o $(B)/gen-$* $<
	$(PYTHON) tools/wpoc.py -I tests/versions/$* -o $(B)/gen-$*/plugin tests/plugins/TChild.wpi
	@touch $@

$(TV)/test_runtime: tests/test_runtime.c $(RUNTIME) $(GEN)/.stamp $(B)/gen-v11/.stamp tests/TBase_v11.c $(CLASSES) $(wildcard classes/*.h)
	@mkdir -p $(TV)
	$(CC) $(CFLAGS) $(SAN) $(INC) -I$(B)/gen-v11 -rdynamic -o $@ tests/test_runtime.c $(RUNTIME) \
	    $(CLASSES) tests/TBase_v11.c $(B)/gen-v11/TBase.reg.c -ldl -lpthread

$(TV)/test_loop: tests/test_loop.c runtime/src/wpo_loop_posix.c runtime/include/wpo_loop.h
	@mkdir -p $(TV)
	$(CC) $(CFLAGS) $(SAN) $(INC) -o $@ tests/test_loop.c runtime/src/wpo_loop_posix.c -lpthread

$(TV)/test_attrs: tests/test_attrs.c service/attrs.c service/attrs.h
	@mkdir -p $(TV)
	$(CC) $(CFLAGS) $(SAN) -Iservice -Itests -o $@ tests/test_attrs.c service/attrs.c

$(TV)/test_db: tests/test_db.c service/db.c service/db.h service/attrs.c
	@mkdir -p $(TV)
	$(CC) $(CFLAGS) $(SAN) -Iservice -Itests -o $@ tests/test_db.c service/db.c service/attrs.c -lsqlite3 -lpthread

$(TV)/test_ui_damage: tests/test_ui_damage.c gui/ui_damage.c gui/ui_damage.h
	@mkdir -p $(TV)
	$(CC) $(CFLAGS) $(SAN) -Igui -Itests -o $@ tests/test_ui_damage.c gui/ui_damage.c

$(TV)/test_launch: tests/test_launch.c service/launch.c service/launch.h
	@mkdir -p $(TV)
	$(CC) $(CFLAGS) $(SAN) $(INC) -Itests -o $@ tests/test_launch.c service/launch.c

$(TV)/test_cbor: tests/test_cbor.c service/cbor.c service/cbor.h
	@mkdir -p $(TV)
	$(CC) $(CFLAGS) $(SAN) -Iservice -Itests -o $@ tests/test_cbor.c service/cbor.c

SERVICE := service/wpsd.c service/objects.c service/tracker.c service/shadows.c service/listing.c service/find.c service/launch.c service/trash.c service/jobs.c service/plugins.c service/db.c service/env.c service/volumes.c service/cbor.c

$(B)/wpsd: $(SERVICE) $(RUNTIME) $(GEN)/.stamp $(wildcard service/*.h classes/*.c classes/*.h)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(SAN) $(INC) -rdynamic -o $@ $(SERVICE) $(RUNTIME) $(CLASSES) -lsqlite3 -ldl -lpthread

$(B)/wps-plughost: service/plughost.c service/cbor.c $(RUNTIME) $(GEN)/.stamp $(wildcard classes/*.c classes/*.h)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(SAN) $(INC) -rdynamic -o $@ service/plughost.c service/cbor.c $(RUNTIME) $(CLASSES) -ldl -lpthread

$(B)/wps-watchd: service/watchd.c service/cbor.c service/cbor.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(SAN) -Iservice -o $@ service/watchd.c service/cbor.c

$(B)/wpsctl: service/wpsctl.c service/cbor.c service/cbor.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(SAN) -Iservice -o $@ service/wpsctl.c service/cbor.c

# M5: a plug-in for wps-plughost, and one built against a newer WPFolder
# (1.1) than the one installed, which must be refused.
M5PLUG := $(TV)/m5plugins/testfolder.so $(TV)/m5future/future.so

$(TV)/m5plugins/testfolder.so: tests/plugins/TestFolder.c tests/plugins/TestFolder.wpi $(GEN)/.stamp
	@mkdir -p $(TV)/m5plugins $(B)/gen-testfolder
	$(PYTHON) tools/wpoc.py -I classes -o $(B)/gen-testfolder tests/plugins/TestFolder.wpi
	$(CC) $(CFLAGS) $(SAN) -fPIC -shared $(INC) -I$(B)/gen-testfolder \
	    -o $@ tests/plugins/TestFolder.c $(B)/gen-testfolder/TestFolder.reg.c

$(TV)/m5future/future.so: tests/plugins/TestFolder.c tests/plugins/TestFolder.wpi $(GEN)/.stamp
	@mkdir -p $(TV)/m5future $(B)/gen-future/classes $(B)/gen-future/out
	cp classes/*.wpi $(B)/gen-future/classes/
	sed -i 's/^version 1.0$$/version 1.1/' $(B)/gen-future/classes/WPFolder.wpi
	$(PYTHON) tools/wpoc.py -I $(B)/gen-future/classes -o $(B)/gen-future/out $(B)/gen-future/classes/*.wpi tests/plugins/TestFolder.wpi
	$(CC) $(CFLAGS) $(SAN) -fPIC -shared -Iruntime/include -Iclasses -Iservice -I$(B)/gen-future/out \
	    -o $@ tests/plugins/TestFolder.c $(B)/gen-future/out/TestFolder.reg.c

PLUGINS := $(M5PLUG) $(TV)/plugins/tchild_v10.so $(TV)/plugins/tchild_v12.so $(TV)/plugins/tchild_v20.so \
           $(TV)/plugins/myfolder.so
plugins: $(PLUGINS)

$(TV)/plugins/tchild_%.so: tests/plugins/TChild.c $(B)/gen-%/.stamp
	@mkdir -p $(TV)/plugins
	$(CC) $(CFLAGS) $(SAN) -fPIC -shared -Iruntime/include -Iservice -I$(B)/gen-$*/plugin -I$(B)/gen-$* \
	    -o $@ tests/plugins/TChild.c $(B)/gen-$*/plugin/TChild.reg.c

$(TV)/plugins/myfolder.so: tests/plugins/MyFolder.c tests/plugins/MyFolder.wpi $(GEN)/.stamp
	@mkdir -p $(TV)/plugins $(B)/gen-myfolder
	$(PYTHON) tools/wpoc.py -I classes -o $(B)/gen-myfolder tests/plugins/MyFolder.wpi
	$(CC) $(CFLAGS) $(SAN) -fPIC -shared $(INC) -I$(B)/gen-myfolder \
	    -o $@ tests/plugins/MyFolder.c $(B)/gen-myfolder/MyFolder.reg.c

test: all
	$(TV)/test_runtime $(TV)/plugins
	$(TV)/test_loop
	$(TV)/test_attrs $(TV)/attrs-tmp
	$(TV)/test_db $(TV)/db-tmp
	$(TV)/test_cbor
	$(TV)/test_launch $(TV)/launch-tmp
	$(TV)/test_ui_damage

# Everything that must stay true: the unit tests (with ASan/UBSan when
# installed), the core building as strict C11 with only the C library, and
# the loop under ThreadSanitizer when libtsan is installed.
CORE_LIBC := calloc free malloc memcpy realloc strchr strcmp strlen
TSAN_OK   := $(shell echo 'int main(void){return 0;}' | $(CC) -fsanitize=thread -x c - -o /dev/null 2>/dev/null && echo yes)

check: test
	@mkdir -p $(TV)
	$(CC) -std=c11 -Wall -Wextra -pedantic -Werror -Iruntime/include -c runtime/src/wpo.c -o $(TV)/wpo_c11.o
	@extra=$$(nm -u $(TV)/wpo_c11.o | awk '{print $$2}' | grep -vxE '$(subst $(eval) ,|,$(CORE_LIBC))'); \
	 if [ -n "$$extra" ]; then echo "core uses more than the C library subset: $$extra"; exit 1; fi; \
	 echo "core: strict C11, C library subset only"
ifeq ($(TSAN_OK),yes)
	$(CC) -O1 -g -std=gnu11 -fsanitize=thread $(INC) -o $(TV)/test_loop_tsan tests/test_loop.c runtime/src/wpo_loop_posix.c -lpthread
	$(TV)/test_loop_tsan
else
	@echo "ThreadSanitizer not installed (Fedora: libtsan); loop race check skipped"
endif

# M2 scenarios against the loopback test bed (needs sudo; see proto/m0/testbed.sh).
m2: all
	proto/m0/testbed.sh down
	proto/m0/testbed.sh up
	tests/m2/m2.py; rc=$$?; proto/m0/testbed.sh down; exit $$rc

# M3 scenarios in both tracking modes (inotify, and wps-watchd as root).
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
G0FLAGS := -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
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

# The GUI: libwpsui (gui/ui_*.c) and wps-shell. Optimised for timing; the
# -asan build is for finding memory errors.
UISRC   := gui/ui_app.c gui/ui_draw.c gui/ui_damage.c gui/ui_frame.c gui/ui_framepaint.c gui/ui_menu.c gui/ui_container.c gui/ui_layer.c gui/ui_entry.c gui/ui_mouse.c gui/ui_notebook.c gui/ui_dnd.c gui/ui_dialog.c gui/ui_textinput.c gui/ui_a11y.c
SHELLSRC := gui/shell/wps-shell.c gui/shell/svc.c gui/shell/layout.c gui/shell/wallpaper.c gui/shell/events.c gui/shell/sound.c gui/shell/gnome.c gui/shell/winlist.c gui/shell/launchpad.c gui/shell/findwin.c gui/shell/icons.c gui/shell/iconpick.c service/cbor.c $(G0)/xdg-shell-protocol.c $(G0)/viewporter-protocol.c $(G0)/xdg-activation-v1-protocol.c $(G0)/text-input-unstable-v3-protocol.c
UIPKG   := cairo harfbuzz freetype2 fontconfig wayland-client wayland-cursor xkbcommon libsystemd
UIDEPS  := $(UISRC) $(SHELLSRC) $(wildcard gui/*.h gui/shell/*.h) classes/wps_menu_ids.h $(G0)/xdg-shell-client-protocol.h $(G0)/viewporter-client-protocol.h $(G0)/xdg-activation-v1-client-protocol.h $(G0)/text-input-unstable-v3-client-protocol.h
UIFLAGS = -Igui -Igui/shell -Iservice -Iclasses -I$(G0) $(shell pkg-config --cflags $(UIPKG))
UILIBS  = $(shell pkg-config --libs $(UIPKG)) -lpthread -lm
# Sounds: PipeWire, and libsndfile when installed (Ogg, FLAC; else WAV only).
SNDFILE := $(shell pkg-config --exists sndfile 2>/dev/null && echo sndfile)
SHFLAGS = $(shell pkg-config --cflags libpipewire-0.3 $(SNDFILE)) $(if $(SNDFILE),-DHAVE_SNDFILE)
SHLIBS  = $(shell pkg-config --libs libpipewire-0.3 $(SNDFILE))

$(B)/wps-shell: $(UIDEPS)
	$(CC) $(G0FLAGS) $(UIFLAGS) $(SHFLAGS) -o $@ $(UISRC) $(SHELLSRC) $(UILIBS) $(SHLIBS)

$(B)/wps-shell-asan: $(UIDEPS)
	$(CC) $(G0FLAGS) $(SAN) $(UIFLAGS) $(SHFLAGS) -o $@ $(UISRC) $(SHELLSRC) $(UILIBS) $(SHLIBS)

# G5: the compositor (wlroots 0.20). It draws frames with the toolkit's
# frame code (gui/ui_framepaint.c), so it links the drawing part of it.
COMPPKG := wlroots-0.20 wayland-server xkbcommon pixman-1 libdrm cairo harfbuzz freetype2 fontconfig xcb
# The lock screen checks passwords with PAM (pam-devel); without it, no locking.
COMPPAM := $(shell test -f /usr/include/security/pam_appl.h && echo -DHAVE_PAM)
COMPSRC := compositor/wps-compositor.c compositor/desktop.c compositor/lock.c gui/ui_draw.c gui/ui_framepaint.c
DECOXML := $(shell pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
$(G0)/xdg-decoration-unstable-v1-client-protocol.h: $(DECOXML)
	@mkdir -p $(G0)
	wayland-scanner client-header $< $@
$(G0)/xdg-decoration-unstable-v1-protocol.c: $(DECOXML)
	@mkdir -p $(G0)
	wayland-scanner private-code $< $@

compositor: $(B)/wps-compositor $(TV)/ssd-client $(TV)/x11-client
# An X11 test client (XWayland).
$(TV)/x11-client: tests/comp/x11-client.c
	@mkdir -p $(TV)
	$(CC) $(G0FLAGS) -o $@ $< $$(pkg-config --cflags --libs xcb)
# A test client that asks for a server-side frame (tests/comp).
$(TV)/ssd-client: tests/comp/ssd-client.c $(G0)/xdg-shell-client-protocol.h $(G0)/xdg-shell-protocol.c $(G0)/xdg-decoration-unstable-v1-client-protocol.h $(G0)/xdg-decoration-unstable-v1-protocol.c
	@mkdir -p $(TV)
	$(CC) $(G0FLAGS) -I$(G0) -o $@ tests/comp/ssd-client.c $(G0)/xdg-shell-protocol.c $(G0)/xdg-decoration-unstable-v1-protocol.c $$(pkg-config --cflags --libs wayland-client)
$(B)/wps-compositor: $(COMPSRC) gui/ui.h compositor/comp.h
	@mkdir -p $(B)
	$(CC) $(G0FLAGS) -DWLR_USE_UNSTABLE $(COMPPAM) -Igui $$(pkg-config --cflags $(COMPPKG)) -o $@ $(COMPSRC) $$(pkg-config --libs $(COMPPKG)) $(if $(COMPPAM),-lpam) -lpthread -lm

$(TV)/test_ui_entry: tests/test_ui_entry.c gui/ui_entry.c gui/ui_draw.c gui/ui.h
	@mkdir -p $(TV)
	$(CC) $(G0FLAGS) $(SAN) $(UIFLAGS) -Itests -o $@ tests/test_ui_entry.c gui/ui_entry.c gui/ui_draw.c $(UILIBS)

$(TV)/test_ui_mouse: tests/test_ui_mouse.c gui/ui_mouse.c gui/ui.h
	@mkdir -p $(TV)
	$(CC) $(G0FLAGS) $(SAN) $(UIFLAGS) -Itests -o $@ tests/test_ui_mouse.c gui/ui_mouse.c $(UILIBS)

$(TV)/test_layout: tests/test_layout.c gui/shell/layout.c gui/shell/layout.h
	@mkdir -p $(TV)
	$(CC) $(G0FLAGS) $(SAN) -Igui/shell -Itests -o $@ tests/test_layout.c gui/shell/layout.c -lm

gui: $(B)/wps-shell $(B)/wps-shell-asan $(TV)/test_ui_entry $(TV)/test_ui_mouse $(TV)/test_layout

# GUI unit tests (need the GUI's -devel packages, unlike make test)
gui-test: gui
	$(TV)/test_ui_damage
	$(TV)/test_ui_entry
	$(TV)/test_ui_mouse
	$(TV)/test_layout

clean:
	rm -rf $(GEN) $(B)/gen-* $(TV)

.PHONY: all plugins test check m0 m2 m3 g0 gui gui-test clean
