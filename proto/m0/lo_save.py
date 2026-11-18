#!/usr/bin/env python3
"""Open, edit and save a document with LibreOffice, through its UNO bridge.

Runs a separate headless LibreOffice with its own profile, so it does not
touch a LibreOffice the user has open. The save is LibreOffice's normal
document save (XStorable.store), the same code path as File > Save.

usage: lo_save.py DOCUMENT PROFILE_DIR
Prints "loaded", waits for a line on stdin, then saves and prints "saved".
"""
import os
import subprocess
import sys
import time

import uno
from com.sun.star.beans import PropertyValue

path, profile = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
port = 20000 + os.getpid() % 10000
office = subprocess.Popen(
    ["soffice", "--headless", "--invisible", "--norestore", "--nologo", "--nodefault",
     f"-env:UserInstallation=file://{profile}",
     f"--accept=socket,host=127.0.0.1,port={port};urp;"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

local = uno.getComponentContext()
resolver = local.ServiceManager.createInstanceWithContext("com.sun.star.bridge.UnoUrlResolver", local)
ctx = None
for _ in range(240):
    try:
        ctx = resolver.resolve(f"uno:socket,host=127.0.0.1,port={port};urp;StarOffice.ComponentContext")
        break
    except Exception:
        time.sleep(0.25)
if ctx is None:
    print("error could not connect to LibreOffice", flush=True)
    office.kill()
    sys.exit(1)

desktop = ctx.ServiceManager.createInstanceWithContext("com.sun.star.frame.Desktop", ctx)
hidden = PropertyValue()
hidden.Name, hidden.Value = "Hidden", True
doc = desktop.loadComponentFromURL(uno.systemPathToFileUrl(path), "_blank", 0, (hidden,))
print("loaded", flush=True)
sys.stdin.readline()
text = doc.getText()
text.insertString(text.getEnd(), " edited", False)
doc.store()
print("saved", flush=True)
doc.close(True)
try:
    desktop.terminate()
except Exception:
    pass
try:
    office.wait(timeout=30)
except subprocess.TimeoutExpired:
    office.kill()
