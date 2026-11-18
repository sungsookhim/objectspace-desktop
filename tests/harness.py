"""Shared harness for the scenario suites (tests/m2, tests/m3): real wpsd
services on the loopback test bed (proto/m0/testbed.sh), as the current
user or as test users 60001/60002 through setpriv."""
import json
import os
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORK = Path(os.environ.get("WORK", f"/var/tmp/wps-m0-{os.environ.get('USER', os.getlogin())}"))
MNT = {fs: WORK / "mnt" / fs for fs in ("ext4", "btrfs", "fat")}
BIN = WORK / "bin"
OUT = ROOT / "build" / os.environ.get("WPS_SUITE", "m2")
results = []
scenarios = []
WATCHD = {"socket": "none"}   # set by a suite to use wps-watchd


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
            "WPS_WATCHD_SOCKET": WATCHD["socket"],
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


def prepare():
    """Check the test bed, copy binaries where test users can run them."""
    for fs, m in MNT.items():
        if not os.path.ismount(m):
            sys.exit(f"{m} is not mounted; run proto/m0/testbed.sh up first")
    OUT.mkdir(parents=True, exist_ok=True)
    BIN.mkdir(parents=True, exist_ok=True)
    for b in ("wpsd", "wpsctl", "wps-watchd"):
        if (ROOT / "build" / b).exists():
            shutil.copy(ROOT / "build" / b, BIN / b)
    os.chmod(WORK, 0o755)


def reset_services():
    run(["sudo", "rm", "-rf", str(WORK / "svc"), str(MNT["ext4"] / ".wps"), str(MNT["fat"] / ".wps")])


def run_scenarios(want, prefix=""):
    for num, title, fn in scenarios:
        if want and num not in want:
            continue
        try:
            ok, detail = fn()
            verdict = "PASS" if ok else "FAIL"
        except Exception as ex:
            verdict, detail = "ERROR", [repr(ex)]
        results.append({"num": prefix + num, "title": title, "verdict": verdict, "detail": detail})
        print(f"[{verdict:5}] {prefix + num:>6}. {title}")
        for line in detail:
            print("                 " + line)


def finish():
    bad = [f.name for f in OUT.glob("*.log") if "Sanitizer" in f.read_text(errors="replace") or
           "runtime error" in f.read_text(errors="replace")]
    print(f"\nsanitizer reports in service logs: {bad or 'none'}")
    (OUT / "report.json").write_text(json.dumps(results, indent=1))
    counts = {}
    for r in results:
        counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
    print("summary:", ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
    sys.exit(0 if not bad and counts.get("PASS", 0) == len(results) else 1)


# ------------------------------------------------- a direct protocol client

def _cb_head(major, n):
    if n < 24:
        return bytes([major << 5 | n])
    for info, size in ((24, 1), (25, 2), (26, 4), (27, 8)):
        if n < 1 << (8 * size):
            return bytes([major << 5 | info]) + n.to_bytes(size, "big")
    raise ValueError("too big")


def cbor_encode(v):
    if v is None:
        return b"\xf6"
    if v is True:
        return b"\xf5"
    if v is False:
        return b"\xf4"
    if isinstance(v, int):
        return _cb_head(0, v) if v >= 0 else _cb_head(1, -1 - v)
    if isinstance(v, str):
        b = v.encode()
        return _cb_head(3, len(b)) + b
    if isinstance(v, (list, tuple)):
        return _cb_head(4, len(v)) + b"".join(cbor_encode(x) for x in v)
    if isinstance(v, dict):
        return _cb_head(5, len(v)) + b"".join(cbor_encode(k) + cbor_encode(x) for k, x in v.items())
    raise TypeError(type(v))


def cbor_decode(b, i=0):
    ib = b[i]; i += 1
    major, info = ib >> 5, ib & 31
    if major == 7:
        if info == 20: return False, i
        if info == 21: return True, i
        if info in (22, 23): return None, i
        if info == 27:
            import struct
            return struct.unpack(">d", b[i:i + 8])[0], i + 8
        raise ValueError("simple")
    if info < 24:
        n = info
    else:
        size = {24: 1, 25: 2, 26: 4, 27: 8}[info]
        n = int.from_bytes(b[i:i + size], "big"); i += size
    if major == 0: return n, i
    if major == 1: return -1 - n, i
    if major in (2, 3):
        s = b[i:i + n]
        return (s.decode() if major == 3 else s), i + n
    if major == 4:
        out = []
        for _ in range(n):
            x, i = cbor_decode(b, i)
            out.append(x)
        return out, i
    if major == 5:
        out = {}
        for _ in range(n):
            k, i = cbor_decode(b, i)
            x, i = cbor_decode(b, i)
            out[k] = x
        return out, i
    raise ValueError("tag")


class Conn:
    """A persistent connection to a service (current user only), for
    measuring request latency without process start-up in the way."""

    def __init__(self, svc):
        import socket
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(svc.env["WPS_RUNTIME_DIR"] + "/wps.sock")
        self.next = 0

    def request(self, **msg):
        self.next += 1
        msg["id"] = self.next
        body = cbor_encode(msg)
        self.s.sendall(len(body).to_bytes(4, "big") + body)
        hdr = self._read(4)
        return cbor_decode(self._read(int.from_bytes(hdr, "big")))[0]

    def _read(self, n):
        out = b""
        while len(out) < n:
            chunk = self.s.recv(n - len(out))
            if not chunk:
                raise ConnectionError("closed")
            out += chunk
        return out

    def close(self):
        self.s.close()


def big_volume(size="6G", inodes=1300000):
    """A large ext4 scratch volume for scale scenarios, at <WORK>/mnt/big."""
    mnt = WORK / "mnt" / "big"
    if os.path.ismount(mnt):
        return mnt
    img = WORK / "img" / "big.img"
    run(["truncate", "-s", size, str(img)], check=True)
    run(["mkfs.ext4", "-q", "-F", "-N", str(inodes), "-L", "wpsbig", str(img)], check=True)
    mnt.mkdir(parents=True, exist_ok=True)
    run(["sudo", "mount", "-o", "loop", str(img), str(mnt)], check=True)
    run(["sudo", "chown", f"{os.getuid()}:{os.getgid()}", str(mnt)], check=True)
    return mnt
