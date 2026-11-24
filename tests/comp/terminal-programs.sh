#!/bin/sh
# Program objects in Objectspace Terminal (0.4.1 step 13), headless: a script in a
# work area runs in Objectspace Terminal (the System's default), in use while it
# runs; Command prompt here opens the user's shell there ($OSD_FOLDER set),
# and System's Command Prompt (0.5) in the home folder;
# both go with the work area when it is minimized; Close window on exit
# on (the window goes with the program) and off (it stays, ended).
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-termprog-XXXX")
W="$PWD/build/osdctl"
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
# the user's shell, for Command prompt here: says where it is, then waits
printf '#!/bin/sh\necho "$PWD|$OSD_FOLDER" >> "%s/prompt.txt"\ncommand -v osd > "%s/osd.txt"\nsleep 30\n' "$S" "$S" > "$S/shell.sh"
chmod +x "$S/shell.sh"
# System's Command Prompt, opened as its icon would be (osdctl open)
cat > "$S/sysprompt.sh" <<EOS
#!/bin/sh
id=\$('$W' list "\$T/xdg/data/objectspace/System" 0 50 details | python3 -c "import json,sys; print([x['name'][1:] for x in json.load(sys.stdin)['result']['items'] if x.get('title') == 'Command Prompt'][0])")
'$W' open "\$id" > "$S/sysprompt.txt" 2>&1
EOS
chmod +x "$S/sysprompt.sh"
# Projects (its icon at 54,30) opens at 320,106: hello.sh at 372,176. The
# Projects icon's menu: Command prompt here at 148,156. The terminals open
# over Projects: opening it again brings it forward (its minimize button
# at 903,118).
cat > "$S/one" <<EOS
3 dclick 54 30
4.5 dclick 372 176
5.8 click 54 30 right
6.5 click 148 156
8 windows
8.5 dclick 54 30
9.3 click 903 118
10.3 windows
10.8 quit
EOS
PRE="printf '#!/bin/sh\necho hello from a script\nsleep 30\n' > \"\$T/desk/Projects/hello.sh\"; chmod +x \"\$T/desk/Projects/hello.sh\"
     '$W' settings_set \"\$T/desk/Projects\" 'work_area=1' > /dev/null" \
    SHELL="$S/shell.sh" env -u OSD_SESSION DESKENV="OSD_NO_GSETTINGS=1 OSD_NO_LAUNCHER=1" SECS=14 tests/comp/run.sh "$S/one" \
    "sleep 7; T=\$T '$S/sysprompt.sh'; sleep 0.5; '$W' list \"\$T/desk/Projects\" 0 50 details > \"\$T/list.txt\" 2>&1; sleep 60" > "$S/one.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/one.txt")
cat > "$S/blocks.py" <<'EOP'
import sys
blocks = []
for l in open(sys.argv[1]):
    if l.startswith("osd-compositor: windows now:"): blocks.append([])
    elif l.startswith("osd-compositor: window '") and blocks: blocks[-1].append(l.strip())
for i, b in enumerate(blocks):
    for l in b:
        if "app io.github.sungsookhim.Objectspace.Terminal" in l: print(i, l.split("'")[1], "minimized" if l.endswith("minimized") else "shown")
EOP
python3 "$S/blocks.py" "$S/one.txt" > "$S/blocks.txt"
check "a script object runs in Objectspace Terminal, titled by it" "grep -q \"^0 hello.sh shown\" '$S/blocks.txt'"
check "its object is in use while it runs" "python3 -c \"import json,sys; d = json.load(open('$T/list.txt')); e = [x for x in d['result']['items'] if x['name'] == 'hello.sh']; sys.exit(0 if e and e[0].get('in_use') else 1)\""
check "Command prompt here: the user's shell in Objectspace Terminal" "grep -q \"^0 Command Prompt - Projects shown\" '$S/blocks.txt'"
check "... in the folder, OSD_FOLDER set" "grep -qxF \"\$T/desk/Projects|\$T/desk/Projects\" '$S/prompt.txt'"
check "System's Command Prompt: the user's shell in Objectspace Terminal" "grep -q \"^0 Command Prompt - $(basename "$HOME") shown\" '$S/blocks.txt'"
check "... in the home folder" "grep -qxF \"$HOME|$HOME\" '$S/prompt.txt'"
check "... and the commands on its PATH (osd)" "[ \"\$(cat '$S/osd.txt' 2>/dev/null)\" = '$PWD/build/osd' ]"
check "both minimized with the work area" "[ \$(grep -c '^1 .* minimized' '$S/blocks.txt') = 2 ]"
cp "$T/shell.log" "$S/shell1.log" 2>/dev/null
rm -rf "$T"
# Close window on exit: on (quick.sh: the window goes with it), off (held.sh's Program page)
cat > "$S/two" <<EOS
3 dclick 54 30
4.5 dclick 460 176
6.5 dclick 372 176
8.5 windows
9 quit
EOS
PRE="printf '#!/bin/sh\nexit 0\n' > \"\$T/desk/Projects/quick.sh\"; printf '#!/bin/sh\necho done\nexit 4\n' > \"\$T/desk/Projects/held.sh\"
     chmod +x \"\$T/desk/Projects/quick.sh\" \"\$T/desk/Projects/held.sh\"
     '$W' settings_set \"\$T/desk/Projects/held.sh\" 'prog_close=0' > /dev/null" \
    env -u OSD_SESSION DESKENV="OSD_NO_GSETTINGS=1 OSD_NO_LAUNCHER=1" SECS=12 tests/comp/run.sh "$S/two" "sleep 60" > "$S/two.txt" 2>&1
T2=$(sed -n 's/^T=//p' "$S/two.txt")
check "Close window on exit on: the window goes with the program" "! grep -q \"window 'quick.sh\" '$S/two.txt'"
check "Close window on exit off: the window stays, ended" "grep -q \"window 'held.sh (ended)' app io.github.sungsookhim.Objectspace.Terminal\" '$S/two.txt'"
check "no sanitizer reports" "! grep -q Sanitizer '$S/shell1.log' '$T2/shell.log'"
[ $fails = 0 ] && rm -rf "$S" "$T2" || echo "logs: $S $T2"
exit $fails
