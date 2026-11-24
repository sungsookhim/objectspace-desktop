#!/usr/bin/env python3
"""M0b: per-user event filtering in the helper.

Starts watchd (as root) and two clients standing in for two users' osdd:
A is UID 60001, B is UID 60002; both are in group 60000. Each client
registers folders by passing open descriptors. The scenarios check that
each user receives events only for what they can see, that revoking access
stops events, and that hidden locations are never revealed.

usage: filter.py         (needs testbed.sh up; uses sudo)
Writes build/m0/filter-report.json.
"""
import json
import os
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
WORK = Path(os.environ.get("WORK", f"/var/tmp/wps-m0-{os.environ.get('USER', os.getlogin())}"))
EXT4 = WORK / "mnt" / "ext4"
BIN = ROOT / "build" / "m0"
SOCK = WORK / "watchd.sock"
results = []


def run(cmd, user=None, check=True):
    if user:
        cmd = ["sudo", "setpriv", f"--reuid={user}", "--regid=60000", "--groups=60000", "--"] + cmd
    p = subprocess.run(cmd, capture_output=True, text=True)
    if check and p.returncode != 0:
        raise RuntimeError(f"{cmd}: {p.stderr.strip()}")
    return p


def as_a(script):
    """Run a shell script as user A."""
    return run(["sh", "-c", script], user=60001)


