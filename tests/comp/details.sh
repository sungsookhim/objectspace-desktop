#!/bin/sh
# The details view, headless (v0.3-plan step 4): a heading clicked sorts
# the folder (again: reversed; folders first), the order coming from wpsd's
# listing; over a column's edge the pointer is the resize one (0.4 step 5);
# a column's edge dragged keeps its width; a column turned off.
cd "$(dirname "$0")/../.."
REPO=$PWD
S=$(mktemp -d "${TMPDIR:-/tmp}/wps-details-XXXX")
# Projects opens at 320,96 (640x480), Ctrl+3: details. Headings at y 133:
# Title at 400, Size at 760; the Title column's right edge at 506 (window x 186).
cat > "$S/script" <<EOS
2.5 dclick 54 30
3.5 key ctrl+3
4.5 click 760 133
6 click 760 133
7.5 click 400 133
8.2 move 506 133
8.6 move 450 300
9 down 506 133
9.3 move 545 133
9.6 move 566 133
9.9 up 566 133
13 shot $S/end.png
13.5 quit
EOS
W="$REPO/build/wpsctl"
order() { echo "'$W' list \"\$T/desk/Projects\" | python3 -c 'import json,sys; print(\" \".join(i[\"name\"] for i in json.load(sys.stdin)[\"result\"][\"items\"]))' >> \"\$T/order.log\""; }
PRE='cd "$T/desk/Projects" && head -c 300 /dev/zero > b.txt && head -c 100 /dev/zero > a.png && head -c 200 /dev/zero > c.md && mkdir z m' \
DESKENV="OSD_WARPCENTER=1 OSD_NO_GSETTINGS=1 OSD_NO_LAUNCHPAD=1" SECS=16 tests/comp/run.sh "$S/script" \
    "sleep 5.5; $(order); sleep 1.5; $(order); sleep 1.5; $(order); sleep 3;
     '$W' settings_set \"\$T/desk/Projects\" col_type=false > /dev/null; sleep 0.5;
     '$W' settings \"\$T/desk/Projects\" > \"\$T/settings.log\"" > "$S/out.txt" 2>&1
T=$(sed -n 's/^T=//p' "$S/out.txt")
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
line() { sed -n "${1}p" "$T/order.log"; }
check "Size clicked: by size, folders first" "[ \"\$(line 1)\" = 'm z a.png c.md b.txt' ]"
check "Size again: reversed (folders still first)" "[ \"\$(line 2)\" = 'z m b.txt c.md a.png' ]"
check "Title clicked: by name" "[ \"\$(line 3)\" = 'm z a.png b.txt c.md' ]"
check "the Title column's width kept (dragged wider)" "python3 -c \"
import json; f = {x['key']: x.get('value') for p in json.load(open('$T/settings.log'))['result'] for x in p['fields']}
w = [float(v) for v in f['col_widths'].split(',')]; exit(0 if w[0] > 200 else 1)\""
# (the compositor logs the shapes asked for by their hot spots: the
# resize arrow's in its middle, 12,12; the arrow's at its tip)
check "over an edge: the resize pointer; off it: the arrow again" "grep 'client cursor' '$S/out.txt' | grep -A1 -m1 'hotspot 12,12' | tail -1 | grep -qv 'hotspot 12,12'"
check "Type turned off" "grep -q '\"key\":\"col_type\"[^}]*\"value\":false' '$T/settings.log'"
check "the sort kept: name (choice 1), ascending" "grep -q '\"key\":\"sort_key\"[^}]*\"value\":1' '$T/settings.log' && grep -q '\"key\":\"sort_desc\"[^}]*\"value\":false' '$T/settings.log'"
echo "$fails failed (screenshot: $S/end.png, logs: $T)"
[ $fails = 0 ]
