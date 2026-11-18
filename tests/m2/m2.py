#!/usr/bin/env python3
"""M2 scenarios: persistence, identity, concurrency, users, travelling volumes.

Runs real wpsd services against the loopback test bed (proto/m0/testbed.sh
up): ext4, btrfs and FAT. Services run as the current user, and as test
users 60001 (A) and 60002 (B, both in group 60000) through setpriv. A second
"computer" is simulated by a second wpsd with its own database and host ID
but the same WPS identity file.

usage: tests/m2/m2.py [scenario ...]     e.g. m2.py 1 25a
Writes build/m2/report.json.
"""
import json
import os
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = Path(os.environ.get("WORK", f"/var/tmp/wps-m0-{os.environ.get('USER', os.getlogin())}"))
MNT = {fs: WORK / "mnt" / fs for fs in ("ext4", "btrfs", "fat")}
BIN = WORK / "bin"
OUT = ROOT / "build" / "m2"
results = []
scenarios = []


def scenario(num, title):
    def reg(fn):
        scenarios.append((str(num), title, fn))
        return fn
    return reg


def run(cmd, user=None, check=False, env=None):
    if user:
        cmd = ["sudo", "setpriv", f"--reuid={user}", "--regid=60000", "--groups=60000", "--", "env"] + \
              [f"{k}={v}" for k, v in (env or {}).items()] + cmd
        env = None
    p = subprocess.run(cmd, capture_output=True, text=True, env={**os.environ, **(env or {})})
    if check and p.returncode != 0:
        raise RuntimeError(f"{' '.join(map(str, cmd))}: {p.stderr.strip() or p.stdout.strip()}")
    return p


def mount_info(path):
    for line in open("/proc/self/mountinfo"):
        f = line.split()
        if f[4] == str(path):
            return f
    return None


def fs_uuid(fs):
    src = run(["findmnt", "-n", "-o", "SOURCE", str(MNT[fs])]).stdout.strip()
    return run(["sudo", "blkid", "-s", "UUID", "-o", "value", src]).stdout.strip()


class Service:
    """One wpsd: a user (None = current user), a computer (host), dirs."""

    def __init__(self, name, user=None, host=None, identity_from=None):
        self.name, self.user = name, user
        self.base = WORK / "svc" / name
        self.env = {
            "WPS_CONFIG_DIR": str(self.base / "config"),
            "WPS_STATE_DIR": str(self.base / "state"),
            "WPS_RUNTIME_DIR": str(self.base / "run"),
            "WPS_SAVE_DELAY_MS": "100",
            "WPS_HOST_ID": host or f"host-{name}",
            "ASAN_OPTIONS": "detect_leaks=1",
        }
        self.proc = None
        self.log = OUT / f"wpsd-{name}.log"
        if user:
            run(["sudo", "mkdir", "-p", str(self.base)], check=True)
            run(["sudo", "chown", f"{user}:60000", str(self.base)], check=True)
        else:
            self.base.mkdir(parents=True, exist_ok=True)
        if identity_from:   # same WPS user on another computer: copy the identity file
            (self.base / "config").mkdir(parents=True, exist_ok=True)
            shutil.copy(identity_from.base / "config" / "identity", self.base / "config" / "identity")

    def config_file(self, name, text):
        d = self.base / "config"
        if self.user:
            run(["sh", "-c", f"mkdir -p {d} && printf '%s' '{text}' > {d}/{name}"], user=self.user, check=True)
        else:
            d.mkdir(parents=True, exist_ok=True)
            (d / name).write_text(text)

    def start(self):
        cmd = [str(BIN / "wpsd")]
        logf = open(self.log, "a")
        if self.user:
            full = ["sudo", "setpriv", f"--reuid={self.user}", "--regid=60000", "--groups=60000", "--", "env"] + \
                   [f"{k}={v}" for k, v in self.env.items()] + cmd
            self.proc = subprocess.Popen(full, stderr=logf, stdout=logf)
        else:
            self.proc = subprocess.Popen(cmd, stderr=logf, stdout=logf, env={**os.environ, **self.env})
        for _ in range(100):
            if self.ctl("ping").get("ok"):
                return self
            time.sleep(0.05)
        raise RuntimeError(f"wpsd {self.name} did not start; see {self.log}")

    def ctl(self, *args):
        p = run([str(BIN / "wpsctl")] + [str(a) for a in args], user=self.user, env=self.env)
        try:
            return json.loads(p.stdout.strip().splitlines()[-1])
        except (ValueError, IndexError):
            return {"ok": False, "error": "no reply", "message": p.stderr.strip()}

    def call(self, target, method, *args, expect=None):
        extra = ["--expect", str(expect)] if expect is not None else []
        return self.ctl("call", *extra, target, method, *args)

    def title(self, target):
        r = self.ctl("resolve", target)
        return r.get("result", {}).get("title") if r.get("ok") else f"<{r.get('message')}>"

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.ctl("shutdown")
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None

    def wipe_db(self):
        for f in (self.base / "state").glob("objects.db*"):
            run(["sudo", "rm", "-f", str(f)])