class Client:
    def __init__(self, uid):
        self.uid = uid
        self.lines = []
        self.p = subprocess.Popen(
            ["sudo", "setpriv", f"--reuid={uid}", "--regid=60000", "--groups=60000", "--",
             "python3", str(WORK / "wpsclient.py"), str(SOCK)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        threading.Thread(target=self._read, daemon=True).start()
        self.wait(lambda l: l.get("ready"))

    def _read(self):
        for line in self.p.stdout:
            self.lines.append(json.loads(line))

    def cmd(self, text):
        self.p.stdin.write(text + "\n")
        self.p.stdin.flush()

    def wait(self, pred, timeout=5.0, start=0):
        end = time.time() + timeout
        while time.time() < end:
            for l in self.lines[start:]:
                if pred(l):
                    return l
            time.sleep(0.02)
        return None

    def reg(self, path):
        n = len(self.lines)
        self.cmd(f"reg {path}")
        return self.wait(lambda l: l.get("from") == "watchd" and "reg" in l, start=n)

    def mark(self):
        return len(self.lines)

    def events(self, since):
        return [l for l in self.lines[since:] if l.get("from") == "watchd" and "mask" in l]

    def close(self):
        try:
            self.cmd("quit")
            self.p.wait(timeout=5)
        except Exception:
            self.p.kill()


def settle(t=0.6):
    time.sleep(t)


def names(evs):
    out = set()
    for e in evs:
        for k in ("dir", "old", "new"):
            v = e.get(k)
            if isinstance(v, dict):
                out.add(v["name"])
    return out


def result(num, title, ok, detail, verdict=None):
    v = verdict or ("PASS" if ok else "FINDING")
    results.append({"num": num, "title": title, "verdict": v, "detail": detail})
    print(f"[{v:7}] F{num}. {title}")
    for d in detail:
        print("            " + d)


def main():
    if not os.path.ismount(EXT4):
        sys.exit("run proto/m0/testbed.sh up first")
    shutil.copy(HERE / "wpsclient.py", WORK / "wpsclient.py")
    os.chmod(WORK / "wpsclient.py", 0o755)

    # Layout, created by user A (60001).
    sh = EXT4 / "shared"
    priv = EXT4 / "private60001"
    run(["sudo", "rm", "-rf", f"{sh}/f", f"{priv}/f", str(EXT4 / "unreg")], check=False)
    as_a(f"mkdir -p {sh}/f/team {sh}/f/team2 {sh}/f/sub {priv}/f && chmod 2775 {sh}/f {sh}/f/team {sh}/f/team2 {sh}/f/sub"
         f" && echo x > {sh}/f/sub/doc.txt && chmod 664 {sh}/f/sub/doc.txt")
    (EXT4 / "unreg").mkdir()

    watchd = subprocess.Popen(["sudo", str(BIN / "watchd"), "-s", str(SOCK), str(EXT4)],
                              stdout=subprocess.PIPE, text=True)
    assert watchd.stdout.readline().startswith("ready")
    a, b = Client(60001), Client(60002)
    try:
        ra = [a.reg(f"{priv}/f"), a.reg(f"{sh}/f")]
        rb = [b.reg(f"{sh}/f")]
        regs_ok = all(r and r.get("ok") for r in ra + rb)

        # F1: activity in A's private folder reaches A only.
        ma, mb = a.mark(), b.mark()
        as_a(f"echo secret > {priv}/f/secret-salary.txt")
        settle()
        ea, eb = a.events(ma), b.events(mb)
        result(1, "A's private activity reaches A only",
               regs_ok and "secret-salary.txt" in names(ea) and not eb,
               [f"registrations ok: {regs_ok}", f"A received {len(ea)} events {sorted(names(ea))}",
                f"B received {len(eb)} events"])

        # F2: B cannot register A's private folder.
        n = b.mark()
        b.cmd(f"reg {priv}/f")
        r = b.wait(lambda l: "cmd" in l and l["cmd"].startswith("reg"), start=n)
        result(2, "B cannot register A's private folder",
               r is not None and r.get("ok") is False,
               [f"B's attempt: {r.get('error') if r else 'no answer'} (the kernel refuses the open)"])

        # F3: moving a file from the shared folder into A's private folder.
        as_a(f"echo report > {sh}/f/moving-secret-plan.txt")
        settle(0.3)
        ma, mb = a.mark(), b.mark()
        as_a(f"mv {sh}/f/moving-secret-plan.txt {priv}/f/renamed-secret-plan.txt")
        settle()
        ea, eb = a.events(ma), b.events(mb)
        b_ren = [e for e in eb if "RENAME" in e["mask"]]
        a_ren = [e for e in ea if "RENAME" in e["mask"]]
        hidden_ok = bool(b_ren) and b_ren[0].get("new") == "<hidden>" and "renamed-secret-plan.txt" not in json.dumps(eb)
        result(3, "A moves a file from the shared folder into a private one",
               hidden_ok and bool(a_ren) and isinstance(a_ren[0].get("new"), dict),
               [f"B sees: {b_ren[0] if b_ren else 'no rename'}",
                f"A sees: {a_ren[0] if a_ren else 'no rename'}",
                f"new name leaked to B: {'renamed-secret-plan.txt' in json.dumps(eb)}"])

        # F4: revoking access stops events.
        rt = b.reg(f"{sh}/f/team")
        mb = b.mark()
        as_a(f"echo 1 > {sh}/f/team/before.txt")
        settle()
        before = names(b.events(mb))
        mb = b.mark()
        as_a(f"chmod 0700 {sh}/f/team && echo 2 > {sh}/f/team/after-revoke.txt")
        settle()
        after = b.events(mb)
        revoked = b.wait(lambda l: "revoked" in l, timeout=0.5, start=mb)
        result(4, "Revoking access (chmod 0700) stops B's events",
               bool(rt and rt.get("ok")) and "before.txt" in before and "after-revoke.txt" not in names(after),
               [f"B registered team: {bool(rt and rt.get('ok'))}; saw before.txt: {'before.txt' in before}",
                f"after chmod 0700, B received: {sorted(names(after)) or 'nothing'}; revoke notice: {revoked is not None}"])

        # F5: a descriptor opened before revocation cannot be registered after it.
        n = b.mark()
        b.cmd(f"open held {sh}/f/team2")
        b.wait(lambda l: l.get("cmd", "").startswith("open"), start=n)
        as_a(f"chmod 0700 {sh}/f/team2")
        n = b.mark()
        b.cmd("regfd held")
        r = b.wait(lambda l: l.get("from") == "watchd" and "reg" in l, start=n)
        result(5, "A descriptor opened before revocation is refused after it",
               r is not None and r.get("ok") is False,
               [f"B held an open descriptor from before chmod 0700; registration: {r}"])

        # F6: a registration with no descriptor is refused.
        n = b.mark()
        b.cmd("regnofd")
        r = b.wait(lambda l: l.get("from") == "watchd" and "reg" in l, start=n)
        result(6, "A registration without a descriptor is refused",
               r is not None and r.get("ok") is False, [f"answer: {r}"])

        # F7: B registers a single file in a folder it did not register.
        rf = b.reg(f"{sh}/f/sub/doc.txt")
        mb = b.mark()
        as_a(f"rm {sh}/f/sub/doc.txt")
        settle()
        eb = b.events(mb)
        result(7, "B registered one file; its deletion reaches B",
               bool(rf and rf.get("ok")) and any("DELETE_SELF" in e["mask"] for e in eb),
               [f"file registration: {bool(rf and rf.get('ok'))}", f"B received: {[e['mask'] for e in eb]}"])

        # F8: activity in a folder nobody registered reaches nobody.
        ma, mb = a.mark(), b.mark()
        (EXT4 / "unreg" / "x.txt").write_text("x")
        settle()
        result(8, "Activity in an unregistered folder reaches nobody",
               not a.events(ma) and not b.events(mb),
               [f"A received {len(a.events(ma))}, B received {len(b.events(mb))}"])

        # F9: flood in A's private folder; B unaffected.
        ma, mb = a.mark(), b.mark()
        t0 = time.time()
        run(["python3", "-c",
             f"import os\nfor i in range(20000): open('{priv}/f/flood%05d' % i,'w').close()"], user=60001)
        t1 = time.time()
        deadline = time.time() + 60
        while time.time() < deadline:
            n_a = sum(1 for e in a.events(ma) if "CREATE" in e["mask"])
            if n_a >= 20000 or (time.time() - t0 > 5 and any("overflow" in l for l in a.lines[ma:])
                                and len(a.lines) == getattr(a, "_last", -1)):
                break
            a._last = len(a.lines)
            time.sleep(0.2)
        t2 = time.time()
        n_a = sum(1 for e in a.events(ma) if "CREATE" in e["mask"])
        n_b = len(b.events(mb))
        over = [l["overflow"] for l in a.lines[ma:] if "overflow" in l]
        result(9, "A floods 20,000 creates in a private folder",
               n_b == 0 and (n_a == 20000 or bool(over)),
               [f"created in {t1 - t0:.1f} s; A received {n_a} CREATE events within {t2 - t0:.1f} s",
                f"overflow notices to A: {len(over)} (lines dropped: {sum(over)}); a notice means A rescans",
                f"B received {n_b} events"])

        # F11: a stalled service gets an overflow notice and stalls nobody else.
        slow = Client(60001)
        slow.reg(f"{priv}/f")
        n = slow.mark()
        slow.cmd("stall 8")
        slow.wait(lambda l: l.get("cmd", "").startswith("stall"), start=n)
        ma, mb, ms = a.mark(), b.mark(), slow.mark()
        as_a(f"mkdir {priv}/f/flood2")
        a.reg(f"{priv}/f/flood2")
        slow.cmd(f"reg {priv}/f/flood2")
        t0 = time.time()
        run(["python3", "-c",
             f"import os\nfor i in range(60000): open('{priv}/f/flood2/g%05d' % i,'w').close()"], user=60001)
        deadline = time.time() + 60
        while time.time() < deadline:
            if sum(1 for e in a.events(ma) if "CREATE" in e["mask"]) >= 60000:
                break
            time.sleep(0.2)
        t_a = time.time() - t0
        n_a = sum(1 for e in a.events(ma) if "CREATE" in e["mask"])
        got = slow.wait(lambda l: "overflow" in l, timeout=20, start=ms)
        slow.close()
        result(11, "A stalled service gets an overflow notice; others are not held up",
               n_a >= 60000 and got is not None and t_a < 15,
               [f"the healthy client received {n_a} CREATE events (60,000 files + the folder) in {t_a:.1f} s",
                f"the stalled client, after resuming: {got}"])

        # Overall: nothing from A's private folder ever reached B.
        private_names = ("secret-salary", "renamed-secret-plan", "flood")
        leak = [l for l in b.lines if any(n in json.dumps(l) for n in private_names)]
        result(10, "Nothing from A's private folder ever reached B", not leak,
               [f"lines received by B: {len(b.lines)}; containing private names: {len(leak)}"])
    finally:
        a.close()
        b.close()
        run(["sudo", "pkill", "-TERM", "-x", "watchd"], check=False)
        try:
            stats = watchd.stdout.read()
            print("watchd:", stats.strip())
        except Exception:
            pass
        watchd.wait(timeout=10)
    (BIN / "filter-report.json").write_text(json.dumps(results, indent=1))
    counts = {}
    for r in results:
        counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
    print("\nsummary:", ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))


if __name__ == "__main__":
    main()
