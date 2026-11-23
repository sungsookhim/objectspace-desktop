#!/bin/sh
# The plug-in SDK as its users get it (sdk/README.md): installed into a
# scratch root (make install-sdk), a skeleton from wpsctl plugin new built
# against it (and the sample), loaded by a private wpsd.
cd "$(dirname "$0")/../.."
REPO=$PWD
S=$(mktemp -d "${TMPDIR:-/tmp}/wps-sdk-XXXX")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
make -s install-sdk DESTDIR="$S/root" PREFIX=/usr > "$S/install.log" 2>&1
SDK="$S/root/usr/share/wps/sdk" INC="$S/root/usr/include/wps"
check "the SDK installed (headers, interfaces, wpoc, sample)" "[ -f '$INC/wpo.h' ] && [ -f '$INC/WPDataFile.h' ] && [ -f '$SDK/classes/WPObject.wpi' ] && [ -x '$SDK/wpoc.py' ] && [ -f '$SDK/sample/Makefile' ]"
(cd "$S" && "$REPO/build/wpsctl" plugin new MyNotes WPDataFile > "$S/new.log" 2>&1)
check "wpsctl plugin new: a skeleton" "[ -f '$S/MyNotes/MyNotes.wpi' ] && [ -f '$S/MyNotes/MyNotes.c' ] && grep -q 'NAME    ?= MyNotes' '$S/MyNotes/Makefile'"
make -s -C "$S/MyNotes" WPS_SDK="$SDK" WPS_INC="$INC" > "$S/build.log" 2>&1
check "... built with the SDK alone" "[ -f '$S/MyNotes/mynotes.so' ]"
cp -r "$SDK/sample" "$S/sample" && make -s -C "$S/sample" WPS_SDK="$SDK" WPS_INC="$INC" >> "$S/build.log" 2>&1
check "the sample built with the SDK alone" "[ -f '$S/sample/wpimagefile.so' ]"
# loaded by a private wpsd
T="$S/t" RT=$(mktemp -d "$XDG_RUNTIME_DIR/wps-sdk-XXXX")
mkdir -p "$T/desk" "$T/xdg/config" "$T/xdg/data"
export WPS_CONFIG_DIR="$T/config" WPS_STATE_DIR="$T/state" WPS_RUNTIME_DIR="$RT" XDG_CONFIG_HOME="$T/xdg/config" XDG_DATA_HOME="$T/xdg/data"
export WPS_TEMPLATES_DIR="$T/templates" WPS_UDISKS_BUS="unix:path=$RT/none" WPS_PLUGIN_DIRS="$S/MyNotes"
"$REPO/build/wpsd" 2> "$S/wpsd.log" &
WP=$!
for i in $(seq 50); do "$REPO/build/wpsctl" ping >/dev/null 2>&1 && break; sleep 0.1; done
for i in $(seq 30); do grep -q "mynotes ready" "$S/wpsd.log" && break; sleep 0.1; done
"$REPO/build/wpsctl" settings class:MyNotes > "$S/class.json" 2>&1
check "loaded: its class's notebook has its Note" "grep -q '\"key\":\"note\"' '$S/class.json' && grep -q 'All of class MyNotes' '$S/class.json'"
"$REPO/build/wpsctl" shutdown >/dev/null 2>&1; wait $WP
rm -rf "$RT"
echo "$fails failed ($S)"
[ $fails = 0 ] && rm -rf "$S"
[ $fails = 0 ]