def attr(path, name="user.wps.WPObject"):
    p = run(["getfattr", "--only-values", "-n", name, str(path)])
    return p.stdout if p.returncode == 0 else None


def fresh(fs, name, user=None, mode="2775"):
    d = MNT[fs] / name
    run(["sudo", "rm", "-rf", str(d)])
    if user:
        run(["sudo", "mkdir", "-p", str(d)], check=True)
        run(["sudo", "chown", f"{user}:60000", str(d)], check=True)
        run(["sudo", "chmod", mode, str(d)], check=True)
    else:
        d.mkdir(parents=True)
    return d


def write_as(user, path, text="x", mode="0644"):
    if user:
        run(["sh", "-c", f"printf '%s' '{text}' > '{path}' && chmod {mode} '{path}'"], user=user, check=True)
    else:
        Path(path).write_text(text)
        os.chmod(path, int(mode, 8))


def remount(fs, opts):
    """Unmount and mount again (the disk leaving and coming back)."""
    img = WORK / "img" / f"{fs}.img"
    run(["sudo", "umount", str(MNT[fs])], check=True)
    extra = f",uid={os.getuid()},gid={os.getgid()}" if fs == "fat" else ""
    run(["sudo", "mount", "-o", f"loop,{opts}{extra}", str(img), str(MNT[fs])], check=True)


# ------------------------------------------------------------------ scenarios

@scenario(1, "Settings survive a restart (ext4 attribute, and FAT through the database)")
def s1():
    me = Service("me").start()
    d, f = fresh("ext4", "s1"), fresh("fat", "s1")
    (d / "notes.txt").write_text("x")
    (f / "notes.txt").write_text("x")
    me.call(d / "notes.txt", "wpSetTitle", "Notes on ext4")
    me.call(f / "notes.txt", "wpSetTitle", "Notes on FAT")
    me.ctl("flush")
    stored = attr(d / "notes.txt") is not None
    me.stop()
    me.start()
    t1, t2 = me.title(d / "notes.txt"), me.title(f / "notes.txt")
    me.stop()
    ok = t1 == "Notes on ext4" and t2 == "Notes on FAT" and stored
    return ok, [f"ext4 title after restart: {t1!r} (stored in the file's attribute: {stored})",
                f"FAT title after restart: {t2!r} (FAT has no attributes: kept in the database)"]


@scenario(2, "Database deleted: file-stored state survives; database-only state is gone")
def s2():
    me = Service("me")
    d, f = MNT["ext4"] / "s1", MNT["fat"] / "s1"   # left by scenario 1
    me.wipe_db()
    me.start()
    t1, t2 = me.title(d / "notes.txt"), me.title(f / "notes.txt")
    me.stop()
    ok = t1 == "Notes on ext4" and t2 == "notes.txt"
    return ok, [f"ext4 title with a new database: {t1!r} (rebuilt from the file)",
                f"FAT title with a new database: {t2!r} (expected: lost, as the design says)"]


