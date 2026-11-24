#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Sungsoo Khim
"""The 0.5 rename's rules (tools/rename-0.5.py) on samples of each kind of name."""
import importlib.util
import re
import sys
from pathlib import Path

spec = importlib.util.spec_from_file_location("rename", Path(__file__).resolve().parent.parent / "tools" / "rename-0.5.py")
rn = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rn)

def run(stage, text):
    return rn.apply(text, [(re.compile(p), r) for p, r in rn.STAGES[stage]])[0]

cases = [
    ("code", "WPFolder *f = WPFolder_data(o); M_WPFolderClass; WPFolderClass;", "OsdFolder *f = OsdFolder_data(o); M_OsdFolderClass; OsdFolderClass;"),
    ("code", "WPDrives WPDrive WPProgramsFolder WPProgram", "OsdDrives OsdDrive OsdProgramsFolder OsdProgram"),
    ("code", "WPObject_wpQueryTitle(o); wpclsNew; \"wpcls\"", "OsdObject_osdQueryTitle(o); osdclsNew; \"osdcls\""),
    ("code", "wpo_object *x; WPO_P_TEXT; WPORoot; WPOClassClass; #include \"wpo.h\"", "osdo_object *x; OSDO_P_TEXT; OsdoRoot; OsdoClassClass; #include \"osdo.h\""),
    ("code", "tools/wpoc.py classes/WPFolder.wpi WpiError _WPI_H", "tools/osdc.py classes/OsdFolder.osdi OsdiError _OSDI_H"),
    ("code", "wps_load(); WPS_ID_LEN; WPS_MI_OPEN; MIME_WPS_X; read_wps_once; connect_wpsd", "osd_load(); OSD_ID_LEN; OSD_MI_OPEN; MIME_OSD_X; read_osd_once; connect_osdd"),
    ("code", "WPS is the name; wpsd; WPSetup; WPS_FOLDER", "WPS is the name; wpsd; OsdSetup; OSD_FOLDER"),
    ("code", "swpo_x; awps_load; newpX", "swpo_x; awps_load; newpX"),   # inside other words: untouched
    ("external", "build/wpsd build/wpsctl wps-shell wps-terminal wps-session.target wps-lock", "build/osdd build/osdctl osd-shell osd-terminal osd-session.target osd-lock"),
    ("external", "org.wps.Desktop user.wps.id application/x-wps-color wps@wps-linux", "io.github.sungsookhim.Objectspace.Desktop user.osd.id application/x-objectspace-color objectspace@sungsookhim.github.io"),
    ("external", "~/.config/wps $(DATADIR)/wps/icons libexec/wps/ wps.sock x.wpsicon a.wpobj", "~/.config/objectspace $(DATADIR)/objectspace/icons libexec/objectspace/ objectspace.sock x.osdicon a.osdobj"),
    ("external", "github.com/sungsookhim/wps-linux wps-devel 80-wps.preset", "github.com/sungsookhim/objectspace-desktop objectspace-devel 80-objectspace.preset"),
    ("ui", "on the WarpCenter; warpcenter_open(); OSD_WARPCENTER; OsdWarpCenter; LaunchPad; lp_launchpad_x", "on TheBar; thebar_open(); OSD_THEBAR; OsdTheBar; Launcher; lp_launcher_x"),
    ("ui", "WPS Terminal; \"WPS (flat)\"", "Objectspace Terminal; \"Objectspace (flat)\""),
]
fails = 0
for stage, src, want in cases:
    got = run(stage, src)
    ok = got == want
    fails += not ok
    print(("ok  " if ok else "FAIL"), stage, src[:50])
    if not ok:
        print("   got: ", got)
        print("   want:", want)
print(f"\n{len(cases)} checks, {fails} failed")
sys.exit(1 if fails else 0)
