#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
# The Screens object (0.7 step 5), headless, two screens (1280x800 and
# 1024x768 to its right), GNOME's monitors.xml followed: its notebook
# (from a shadow on the desktop); the second dragged in the picture below
# the first: monitors.xml has it there and the compositor puts it there;
# a resolution changed and not kept: back after the time; a scale changed
# and kept: kept; the second turned off, then on again. monitors.xml keeps
# GNOME's names for a screen (vendor, product, serial) and its
# configurations for other screens.
cd "$(dirname "$0")/../.."
S=$(mktemp -d "${TMPDIR:-/tmp}/osd-screensobj-XXXX")
W="$PWD/build/osdctl"
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
cat > "$S/monitors.xml" <<EOX
<monitors version="2">
  <configuration>
    <logicalmonitor><x>0</x><y>0</y><scale>1</scale><primary>yes</primary>
      <monitor><monitorspec><connector>eDP-1</connector><vendor>BOE</vendor><product>0x0abc</product><serial>0x0</serial></monitorspec><mode><width>1920</width><height>1080</height><rate>60.000</rate></mode></monitor>
    </logicalmonitor>
  </configuration>
  <configuration>
    <logicalmonitor><x>0</x><y>0</y><scale>1</scale><primary>yes</primary>
      <monitor><monitorspec><connector>HEADLESS-1</connector><vendor>GSM</vendor><product>LG 4K</product><serial>0x1234</serial></monitorspec><mode><width>1280</width><height>800</height><rate>60.000</rate></mode></monitor>
    </logicalmonitor>
    <logicalmonitor><x>1280</x><y>0</y><scale>1</scale>
      <monitor><monitorspec><connector>HEADLESS-2</connector><vendor>x</vendor><product>x</product><serial>2</serial></monitorspec><mode><width>1024</width><height>768</height><rate>60.000</rate></mode></monitor>
    </logicalmonitor>
  </configuration>