@scenario(10, "Hard links: one object ID, a separate position in each folder")
def s10():
    me = Service("me").start()
    d = fresh("ext4", "s10")
    (d / "a").mkdir()
    (d / "b").mkdir()
    (d / "a" / "doc.txt").write_text("x")
    os.link(d / "a" / "doc.txt", d / "b" / "doc.txt")
    me.call(d / "a", "wpSetIconPos", f"@{d}/a/doc.txt", 10, 10)
    me.call(d / "b", "wpSetIconPos", f"@{d}/b/doc.txt", 90, 90)
    me.ctl("flush")
    ida = me.ctl("resolve", d / "a" / "doc.txt")["result"]["id"]
    idb = me.ctl("resolve", d / "b" / "doc.txt")["result"]["id"]
    pa = me.call(d / "a", "wpQueryIconPos", f"@{d}/a/doc.txt").get("result")
    pb = me.call(d / "b", "wpQueryIconPos", f"@{d}/b/doc.txt").get("result")
    me.stop()
    ok = ida and ida == idb and pa == "10,10" and pb == "90,90"
    return ok, [f"IDs through the two names: {ida} / {idb}", f"positions: folder a {pa}, folder b {pb}"]


@scenario(21, "Two clients change different settings of one object, 200 times each")
def s21():
    me = Service("me").start()
    d = fresh("ext4", "s21")
    (d / "folder").mkdir()
    target = d / "folder"
    def client(method, values):
        for v in values:
            me.call(target, method, v)
    t1 = threading.Thread(target=client, args=("wpSetTitle", [f"title {i}" for i in range(200)]))
    t2 = threading.Thread(target=client, args=("wpSetSortOrder", [i % 16 for i in range(200)]))
    t1.start(); t2.start(); t1.join(); t2.join()
    me.ctl("flush")
    me.stop()
    me.start()
    title = me.title(target)
    sort = me.call(target, "wpQuerySortOrder").get("result")
    me.stop()
    ok = title == "title 199" and sort == 199 % 16
    return ok, [f"after both clients and a restart: title {title!r}, sort order {sort}"]


@scenario(22, "Two clients change the same setting at once: one wins, one gets a conflict")
def s22():
    me = Service("me").start()
    d = fresh("ext4", "s22")
    (d / "f.txt").write_text("x")
    seen = me.ctl("resolve", d / "f.txt")["result"]["changes"]
    r1 = me.call(d / "f.txt", "wpSetTitle", "from client 1", expect=seen)
    r2 = me.call(d / "f.txt", "wpSetTitle", "from client 2", expect=seen)
    me.stop()
    ok = r1.get("ok") and not r2.get("ok") and r2.get("error") == "conflict" and \
        r2.get("object", {}).get("title") == "from client 1"
    return ok, [f"first: ok={r1.get('ok')}; second: {r2.get('error')}, current title {r2.get('object', {}).get('title')!r}"]


@scenario(23, "Users A and B write settings on a shared, writable folder")
def s23():
    a = Service("A", user=60001).start()
    b = Service("B", user=60002).start()
    d = fresh("ext4", "s23", user=60001)
    run(["mkdir", f"{d}/project"], user=60001, check=True)
    run(["chmod", "2775", f"{d}/project"], user=60001, check=True)
    a.call(d / "project", "wpSetTitle", "Project X")
    b.call(d / "project", "wpSetSortOrder", 5)
    a.ctl("flush"); b.ctl("flush")
    ta, sa = a.title(d / "project"), a.call(d / "project", "wpQuerySortOrder").get("result")
    tb, sb = b.title(d / "project"), b.call(d / "project", "wpQuerySortOrder").get("result")
    a.stop(); b.stop()
    ok = ta == tb == "Project X" and sa == sb == 5
    return ok, [f"A sees title {ta!r}, sort {sa}", f"B sees title {tb!r}, sort {sb}",
                "both changes are in the folder's shared attributes"]


