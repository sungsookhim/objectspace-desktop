#!/bin/sh
# The osd command (0.4.1 step 14), headless, against a running desktop:
# get, set, create, shadow, say, ask (answered with Enter), selection (an
# icon clicked on the desktop), open (a folder: its window), folder.
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-cmd-XXXX")
Objectspace="$PWD/build/osd"
# notes.txt on the desktop at 124,40 (clicked: selected); the ask dialog
# answered with Enter (its first button) at 6 s.
cat > "$S/script" <<EOS
3 click 124 40
6 key Return
9 windows
9.5 quit
EOS
cat > "$S/cmd.sh" <<EOS
W='$Objectspace'; S='$S'
sleep 2.5
\$W get "\$T/desk/Projects" sort_key > "\$S/get.txt" 2>&1
\$W set "\$T/desk/notes.txt" 'title=My Notes' > "\$S/set.txt" 2>&1; echo \$? >> "\$S/set.txt"
\$W get "\$T/desk/notes.txt" title > "\$S/title.txt" 2>&1
\$W create "\$T/desk/Projects" OsdFolder Reports > "\$S/create.txt" 2>&1
\$W shadow "\$T/desk/notes.txt" "\$T/desk/Projects" > "\$S/shadow.txt" 2>&1
\$W say "from a script" > "\$S/say.txt" 2>&1; echo \$? >> "\$S/say.txt"
sleep 1
\$W selection > "\$S/selection.txt" 2>&1
\$W ask "Go on?" Yes No > "\$S/ask.txt" 2>&1; echo \$? >> "\$S/ask.txt"
\$W open "\$T/desk/Projects" > "\$S/open.txt" 2>&1
OSD_FOLDER="\$T/desk" \$W folder > "\$S/folder.txt" 2>&1
\$W get "\$T/desk/nothing-here" title > "\$S/bad.txt" 2>&1; echo \$? >> "\$S/bad.txt"
sleep 60
EOS
env -u OSD_SESSION DESKENV="OSD_NO_GSETTINGS=1 OSD_NO_LAUNCHER=1" SECS=12 tests/comp/run.sh "$S/script" "sh '$S/cmd.sh'" > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
check "get: a property's value, plain" "[ \"\$(cat '$S/get.txt')\" = name ]"
check "set: a setup string (exit 0)" "[ \"\$(cat '$S/set.txt')\" = 0 ]"
check "... read back" "[ \"\$(cat '$S/title.txt')\" = 'My Notes' ]"
check "create: the new object's path" "[ \"\$(cat '$S/create.txt')\" = \"$T/desk/Projects/Reports\" ] && [ -d '$T/desk/Projects/Reports' ]"
check "shadow: its address (a central object: vobj:ID)" "grep -Eq '^vobj:[0-9a-f-]{36}$' '$S/shadow.txt'"
check "say (exit 0)" "[ \"\$(cat '$S/say.txt')\" = 0 ]"
check "selection: the desktop's selected icon" "[ \"\$(cat '$S/selection.txt')\" = \"$T/desk/notes.txt\" ]"
check "ask: the button chosen" "[ \"\$(head -1 '$S/ask.txt')\" = Yes ] && [ \"\$(tail -1 '$S/ask.txt')\" = 0 ]"
check "open: a folder's window" "grep -q \"window 'Projects - Icon View'\" '$S/out.txt'"
check "folder: OSD_FOLDER" "[ \"\$(cat '$S/folder.txt')\" = \"$T/desk\" ]"
check "an error: a message, exit 1" "[ \"\$(tail -1 '$S/bad.txt')\" = 1 ] && grep -q '^osd: ' '$S/bad.txt'"
[ $fails = 0 ] && rm -rf "$S" "$T" || echo "logs: $S $T"
exit $fails