</monitors>
EOX
# The Screens shadow lands at 560,45; its notebook at 380,98: the picture's
# second screen at 715,300; the tab of the second's page at 680,146, its
# On at 590,212, Resolution at 687,242, Scale at 687,302.
cat > "$S/script" <<EOS
5 dclick 560 45
7.5 down 715 300
7.7 move 650 350
7.9 move 600 420
8.1 up 600 420
9 screens
9.2 shot $S/arranged.png
9.5 click 680 146
10 click 687 242
10.3 key Escape
10.6 key Up
11.5 screens
16 screens
16.5 click 687 302
16.8 key Escape
17.1 key Down
17.8 key Return
19 screens
19.5 click 590 212
20.2 key Return
21.5 screens
22 click 590 212
22.7 key Return
24 screens
24.5 quit
EOS
PRE="mkdir -p \"\$XDG_CONFIG_HOME\"; cp '$S/monitors.xml' \"\$XDG_CONFIG_HOME/\"" \
OSD_COMP_WATCH_MONITORS=1 SIZE=1280x800,1024x768 SECS=27 \
DESKENV="OSD_THEBAR=1 OSD_NO_LAUNCHER=1 OSD_NO_GSETTINGS=1 OSD_WALLPAPER= OSD_SCREENS_KEEP_SECS=4" tests/comp/run.sh "$S/script" \
    "sleep 2; a=\$('$W' setup | python3 -c \"import json,sys; print(json.load(sys.stdin)['result']['objects']['OsdScreens']['address'])\"); '$W' shadow \"\$a\" \"\$T/desk\" > \"\$T/shadow.log\" 2>&1
     n=0; last=; while sleep 0.2; do m=\$(stat -c %Y.%s \"\$XDG_CONFIG_HOME/monitors.xml\" 2>/dev/null; md5sum < \"\$XDG_CONFIG_HOME/monitors.xml\" 2>/dev/null); [ \"\$m\" = \"\$last\" ] && continue; last=\$m; n=\$((n + 1)); cp \"\$XDG_CONFIG_HOME/monitors.xml\" \"\$T/xml.\$n\" 2>/dev/null; done" \
    > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
# the Nth "screens now" answer's line for a screen
scr() { awk -v n="$1" -v s="$2" '/screens now:/ { k++ } k == n && $2 == s { print; exit }' "$S/out.txt"; }
echo "  second: $(scr 1 HEADLESS-2) | $(scr 2 HEADLESS-2) | $(scr 3 HEADLESS-2) | $(scr 4 HEADLESS-2) | $(scr 5 HEADLESS-2) | $(scr 6 HEADLESS-2)"
# HEADLESS-2's place in a monitors.xml's first configuration
place() { python3 -c "
import re, sys
t = open('$1').read()
c = t[t.index('<configuration>'):t.index('</configuration>')]
for lm in re.findall(r'<logicalmonitor>(.*?)</logicalmonitor>', c, re.S):
    if '<connector>$2</connector>' in lm:
        print(re.search(r'<x>(-?\d+)</x>', lm).group(1), re.search(r'<y>(-?\d+)</y>', lm).group(1), re.search(r'<scale>([\d.]+)</scale>', lm).group(1), re.search(r'<width>(\d+)</width>', lm).group(1))
" 2>/dev/null; }
# monitors.xml as it was written each time (xml.1 GNOME's, then the Screens object's)
snap() { for f in $(ls "$T"/xml.* 2>/dev/null | sort -t. -k2 -n); do python3 -c "import sys; exec(sys.argv[1])" "$1" "$f" && { echo "$f"; return; }; done; }
d=$(snap "import re,sys; t=open(sys.argv[2]).read(); c=t[t.index('<configuration>'):t.index('</configuration>')]; sys.exit(0 if re.search(r'<y>800</y>', c) and '<scale>1</scale>' in c and '1024</width>' in c and '<disabled>' not in c else 1)")
r=$(snap "import sys; t=open(sys.argv[2]).read(); c=t[t.index('<configuration>'):t.index('</configuration>')]; sys.exit(0 if 'HEADLESS-2' in c and '<width>1280</width>' in c.split('HEADLESS-2')[1] else 1)")
k=$(snap "import sys; t=open(sys.argv[2]).read(); c=t[t.index('<configuration>'):t.index('</configuration>')]; sys.exit(0 if '<scale>1.25</scale>' in c else 1)")
o=$(snap "import sys; t=open(sys.argv[2]).read(); c=t[t.index('<configuration>'):t.index('</configuration>')]; sys.exit(0 if '<disabled>' in c else 1)")
last=$(ls "$T"/xml.* 2>/dev/null | sort -t. -k2 -n | tail -1)
echo "  monitors.xml written $(ls "$T"/xml.* 2>/dev/null | wc -l) times: below $d, 1280 $r, 1.25 $k, off $o"
check "the Screens object: a shadow of it on the desktop" "grep -q 'vobj:' '$T/shadow.log'"
check "dragged below the first: monitors.xml has it there" "[ -n '$d' ]"
check "... and the compositor put it there" "scr 1 HEADLESS-2 | grep -q 'on 1024x768@60000 scale 1 at [0-9]*,800'"
check "a resolution changed: at once" "scr 2 HEADLESS-2 | grep -q 'on 1280x800'"
check "... in the file" "[ -n '$r' ]"
check "... not kept: back after the time (the compositor, the file)" "scr 3 HEADLESS-2 | grep -q 'on 1024x768' && [ -n '$k' ] && (set -- \$(place \"\$(ls '$T'/xml.* | sort -t. -k2 -n | grep -B1 -x '$k' | head -1)\" HEADLESS-2); [ \"\$4\" = 1024 ] && [ \"\$3\" = 1 ])"
check "a scale changed and kept: kept" "scr 4 HEADLESS-2 | grep -q 'scale 1.25' && grep -q '<scale>1.25</scale>' '$last'"
check "turned off and kept: off (in <disabled>)" "scr 5 HEADLESS-2 | grep -q ' off ' && [ -n '$o' ]"
check "... the first still on and the primary" "scr 5 HEADLESS-1 | grep -q 'on 1280x800.* primary'"
check "on again: on, beside the first" "scr 6 HEADLESS-2 | grep -q ' on '"
check "GNOME's names for a screen kept (vendor, product, serial)" "grep -q '<vendor>GSM</vendor>' '$last' && grep -q '<product>LG 4K</product>' '$last'"
check "GNOME's configuration for other screens kept" "grep -q '<connector>eDP-1</connector>' '$last' && [ \$(grep -c '<configuration>' '$last') = 2 ]"
check "no sanitizer reports" "! grep -q Sanitizer '$T/shell.log' && ! grep -q Sanitizer '$T/comp.log'"
echo "$fails failed ($S, logs: $T)"
[ $fails = 0 ] && rm -rf "$S" "$T"
exit $fails