@scenario(24, "User B, without write permission, retitles A's file")
def s24():
    a = Service("A", user=60001).start()
    b = Service("B", user=60002).start()
    d = fresh("ext4", "s24", user=60001)
    write_as(60001, d / "report.txt", mode="0644")
    a.call(d / "report.txt", "wpSetTitle", "Report")
    a.ctl("flush")
    rb = b.call(d / "report.txt", "wpSetTitle", "My copy of the report")
    b.ctl("flush")
    tb, ta = b.title(d / "report.txt"), a.title(d / "report.txt")
    b.stop(); b.start()
    tb2 = b.title(d / "report.txt")
    a.stop(); b.stop()
    raw = attr(d / "report.txt") or ""
    ok = rb.get("ok") and tb == tb2 == "My copy of the report" and ta == "Report" and "My copy" not in raw
    return ok, [f"B sees {tb!r} (and {tb2!r} after restarting B)", f"A sees {ta!r}",
                f"file attribute unchanged by B: {'My copy' not in raw}"]


@scenario(25, "Users A and B arrange a shared folder differently")
def s25():
    a = Service("A", user=60001).start()
    b = Service("B", user=60002).start()
    d = fresh("ext4", "s25", user=60001)
    write_as(60001, d / "plan.txt", mode="0664")
    a.call(d, "wpSetIconPos", f"@{d}/plan.txt", 40, 40)
    b.call(d, "wpSetIconPos", f"@{d}/plan.txt", 300, 200)
    a.ctl("flush"); b.ctl("flush")
    a.stop(); b.stop(); a.start(); b.start()
    pa = a.call(d, "wpQueryIconPos", f"@{d}/plan.txt").get("result")
    pb = b.call(d, "wpQueryIconPos", f"@{d}/plan.txt").get("result")
    a.stop(); b.stop()
    ok = pa == "40,40" and pb == "300,200"
    return ok, [f"A (owner, stored on the folder): {pa}", f"B (stored in B's own database): {pb}"]


def portable(svc, fs):
    svc.config_file("portable", fs_uuid(fs) + "\n")


@scenario("25a", "A FAT disk carries the state to another computer (same WPS identity)")
def s25a():
    home = Service("home")
    home.start(); home.stop()                     # creates the identity
    work = Service("work", identity_from=home)
    portable(home, "fat"); portable(work, "fat")
    d = fresh("fat", "s25a")
    (d / "slides.odp").write_text("x")
    home.start()
    home.call(d / "slides.odp", "wpSetTitle", "Conference slides")
    home.ctl("flush")
    home.stop()
    remount("fat", "rw")                          # the disk is unplugged and taken to work
    work.start()
    t = work.title(d / "slides.odp")
    work.stop()
    voldb = list((MNT["fat"] / ".wps").glob("*.db"))
    ok = t == "Conference slides" and len(voldb) == 1
    return ok, [f"title on the other computer: {t!r}", f"volume databases on the disk: {[p.name for p in voldb]}"]


@scenario("25b", "Both computers change the same title while apart; read-only changes wait")
def s25b():
    home, work = Service("home"), Service("work")
    d = MNT["fat"] / "s25a"
    remount("fat", "ro")                          # at home, mounted read-only
    home.start()
    r = home.call(d / "slides.odp", "wpSetTitle", "Home version")
    home.ctl("flush")
    home.stop()
    remount("fat", "rw")                          # taken to work, writable there
    work.start()
    work.call(d / "slides.odp", "wpSetTitle", "Work version")
    work.ctl("flush")
    work.stop()
    remount("fat", "rw")                          # back home, writable now: sync
    home.start()
    sync = home.ctl("sync").get("result", "")
    th = home.title(d / "slides.odp")
    stats = home.ctl("stats").get("result", {})
    home.stop()
    work.start()
    work.ctl("sync")
    tw = work.title(d / "slides.odp")
    work.stop()
    ok = r.get("ok") and th == tw and th in ("Home version", "Work version") and stats.get("conflicts", 0) >= 1
    return ok, [f"home changed it while read-only: ok={r.get('ok')}", f"sync at home: {sync}",
                f"home now shows {th!r}, work shows {tw!r} (they agree)",
                f"conflicts recorded at home: {stats.get('conflicts')}"]


