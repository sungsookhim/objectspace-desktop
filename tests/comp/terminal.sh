#!/bin/sh
# WPS Terminal (0.4.1 step 12), headless: true colours and Korean drawn,
# vim edits and saves a file, a resize reaches the program, htop runs and
# quits (the window closing with it), --hold keeps the window with the
# program's status until Enter.
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/wps-term-XXXX")
TERM_BIN="$PWD/build/wps-terminal-asan"
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
run() {   # NAME SCRIPT COMMAND...: one terminal in the headless compositor
    n=$1; sc=$2; shift 2
    NODESK=1 SECS=12 tests/comp/run.sh "$sc" "$* 2>> '$S/term.log'" > "$S/$n.txt" 2>&1
    t=$(sed -n 's/^T=//p' "$S/$n.txt"); [ -n "$t" ] && rm -rf "$t"
}
# The window opens at 266,78 (80x24); its text starts at 273,107 (17 pixels wide cells: 9 x 22).
cat > "$S/colours.sh" <<'EOS'
printf '\033[48;2;255;0;0m      \033[0m \033[38;2;0;0;255m\342\226\210\342\226\210\033[0m \355\225\234\352\265\255\n'
sleep 30
EOS
printf '3 shot %s/colours.png\n3.5 quit\n' "$S" > "$S/s1"
run colours "$S/s1" "'$TERM_BIN' -- sh '$S/colours.sh'"
px() { python3 -c "from PIL import Image; print(*Image.open('$1').convert('RGB').getpixel(($2, $3)))"; }
check "a true-colour background (red)" "[ \"\$(px '$S/colours.png' 290 117)\" = '255 0 0' ]"
check "a true-colour foreground (blue blocks)" "[ \"\$(px '$S/colours.png' 345 117)\" = '0 0 255' ]"
check "Korean drawn (dark pixels in its cells)" "python3 -c \"from PIL import Image; im = Image.open('$S/colours.png').convert('RGB'); exit(0 if sum(1 for x in range(372, 410) for y in range(108, 126) if sum(im.getpixel((x, y))) < 300) > 20 else 1)\""
# vim: insert a line, save, quit; the window closes with it
cat > "$S/s2" <<EOS
3 key i
3.2 key h
3.3 key i
3.5 key Escape
3.7 key shift+semicolon
3.8 key w
3.9 key q
4 key Return
5.5 windows
6 quit
EOS
run vim "$S/s2" "'$TERM_BIN' -- vim -u NONE -N '$S/edited.txt'"
check "vim edits and saves a file" "[ \"\$(cat '$S/edited.txt' 2>/dev/null)\" = hi ]"
check "... and the window closes with it" "! grep -q \"window 'vim\" '$S/vim.txt'"
# a resize: maximized (its button at 976,93), the program told (SIGWINCH)
cat > "$S/winch.sh" <<EOS
trap 'stty size > "$S/size.txt"' WINCH
stty size > "$S/size0.txt"
while :; do sleep 0.2; done
EOS
printf '3 click 976 93\n5 quit\n' > "$S/s3"
run winch "$S/s3" "'$TERM_BIN' -- sh '$S/winch.sh'"
check "its size at the start (24 80)" "[ \"\$(cat '$S/size0.txt' 2>/dev/null)\" = '24 80' ]"
check "maximized: the program has the new size" "[ -s '$S/size.txt' ] && [ \$(cut -d' ' -f1 '$S/size.txt') -gt 24 ] && [ \$(cut -d' ' -f2 '$S/size.txt') -gt 80 ]"
# htop: drawn (its header row's colours), q quits it
printf '3.5 shot %s/htop.png\n3.8 key q\n5 windows\n5.5 quit\n' "$S" > "$S/s4"
run htop "$S/s4" "'$TERM_BIN' -- htop"
check "htop drawn (coloured cells)" "python3 -c \"from PIL import Image; im = Image.open('$S/htop.png').convert('RGB'); exit(0 if len({im.getpixel((x, y)) for x in range(273, 990, 3) for y in range(107, 630, 3)}) > 6 else 1)\""
check "htop quits, its window with it" "! grep -q \"window 'htop\" '$S/htop.txt'"
# --hold: the program's status shown, Enter closes
printf '3 windows\n3.2 shot %s/hold.png\n3.5 key Return\n4.5 windows\n5 quit\n' "$S" > "$S/s5"
run hold "$S/s5" "'$TERM_BIN' --hold --title Held -- sh -c 'echo done; exit 3'"
check "--hold: the window stays, its title says it ended" "grep -q \"window 'Held (ended)'\" '$S/hold.txt'"
check "... and Enter closes it" "[ \$(grep -c \"window 'Held\" '$S/hold.txt') = 1 ]"
check "no sanitizer reports" "! grep -q 'Sanitizer' '$S/term.log'"
[ $fails = 0 ] && rm -rf "$S" || echo "logs: $S"
exit $fails