@scenario("25d", "Two users share a portable disk: separate volume databases")
def s25d():
    a = Service("A2", user=60001)
    b = Service("B2", user=60002)
    for s in (a, b):
        portable(s, "ext4")
    # The disk's .wps folder (mode 1777, like /tmp) is made by the first user
    # who can, or by an administrator; here the disk root belongs to neither.
    run(["sudo", "mkdir", "-p", str(MNT["ext4"] / ".wps")], check=True)
    run(["sudo", "chmod", "1777", str(MNT["ext4"] / ".wps")], check=True)
    d = fresh("ext4", "s25d", user=60001)
    write_as(60001, d / "shared.txt", mode="0644")
    a.start(); b.start()
    a.call(d, "wpSetIconPos", f"@{d}/shared.txt", 1, 1)
    b.call(d / "shared.txt", "wpSetTitle", "B's private name")   # no permission: B's overlay
    a.ctl("flush"); b.ctl("flush")
    a.ctl("sync"); b.ctl("sync")
    ta = a.title(d / "shared.txt")
    a.stop(); b.stop()
    dbs = sorted((MNT["ext4"] / ".wps").glob("*.db"))
    modes = {p.name: oct(p.stat().st_mode & 0o777) for p in dbs}
    readable_by_a = run(["cat", str(dbs[-1])], user=60001).returncode == 0 if dbs else None
    ok = len(dbs) == 2 and all(m == "0o600" for m in modes.values()) and ta == "shared.txt"
    return ok, [f"volume databases: {modes}", f"A sees the file as {ta!r} (B's name stays B's)",
                f"A can read one of them: {readable_by_a} (one is A's own)"]


# ------------------------------------------------------------------- driver

def main():
    want = set(sys.argv[1:])
    for fs, m in MNT.items():
        if not os.path.ismount(m):
            sys.exit(f"{m} is not mounted; run proto/m0/testbed.sh up first")
    OUT.mkdir(parents=True, exist_ok=True)
    for f in OUT.glob("wpsd-*.log"):
        f.unlink()
    run(["sudo", "rm", "-rf", str(WORK / "svc"), str(MNT["ext4"] / ".wps"), str(MNT["fat"] / ".wps")])
    BIN.mkdir(parents=True, exist_ok=True)
    for b in ("wpsd", "wpsctl"):
        shutil.copy(ROOT / "build" / b, BIN / b)
    os.chmod(WORK, 0o755)
    for num, title, fn in scenarios:
        if want and num not in want:
            continue
        try:
            ok, detail = fn()
            verdict = "PASS" if ok else "FAIL"
        except Exception as ex:
            verdict, detail = "ERROR", [repr(ex)]
        results.append({"num": num, "title": title, "verdict": verdict, "detail": detail})
        print(f"[{verdict:5}] {num:>3}. {title}")
        for line in detail:
            print("              " + line)
    leaks = [f.name for f in OUT.glob("wpsd-*.log") if "Sanitizer" in f.read_text() or "runtime error" in f.read_text()]
    print(f"\nsanitizer reports in service logs: {leaks or 'none'}")
    (OUT / "report.json").write_text(json.dumps(results, indent=1))
    counts = {}
    for r in results:
        counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
    print("summary:", ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
    sys.exit(0 if not leaks and counts.get("PASS", 0) == len(results) else 1)


if __name__ == "__main__":
    main()
