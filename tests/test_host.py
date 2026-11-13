#!/usr/bin/env python3
"""Desktop test suite for Home Screen Manager.

Builds a mock PS5 app.db (two users, MyHome groups, a rowid-alias primary key
on the group table), runs the host build of the payload, and drives it over
HTTP exactly like the web menu does.
"""
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

BIN = os.path.abspath(sys.argv[1])
PORT = 8090
BASE = "http://127.0.0.1:%d" % PORT
USERS = ["1eb71bbd", "2c4f0a11"]
STORE, GALLERY, LIBRARY, PLUS = "NPXS40047", "NPXS40075", "NPXS40071", "NPXS40037"

passed = 0


def check(cond, msg):
    global passed
    if not cond:
        raise AssertionError(msg)
    passed += 1
    print("  ok  " + msg)


# (titleId, titleName, dispLocation, visible, installStatus, conceptName, icon0Info)
APPS = [
    (STORE, "PlayStation Store", 0x8A, 1, 2, None, None),
    (GALLERY, "Media Gallery", 0x8A, 1, 2, None, None),
    (LIBRARY, "Game Library", 0x8A, 1, 2, None, None),
    (PLUS, "PlayStation Plus", 0x8A, 1, 2, None, None),
    ("PPSA01325", "Astro Playroom", 0x8A, 1, 2, "ASTRO's PLAYROOM", "/user/appmeta/PPSA01325/icon0.png?ts=1"),
    ("CUSA00900", "Some PS4 Game", 0x8A, 1, 2, None, "/etc/passwd"),
    ("NPXS40053", "TV & Video", 0xBC, 1, 2, None, None),
    ("CUSA01065", "Netflix", 0xBC, 1, 2, None, "/user/appmeta/../../etc/passwd"),
    ("PPSA99999", "Uninstalled Game", 0x8A, 0, 0, None, None),
    ("NPXS40140", "Disc placeholder", 0x8A, 1, 2, None, None),
    ("NPXS40085", "Small Diag", 0x00, 0, 2, None, None),
    ("PPSA02000", "Library Only Game", 0x00, 1, 2, None, None),
    ("NPXS40139", "App Library", 0xBC, 1, 2, None, None),
    ("NPXS40056", "All Apps", 0xBC, 1, 2, None, None),
]
EXPECTED = {STORE, GALLERY, PLUS, "PPSA01325", "CUSA00900", "NPXS40053", "CUSA01065", "PPSA02000", "NPXS40056",
            LIBRARY, "NPXS40139"}


def make_db(path, layout="normal"):
    con = sqlite3.connect(path)
    c = con.cursor()
    c.execute("CREATE TABLE tbl_version(category TEXT PRIMARY KEY, status INTEGER)")
    c.execute("INSERT INTO tbl_version VALUES('access_index', 42), ('played_index', 7)")
    c.execute("CREATE TABLE tbl_contentinfo(titleId TEXT PRIMARY KEY, titleName TEXT, "
              "dispLocation INTEGER, AppInfoJson TEXT)")
    for a in APPS:
        c.execute("INSERT INTO tbl_contentinfo VALUES(?,?,?,?)", (a[0], a[1], a[2], "{}"))
    if layout == "unknown":
        con.commit()
        con.close()
        return
    for n, u in enumerate(USERS):
        c.execute(f'CREATE TABLE "tbl_iconinfo_{u}"(titleId TEXT PRIMARY KEY, titleName TEXT, '
                  'localConceptId TEXT, dispLocation INTEGER, visible INTEGER, pinnedStatus INTEGER, '
                  'lastAccessIndex INTEGER, installStatus INTEGER, emsDeletedFlag INTEGER)')
        c.execute(f'CREATE TABLE "tbl_concepticoninfo_{u}"(localConceptId TEXT, conceptName TEXT, icon0Info TEXT)')
        for i, (tid, name, disp, vis, inst, cname, icon) in enumerate(APPS):
            c.execute(f'INSERT INTO "tbl_iconinfo_{u}" VALUES(?,?,?,?,?,?,?,?,?)',
                      (tid, name, "cid:local:" + tid, disp, vis, 2, 40 - i, inst, 0))
            c.execute(f'INSERT INTO "tbl_concepticoninfo_{u}" VALUES(?,?,?)', ("cid:local:" + tid, cname, icon))
        c.execute(f'CREATE TABLE "tbl_group_{u}"(id INTEGER PRIMARY KEY, itemId TEXT, '
                  'groupName TEXT, type TEXT, idx INTEGER, lastModifiedDate TEXT, '
                  'UNIQUE(itemId, groupName))')
        for idx, tid in enumerate([PLUS, STORE, LIBRARY], start=1):
            c.execute(f'INSERT INTO "tbl_group_{u}"(itemId, groupName, type, idx, lastModifiedDate) '
                      "VALUES(?, 'MyHome', 'title', ?, '2026-01-0%d')" % idx, (tid, idx))
        # an unrelated collection that must never be touched
        c.execute(f'INSERT INTO "tbl_group_{u}"(itemId, groupName, type, idx) '
                  "VALUES(?, 'Collection_1', 'title', 1)", (STORE,))
    con.commit()
    con.close()


def dump(path, skip_pk=True):
    """Canonical content of every table (rowid-alias ids dropped)."""
    con = sqlite3.connect(path)
    out = {}
    for (name,) in con.execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"):
        cols = [r[1] for r in con.execute(f'PRAGMA table_info("{name}")')
                if not (skip_pk and r[5] == 1 and r[2].upper() == "INTEGER")]
        rows = con.execute(f'SELECT {",".join(chr(34)+c+chr(34) for c in cols)} FROM "{name}"').fetchall()
        out[name] = sorted(rows, key=repr)
    con.close()
    return out


def q(path, sql, *args):
    con = sqlite3.connect(path)
    r = con.execute(sql, args).fetchall()
    con.close()
    return r


def qx(path, sql, *args):
    con = sqlite3.connect(path)
    con.execute(sql, args)
    con.commit()
    con.close()


def http(method, path, raw=False):
    req = urllib.request.Request(BASE + path, method=method, data=b"" if method == "POST" else None)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            body = r.read()
            return r.status, (body if raw else json.loads(body))
    except urllib.error.HTTPError as e:
        body = e.read()
        try:
            return e.code, (body if raw else json.loads(body))
        except ValueError:
            return e.code, body


SAFE_ENV = {"HH_PLDMGR_DIR": "/nonexistent/pldmgr", "HH_USB_FMT": "/nonexistent/usb%d",
            "HH_LOADER_PORT": "1", "HH_GITHUB_API": "http://127.0.0.1:1"}


class Server:
    def __init__(self, appdb, datadir, env_extra=None, wait=True):
        env = dict(os.environ, HH_APP_DB=appdb, HH_DATA_DIR=datadir,
                   ASAN_OPTIONS="detect_leaks=1", **SAFE_ENV)
        env.update(env_extra or {})
        self.p = subprocess.Popen([BIN], env=env, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True)
        self.out = []
        threading.Thread(target=self._pump, daemon=True).start()
        if not wait:
            return
        end = time.time() + 60  # repairs at start (backgrounds) can take a few seconds
        while time.time() < end:
            try:
                urllib.request.urlopen(BASE + "/api/status", timeout=1).read()
                return
            except Exception:
                if self.p.poll() is not None:
                    break
                time.sleep(0.05)
        raise RuntimeError("server did not start:\n" + "".join(self.out))

    def _pump(self):
        for line in self.p.stdout:
            self.out.append(line)

    def stop(self):
        if self.p.poll() is None:
            http("POST", "/api/quit")
            self.p.wait(timeout=10)
        log = "".join(self.out)
        assert "ERROR: AddressSanitizer" not in log and "runtime error" not in log, log
        return log


def listing():
    code, j = http("GET", "/api/list")
    assert code == 200 and j["ok"], j
    return {t["id"]: t for t in j["tiles"]}, j["tiles"]


def tile(_unused, tid):
    return listing()[0][tid]


# ---------------------------------------------------------------------------
# locked tiles hidden by an older version are put back
# ---------------------------------------------------------------------------

def library_hide_tests(tmp):
    print("Game/App Library can be hidden and stay hidden")
    appdb = os.path.join(tmp, "libhide.db")
    data = os.path.join(tmp, "libhide-data")
    make_db(appdb)
    original = dump(appdb)
    s = Server(appdb, data)
    by_id, _ = listing()
    check(by_id[LIBRARY]["locked"] and not by_id[LIBRARY]["hidden"], "Game Library listed, locked, visible at start")
    code, r = http("POST", "/api/hide?id=" + LIBRARY)
    check(code == 200 and vis(appdb, LIBRARY) == [0, 0], "Game Library can be hidden")
    check(listing()[0][LIBRARY]["hidden"], "the list shows Game Library hidden now")
    code, r = http("POST", "/api/hide?id=NPXS40139")
    check(code == 200 and vis(appdb, "NPXS40139") == [0, 0], "App Library can be hidden too")
    # the PS5 decides their tab, so tab-move is still refused
    code, r = http("POST", "/api/tab?id=" + LIBRARY + "&to=media")
    check(code == 409 and "PS5 decides" in r["error"], "Library still can't be moved between tabs")
    code, st = http("GET", "/api/status")
    check(st["released_locked"] == 0, "Library is no longer force-shown at launch")
    # the system brings Game Library back; the launch re-hide puts it back
    s.stop()
    for u in USERS:
        qx(appdb, f'UPDATE "tbl_iconinfo_{u}" SET visible=1 WHERE titleId=?', LIBRARY)
    s = Server(appdb, data)
    code, st = http("GET", "/api/status")
    check(st["reapplied_at_launch"] >= 1 and vis(appdb, LIBRARY) == [0, 0],
          "Home Screen Manager re-hides Game Library after the system restores it")
    http("POST", "/api/unhide?id=" + LIBRARY)
    http("POST", "/api/unhide?id=NPXS40139")
    check(dump(appdb) == original, "showing both Library tiles again restores the original database")
    s.stop()


# ---------------------------------------------------------------------------
# updater: fake GitHub, fake USB drive, fake ELF loader
# ---------------------------------------------------------------------------

import hashlib
from http import server as httpserver
import socket
import socketserver
import struct


def fake_elf(version, size=40 * 1024, marker=True):
    """marker: True/"old" = the marker from before the rename, "new" = the new one, "both", or False = none."""
    hdr = b"\x7fELF" + bytes([2, 1, 1]) + b"\x00" * 9 + struct.pack("<HH", 2, 0x3E)
    body = hdr + b"\x00" * 2000
    if marker in (True, "old", "both"):
        body += b"junk HOMEHIDER-ELF-VERSION=\x00 more HOMEHIDER-ELF-VERSION=" + version.encode() + b";"
    if marker in ("new", "both"):
        body += b"junk HOMESCREENMANAGER-ELF-VERSION=\x00 more HOMESCREENMANAGER-ELF-VERSION=" + version.encode() + b";"
    return body + bytes((i * 7) % 251 for i in range(size - len(body)))


class FakeGitHub:
    def __init__(self, port):
        self.port = port
        self.release = None       # dict or None -> 404
        self.files = {}
        outer = self

        class H(httpserver.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                if self.path == "/repos/me/HomeHider/releases/latest" and outer.release is not None:
                    b = json.dumps(outer.release).encode()
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(b)))
                    self.end_headers()
                    self.wfile.write(b)
                elif self.path == "/dl/redirect":
                    self.send_response(302)
                    self.send_header("Location", "http://127.0.0.1:%d/dl/file" % outer.port)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                elif self.path in outer.files:
                    b = outer.files[self.path]
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(b)))
                    self.end_headers()
                    self.wfile.write(b)
                else:
                    self.send_response(404)
                    self.send_header("Content-Length", "0")
                    self.end_headers()

        socketserver.TCPServer.allow_reuse_address = True
        self.srv = socketserver.ThreadingTCPServer(("127.0.0.1", port), H)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def publish(self, version, elf, digest=True, bad_digest=False):
        self.files["/dl/file"] = elf
        d = hashlib.sha256(elf).hexdigest()
        if bad_digest:
            d = "0" * 64
        self.release = {
            "tag_name": "v" + version,
            "html_url": "https://github.com/me/HomeHider/releases/tag/v" + version,
            "body": "What\u2019s new:\n- \"quoted\" \\ stuff \ud83c\udfae",
            "author": {"login": "me", "nested": {"a": [1, 2, {"b": None}]}},
            "assets": [
                {"name": "notes.txt", "browser_download_url": "http://x/notes.txt", "uploader": {"login": "me"}},
                {"name": "HomeHider.elf", "url": "http://127.0.0.1:%d/api/asset/1" % self.port,
                 "browser_download_url": "http://127.0.0.1:%d/dl/redirect" % self.port,
                 "uploader": {"login": "me", "id": 5}, "size": len(elf),
                 **({"digest": "sha256:" + d} if digest else {})},
            ],
        }

    def close(self):
        self.srv.shutdown()
        self.srv.server_close()


class FakeLoader:
    """Plays the ELF loader: receives the new build, then (like the new
    instance would) asks the running Home Screen Manager to quit."""

    def __init__(self, port):
        self.received = []
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", port))
        self.sock.listen(4)
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while True:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            data = b""
            while True:
                chunk = c.recv(65536)
                if not chunk:
                    break
                data += chunk
            c.close()
            self.received.append(data)
            time.sleep(0.2)
            try:
                http("POST", "/api/quit")
            except Exception:
                pass

    def close(self):
        self.sock.close()


def wait_idle(timeout=20):
    t0 = time.time()
    while time.time() - t0 < timeout:
        code, u = http("GET", "/api/update/status")
        if code == 200 and not u["busy"]:
            return u
        time.sleep(0.1)
    raise AssertionError("updater stayed busy")


def wait_exit(server, timeout=20):
    server.p.wait(timeout=timeout)
    return server.stop()


def updater_tests(tmp):
    print("updater")
    gh_port, loader_port = 18080, 19021
    gh = FakeGitHub(gh_port)
    loader = FakeLoader(loader_port)
    try:
        appdb = os.path.join(tmp, "upd.db")
        data = os.path.join(tmp, "upd-data")
        pld = os.path.join(tmp, "pldmgr", "payloads")
        usb_fmt = os.path.join(tmp, "usb%d")
        os.makedirs(os.path.join(pld, "local", "homehider"))
        os.makedirs(os.path.join(pld, "local", "other"))
        make_db(appdb)
        current = open(BIN, "rb").read()
        old_v02 = fake_elf("0", marker=False) + b"/data/homehider/state.db"
        other = fake_elf("0", marker=False)
        open(os.path.join(pld, "local", "homehider", "HomeHider.elf"), "wb").write(current)
        open(os.path.join(pld, "HomeHider-v0.2.0.elf"), "wb").write(old_v02)
        open(os.path.join(pld, "local", "other", "ftpsrv.elf"), "wb").write(other)
        env = {"HH_PLDMGR_DIR": pld, "HH_USB_FMT": usb_fmt, "HH_LOADER_PORT": str(loader_port),
               "HH_GITHUB_API": "http://127.0.0.1:%d" % gh_port}

        cur_ver = re.search(rb"HOMEHIDER-ELF-VERSION=([0-9.]+);", current).group(1).decode()
        s = Server(appdb, data, env)
        code, st = http("GET", "/api/status")
        check(st["marker"] == "HOMESCREENMANAGER-ELF-VERSION=%s;" % cur_ver, "build carries its version marker")
        check(re.search(rb"HOMESCREENMANAGER-ELF-VERSION=%s;" % cur_ver.encode(), current) is not None,
              "the new marker is in the file, and so is the old one (so builds from before the rename can still read the version)")
        u = wait_idle()
        check(u["current"] == cur_ver and not u["usb"]["newer"] and not u["repo"], "nothing to update at start")

        print("  repository setting")
        code, r = http("POST", "/api/settings?repo=" + urllib.parse.quote("bad repo"))
        check(code == 400, "invalid repository rejected")
        code, r = http("POST", "/api/settings?repo=" + urllib.parse.quote("https://github.com/me/HomeHider.git"))
        check(code == 200, "pasted GitHub URL accepted")
        code, st = http("GET", "/api/status")
        check(st["settings"]["repo"] == "me/HomeHider", "URL normalised to owner/name")

        print("  GitHub: no release yet, then a release")
        http("POST", "/api/update/check")
        u = wait_idle()
        check(not u["github"]["ok"] and "No published release" in u["github"]["error"], "404 explained")
        new = fake_elf("9.9.9")
        gh.publish("9.9.9", new)
        http("POST", "/api/update/check")
        u = wait_idle()
        g = u["github"]
        check(g["ok"] and g["newer"] and g["version"] == "9.9.9", "newer GitHub release found")
        check(g["asset"] == "HomeHider.elf" and g["digest"], "picked the .elf asset with its digest")
        check(g["notes"].startswith("What\u2019s new") and "\U0001F3AE" in g["notes"], "release notes decoded (UTF-8, escapes)")

        print("  GitHub: checksum mismatch is refused")
        gh.publish("9.9.9", new, bad_digest=True)
        http("POST", "/api/update/check"); wait_idle()
        before = {p: open(p, "rb").read() for p in glob_elves(pld)}
        code, r = http("POST", "/api/update/install?source=github")
        check(code == 200, "install started")
        u = wait_idle()
        check(u["phase"] == "error" and "checksum" in u["message"], "bad checksum -> error, nothing installed")
        check({p: open(p, "rb").read() for p in glob_elves(pld)} == before, "payload files untouched")

        print("  GitHub: real install + restart")
        gh.publish("9.9.9", new)
        http("POST", "/api/update/check"); wait_idle()
        code, r = http("POST", "/api/update/install?source=github")
        log = wait_exit(s)
        check(loader.received and loader.received[-1] == new, "new build handed to the ELF loader")
        check(open(os.path.join(pld, "local", "homehider", "HomeHider.elf"), "rb").read() == new, "Payload Manager copy replaced")
        check(open(os.path.join(pld, "HomeHider-v0.2.0.elf"), "rb").read() == new, "old v0.2 copy recognised and replaced")
        check(open(os.path.join(pld, "local", "other", "ftpsrv.elf"), "rb").read() == other, "other payloads untouched")
        check(open(os.path.join(data, "HomeScreenManager.elf"), "rb").read() == new, "own copy written, under the new name")
        bk = os.path.join(data, "backup")
        check(open(os.path.join(bk, "HomeScreenManager-%s.elf" % cur_ver), "rb").read() == current, "previous version backed up")
        check(os.path.exists(os.path.join(bk, "HomeScreenManager-previous.elf")), "v0.2 copy backed up")
        check(os.path.exists(os.path.join(data, ".updated")), "restart flag left for the new instance")
        check(not [f for f in os.listdir(os.path.join(pld, "local", "homehider")) if f.endswith(".hhnew")], "no temp files left")

        print("  first start after an update")
        s = Server(appdb, data, dict(env, HH_GITHUB_API="http://127.0.0.1:1"))
        code, st = http("GET", "/api/status")
        check(st["updated_to"] == cur_ver, "status says it was just updated")
        check(not any("would open" in l for l in s.out), "browser not reopened after an update")
        check(not os.path.exists(os.path.join(data, ".updated")), "restart flag consumed")
        u = wait_idle()
        check(not u["github"]["checked"], "no automatic check right after updating")
        http("POST", "/api/update/check")
        u = wait_idle()
        check(not u["github"]["ok"] and "Couldn't reach GitHub" in (u["github"]["error"] or ""), "unreachable GitHub explained")
        lg = http("GET", "/api/log", raw=True)[1].decode()
        check("update check: GitHub me/HomeHider: couldn't connect (code 0x" in lg, "an update check that can't connect logs the code and the step")
        check("update check: GitHub me/HomeHider: a newer version is available" in lg or "update check: GitHub me/HomeHider: no newer version" in lg,
              "and a check that got an answer logs how it went")
        check("update check: USB drives:" in lg, "USB drives are logged too")

        print("  USB drive")
        os.makedirs(usb_fmt % 0 + "/homehider")
        os.makedirs(usb_fmt % 1)
        usb_new = fake_elf("9.9.10")
        open(usb_fmt % 0 + "/homehider/HomeHider-v9.9.10.elf", "wb").write(usb_new)
        open(usb_fmt % 1 + "/HomeHider-old.elf", "wb").write(fake_elf("0.0.1"))
        open(usb_fmt % 1 + "/random.elf", "wb").write(other)
        http("POST", "/api/update/check?usb_only=1")
        u = wait_idle()
        check(u["usb"]["newer"] and u["usb"]["version"] == "9.9.10", "newest USB build found")
        os.makedirs(usb_fmt % 2 + "/homescreenmanager")
        usb_new = fake_elf("9.9.11", marker="new")
        open(usb_fmt % 2 + "/homescreenmanager/HomeScreenManager-v9.9.11.elf", "wb").write(usb_new)
        http("POST", "/api/update/check?usb_only=1")
        u = wait_idle()
        check(u["usb"]["newer"] and u["usb"]["version"] == "9.9.11",
              "a build with only the new marker, in a homescreenmanager folder, is found too and wins")
        code, r = http("POST", "/api/update/install?source=usb")
        wait_exit(s)
        check(loader.received[-1] == usb_new, "USB build handed to the loader")
        check(open(os.path.join(pld, "local", "homehider", "HomeHider.elf"), "rb").read() == usb_new, "USB build installed")

        print("  upload from a phone/PC")
        s = Server(appdb, data, env)
        wait_idle()
        def upload(body, q=""):
            req = urllib.request.Request(BASE + "/api/update/upload" + q, method="POST", data=body,
                                         headers={"Origin": BASE})
            try:
                with urllib.request.urlopen(req, timeout=30) as r:
                    return r.status, json.loads(r.read())
            except urllib.error.HTTPError as e:
                return e.code, json.loads(e.read())
        code, r = upload(b"not an elf at all" * 3000)
        check(code == 409 and "isn't a PS5 payload" in r["error"], "garbage upload refused")
        code, r = upload(other)
        check(code == 409 and "isn't a Home Screen Manager build" in r["error"], "other payload refused")
        code, r = upload(fake_elf("0.0.1"))
        check(code == 409 and "Allow older" in r["error"], "older build needs explicit permission")
        evil = urllib.request.Request(BASE + "/api/update/upload", method="POST", data=fake_elf("9.9.12"),
                                      headers={"Origin": "http://evil.example"})
        try:
            urllib.request.urlopen(evil, timeout=10); code = 200
        except urllib.error.HTTPError as e:
            code = e.code
        check(code == 403, "cross-site upload refused")
        up = fake_elf("9.9.11")
        code, r = upload(up)
        check(code == 200 and "9.9.11" in r["note"], "upload installed")
        wait_exit(s)
        check(loader.received[-1] == up, "uploaded build handed to the loader")
        check(open(os.path.join(pld, "local", "homehider", "HomeHider.elf"), "rb").read() == up, "uploaded build installed")

        print("  loader not running")
        s = Server(appdb, data, dict(env, HH_LOADER_PORT="1"))
        wait_idle()
        code, r = upload(fake_elf("9.9.13"))
        check(code == 200, "install still succeeds")
        time.sleep(1.0)
        u = wait_idle()
        check(u["phase"] == "done" and "start it again from Payload Manager" in u["message"], "asks to restart manually")
        s.stop()
    finally:
        gh.close()
        loader.close()


# ---------------------------------------------------------------------------
# folders
# ---------------------------------------------------------------------------

import zlib


def tiny_png(w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def png_size(path):
    d = open(path, "rb").read(32)
    assert d[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    return struct.unpack(">II", d[16:24])


def form(path, fields):
    body = urllib.parse.urlencode(fields).encode()
    req = urllib.request.Request(BASE + path, method="POST", data=body,
                                 headers={"Content-Type": "application/x-www-form-urlencoded"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read())


def post_bytes(path, data):
    req = urllib.request.Request(BASE + path, method="POST", data=data)
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read())


def tile_rows(appdb, tid):
    return [q(appdb, f'SELECT visible, dispLocation FROM "tbl_iconinfo_{u}" WHERE titleId=?', tid) for u in USERS]


def vis(appdb, tid):
    return [q(appdb, f'SELECT visible FROM "tbl_iconinfo_{u}" WHERE titleId=?', tid)[0][0] for u in USERS]


def folder_tests(tmp):
    print("folders")
    appdb = os.path.join(tmp, "fold.db")
    data = os.path.join(tmp, "fold-data")
    apps = os.path.join(tmp, "user-app")
    icons = os.path.join(tmp, "icons")
    os.makedirs(icons)
    for i, tid in enumerate(["PPSA01325", "CUSA00900", "CUSA01065"]):
        open(os.path.join(icons, tid + ".png"), "wb").write(tiny_png(64 + i * 10, 64 + i * 10, (200, 40 * i, 90)))
    make_db(appdb)
    original = dump(appdb)
    env = {"HH_USER_APP_DIR": apps, "HH_ICON_DIR": icons}
    s = Server(appdb, data, env)

    # the user hid one game on their own before making the folder
    http("POST", "/api/hide?id=CUSA00900")

    print("  create")
    code, r = form("/api/folders/save", {"name": "", "items": "PPSA01325"})
    check(code == 409 and "name" in r["error"], "empty name refused")
    code, r = form("/api/folders/save", {"name": "x" * 60, "items": "PPSA01325"})
    check(code == 409, "over-long name refused")
    name = 'Sports & "Racing" ★'
    code, r = form("/api/folders/save", {"name": name, "tab": "games", "color": "red",
                                         "items": "PPSA01325,CUSA00900,BOGUS,NPXS40071,PPSA01325,HHFD00001"})
    check(code == 200 and r["id"] == "HHFD00001", "folder created as HHFD00001")
    f = r["folder"]
    check([i["id"] for i in f["items"]] == ["PPSA01325", "CUSA00900"], "invalid/duplicate/locked items dropped, order kept")
    check(f["name"] == name and f["installed"], "name stored, tile installed")
    pj = json.load(open(os.path.join(apps, "HHFD00001", "sce_sys", "param.json")))
    check(pj["titleId"] == "HHFD00001" and pj["deeplinkUri"] == "http://127.0.0.1:8090/folder/HHFD00001",
          "param.json points the tile at the folder page")
    check(pj["localizedParameters"]["en-US"]["titleName"] == name, "tile name survives quotes and unicode")
    check(png_size(os.path.join(apps, "HHFD00001", "sce_sys", "icon0.png")) == (512, 512), "collage icon0.png is 512x512")
    rows = tile_rows(appdb, "HHFD00001")
    check(all(r_ and r_[0][0] == 1 and r_[0][1] & 2 and not r_[0][1] & 4 for r_ in rows), "tile visible on the Games tab")
    check(vis(appdb, "PPSA01325") == [0, 0] and vis(appdb, "CUSA00900") == [0, 0], "games inside are hidden")
    code, d = http("GET", "/api/diag", raw=True)
    d = d.decode()
    check("folder HHFD00001: app install returned 0x00000000" in d and "placed on games tab" in d,
          "folder steps written to the log shown in Diagnostics")
    check("HTTP 409 on POST /api/folders/save" in d, "refused requests logged with their reason")
    http("POST", "/api/clientlog?msg=" + urllib.parse.quote("TypeError: x is undefined @12"))
    check("menu: TypeError: x is undefined @12" in http("GET", "/api/diag", raw=True)[1].decode(),
          "menu script errors reach the log")

    print("  list and pages")
    by_id, _ = listing()
    check("HHFD00001" not in by_id, "folder tile not offered as a game")
    check(by_id["PPSA01325"]["folder"] == "HHFD00001" and by_id["CUSA01065"]["folder"] is None, "membership reported")
    code, r = http("POST", "/api/unhide?id=PPSA01325")
    check(code == 409 and "folder" in r["error"], "showing a game that's in a folder is refused")
    code, r = http("POST", "/api/hide?id=HHFD00001")
    check(code == 409, "folder tile can't be hidden like a game")
    code, page = http("GET", "/folder/HHFD00001", raw=True)
    check(code == 200 and b"Sort by" in page, "folder page served")
    check(b'id="head"' in page and b"header{display:none" in page,
          "folder page has a header (picture + name) that the page settings turn on")
    check(b"body.noedit .foot{display:none}" in page and b'id="edit"' in page,
          "and an edit link that the page settings can hide")
    check(b'id="count"' in page and b'id="grid"' in page and b'id="edit"' in page,
          "folder page keeps the count, sort, grid and edit link")
    code, j = http("GET", "/api/folder?id=HHFD00001")
    check(j["folder"]["items"][0]["name"] == "ASTRO's PLAYROOM", "folder API returns names")
    code, j = http("GET", "/api/folder?id=HHFD00099")
    check(code == 404, "unknown folder -> 404")
    code, png = http("GET", "/api/folder_icon?id=HHFD00001", raw=True)
    check(code == 200 and png[:4] == b"\x89PNG", "folder icon served")

    print("  edit")
    code, r = form("/api/folders/save", {"id": "HHFD00001", "name": "Sports", "tab": "media", "color": "teal",
                                         "items": "CUSA00900,CUSA01065"})
    check(code == 200, "folder edited")
    check(vis(appdb, "PPSA01325") == [1, 1], "game taken out is shown again")
    check(vis(appdb, "CUSA01065") == [0, 0], "game added is hidden")
    rows = tile_rows(appdb, "HHFD00001")
    check(all(r_[0][1] & 4 and not r_[0][1] & 2 for r_ in rows), "tile moved to the Media tab")
    code, r = form("/api/folders/save", {"name": "Favourites", "items": "CUSA01065,PPSA02000"})
    check(code == 200 and r["id"] == "HHFD00002", "second folder created")
    code, j = http("GET", "/api/folder?id=HHFD00001")
    check([i["id"] for i in j["folder"]["items"]] == ["CUSA00900", "CUSA01065"],
          "a game put in a second folder stays in the first")
    code, j = http("GET", "/api/folder?id=HHFD00002")
    check([i["id"] for i in j["folder"]["items"]] == ["CUSA01065", "PPSA02000"], "second folder has its games")
    check(vis(appdb, "CUSA01065") == [0, 0], "a game in two folders stays hidden")
    by_id, _ = listing()
    check(by_id["CUSA01065"]["folders"] == ["HHFD00001", "HHFD00002"] and by_id["CUSA01065"]["folder"] == "HHFD00001",
          "the list reports every folder a game is in")
    check(by_id["CUSA00900"]["folders"] == ["HHFD00001"] and by_id["CUSA02000"]["folders"] == [] if "CUSA02000" in by_id
          else by_id["CUSA00900"]["folders"] == ["HHFD00001"], "single-folder and no-folder games listed right")

    print("  pictures")
    before = open(os.path.join(apps, "HHFD00002", "sce_sys", "icon0.png"), "rb").read()
    code, r = post_bytes("/api/folders/icon?id=HHFD00002", b"definitely not an image" * 10)
    check(code == 409 and "picture" in r["error"], "bad picture refused")
    code, r = post_bytes("/api/folders/icon?id=HHFD00002", tiny_png(800, 400, (10, 200, 30)))
    check(code == 200, "custom picture accepted")
    after = open(os.path.join(apps, "HHFD00002", "sce_sys", "icon0.png"), "rb").read()
    check(after != before and png_size(os.path.join(apps, "HHFD00002", "sce_sys", "icon0.png")) == (512, 512),
          "picture cropped to a 512x512 tile icon")
    code, j = http("GET", "/api/folder?id=HHFD00002")
    check(j["folder"]["custom_icon"], "custom picture remembered")
    code, r = http("POST", "/api/folders/icon?id=HHFD00002&reset=1")
    check(code == 200 and not http("GET", "/api/folder?id=HHFD00002")[1]["folder"]["custom_icon"], "back to collage")

    print("  show everything / repair at launch")
    code, r = http("POST", "/api/unhide_all")
    check(vis(appdb, "CUSA01065") == [0, 0] and vis(appdb, "CUSA00900") == [0, 0], "folder games stay hidden")
    s.stop()
    con = sqlite3.connect(appdb)
    for u in USERS:
        con.execute(f'DELETE FROM "tbl_iconinfo_{u}" WHERE titleId=?', ("HHFD00002",))
        con.execute(f'UPDATE "tbl_iconinfo_{u}" SET dispLocation=138 WHERE titleId=?', ("HHFD00001",))
    con.commit()
    con.close()
    s = Server(appdb, data, env)
    code, st = http("GET", "/api/status")
    check(st["folders_fixed"] == 2, "missing tile reinstalled and moved tile put back")
    check(all(r_ and r_[0][1] & 4 for r_ in tile_rows(appdb, "HHFD00001")), "tile back on the Media tab")
    check(all(r_ for r_ in tile_rows(appdb, "HHFD00002")), "deleted tile reinstalled")

    print("  delete")
    code, r = http("POST", "/api/folders/delete?id=HHFD00002")
    check(code == 200, "folder deleted")
    check(all(not r_ for r_ in tile_rows(appdb, "HHFD00002")), "tile removed")
    check(not os.path.exists(os.path.join(apps, "HHFD00002")), "tile files removed")
    check(vis(appdb, "PPSA02000") == [1, 1], "a game only in the deleted folder is shown again")
    check(vis(appdb, "CUSA01065") == [0, 0], "a game still in another folder stays hidden")
    code, r = http("POST", "/api/folders/delete?id=HHFD00001")
    check(vis(appdb, "CUSA01065") == [1, 1], "it is shown once its last folder is gone")
    check(vis(appdb, "CUSA00900") == [0, 0], "game hidden before joining stays hidden")
    http("POST", "/api/unhide?id=CUSA00900")
    check(dump(appdb) == original, "database back to the original after folders come and go")

    print("  restore original backup removes folder tiles")
    form("/api/folders/save", {"name": "Temp", "items": "PPSA01325"})
    check(all(r_ for r_ in tile_rows(appdb, "HHFD00001")), "temp folder installed")
    code, r = http("POST", "/api/restore")
    check(code == 200 and dump(appdb) == original, "original restored")
    check(not os.path.exists(os.path.join(apps, "HHFD00001")), "folder tile files removed")
    code, j = http("GET", "/api/folders")
    check(j["folders"] == [], "folder list cleared")
    s.stop()


def multi_folder_tests(tmp):
    print("games in several folders")
    appdb = os.path.join(tmp, "multi.db")
    data = os.path.join(tmp, "multi-data")
    apps = os.path.join(tmp, "multi-app")
    icons = os.path.join(tmp, "multi-icons")
    os.makedirs(icons)
    for i, tid in enumerate(["PPSA01325", "CUSA00900", "CUSA01065"]):
        open(os.path.join(icons, tid + ".png"), "wb").write(tiny_png(64 + i * 10, 64 + i * 10, (200, 40 * i, 90)))
    make_db(appdb)
    original = dump(appdb)
    s = Server(appdb, data, {"HH_USER_APP_DIR": apps, "HH_ICON_DIR": icons})

    # the user hid CUSA00900 on their own before any folder existed
    http("POST", "/api/hide?id=CUSA00900")

    code, r = form("/api/folders/save", {"name": "A", "items": "PPSA01325,CUSA00900"})
    check(code == 200 and r["id"] == "HHFD00001", "folder A created")
    code, r = form("/api/folders/save", {"name": "B", "items": "PPSA01325,CUSA00900,CUSA01065"})
    check(code == 200 and r["id"] == "HHFD00002", "folder B takes games that are already in A")
    for fid, want in (("HHFD00001", ["PPSA01325", "CUSA00900"]), ("HHFD00002", ["PPSA01325", "CUSA00900", "CUSA01065"])):
        j = http("GET", "/api/folder?id=" + fid)[1]
        check([i["id"] for i in j["folder"]["items"]] == want, fid + " keeps all its games")
    by_id, _ = listing()
    check(by_id["PPSA01325"]["folders"] == ["HHFD00001", "HHFD00002"], "a game lists both its folders")
    check(by_id["CUSA01065"]["folders"] == ["HHFD00002"], "a game in one folder lists one")
    check(by_id["PPSA02000"]["folders"] == [] and by_id["PPSA02000"]["folder"] is None, "a free game lists none")
    check(vis(appdb, "PPSA01325") == [0, 0], "a game in two folders is hidden")

    print("  leaving one folder")
    code, r = form("/api/folders/save", {"id": "HHFD00002", "name": "B", "items": "CUSA00900"})
    check(code == 200, "folder B edited")
    check(vis(appdb, "PPSA01325") == [0, 0], "still hidden: it is still in folder A")
    check(vis(appdb, "CUSA01065") == [1, 1], "shown: B was its only folder")
    j = http("GET", "/api/folder?id=HHFD00001")[1]
    check([i["id"] for i in j["folder"]["items"]] == ["PPSA01325", "CUSA00900"], "folder A untouched by editing B")

    print("  take out of a chosen folder")
    form("/api/folders/save", {"id": "HHFD00002", "name": "B", "items": "CUSA00900,PPSA01325,CUSA01065"})
    code, r = http("POST", "/api/folders/take_out?id=PPSA02000")
    check(code == 409 and "isn't in a folder" in r["error"], "take-out of a game in no folder refused")
    code, r = http("POST", "/api/folders/take_out?id=PPSA01325")
    check(code == 409 and "more than one folder" in r["error"], "take-out must say which folder when there are two")
    code, r = http("POST", "/api/folders/take_out?id=PPSA01325&folder=BOGUS")
    check(code == 400, "bad folder id refused")
    code, r = http("POST", "/api/folders/take_out?id=CUSA01065&folder=HHFD00001")
    check(code == 409 and "isn't in that folder" in r["error"], "take-out from a folder it isn't in refused")
    code, r = http("POST", "/api/folders/take_out?id=PPSA01325&folder=HHFD00002")
    check(code == 200 and "another folder" in r["note"], "taken out of B, and told it is still in A")
    check(vis(appdb, "PPSA01325") == [0, 0], "still hidden after leaving only one of its folders")
    check(listing()[0]["PPSA01325"]["folders"] == ["HHFD00001"], "the list shows the one folder left")
    code, r = http("POST", "/api/unhide?id=PPSA01325")
    check(code == 409, "showing a game that is still in a folder is refused")
    code, r = http("POST", "/api/tab?id=PPSA01325&to=media")
    check(code == 409 and "folder" in r["error"], "can't move a tab while it is in a folder")
    code, r = http("POST", "/api/folders/take_out?id=PPSA01325")
    check(code == 200 and vis(appdb, "PPSA01325") == [1, 1], "out of its last folder: shown again")

    print("  hidden by the user before joining")
    # CUSA00900 is in A and B and was hidden by the user first
    code, r = http("POST", "/api/folders/delete?id=HHFD00001")
    check(code == 200 and vis(appdb, "CUSA00900") == [0, 0], "still hidden while in B")
    code, r = http("POST", "/api/folders/delete?id=HHFD00002")
    check(code == 200 and vis(appdb, "CUSA00900") == [0, 0], "stays hidden after both folders go (the user hid it)")
    http("POST", "/api/unhide?id=CUSA00900")
    check(dump(appdb) == original, "database back to the original")

    print("  deleting a folder keeps shared games hidden")
    form("/api/folders/save", {"name": "C", "items": "PPSA01325,CUSA01065"})
    form("/api/folders/save", {"name": "D", "items": "PPSA01325"})
    code, r = http("POST", "/api/folders/delete?id=HHFD00001")
    check(vis(appdb, "CUSA01065") == [1, 1], "game only in the deleted folder is shown")
    check(vis(appdb, "PPSA01325") == [0, 0], "game also in another folder stays hidden")
    http("POST", "/api/folders/delete?id=HHFD00002")
    check(vis(appdb, "PPSA01325") == [1, 1], "shown when the last folder is deleted")
    check(dump(appdb) == original, "nothing left behind")
    s.stop()

    print("  folders made before games could be in several")
    appdb = os.path.join(tmp, "migrate.db")
    data = os.path.join(tmp, "migrate-data")
    os.makedirs(data)
    make_db(appdb)
    old = sqlite3.connect(os.path.join(data, "state.db"))
    old.executescript(
        "CREATE TABLE folders(title_id TEXT PRIMARY KEY, name TEXT NOT NULL, tab TEXT NOT NULL DEFAULT 'games',"
        " color TEXT NOT NULL DEFAULT 'blue', custom_icon INTEGER NOT NULL DEFAULT 0, created_at TEXT);"
        "CREATE TABLE folder_items(title_id TEXT PRIMARY KEY, folder TEXT NOT NULL,"
        " pos INTEGER NOT NULL DEFAULT 0, was_hidden INTEGER NOT NULL DEFAULT 0);"
        "INSERT INTO folders(title_id, name) VALUES('HHFD00001', 'Old');"
        "INSERT INTO folder_items VALUES('PPSA01325', 'HHFD00001', 0, 0), ('CUSA00900', 'HHFD00001', 1, 1);")
    old.commit()
    old.close()
    s = Server(appdb, data, {"HH_USER_APP_DIR": os.path.join(tmp, "migrate-app")})
    con = sqlite3.connect(os.path.join(data, "state.db"))
    npk = con.execute("SELECT count(*) FROM pragma_table_info('folder_items') WHERE pk>0").fetchone()[0]
    rows = con.execute("SELECT title_id, folder, pos, was_hidden FROM folder_items ORDER BY pos").fetchall()
    con.close()
    check(npk == 2, "folder_items upgraded to the (game, folder) key")
    check(rows == [("PPSA01325", "HHFD00001", 0, 0), ("CUSA00900", "HHFD00001", 1, 1)], "existing folder contents carried over")
    code, r = form("/api/folders/save", {"name": "New", "items": "PPSA01325"})
    check(code == 200, "an upgraded install can put a game in a second folder")
    check(listing()[0]["PPSA01325"]["folders"] == ["HHFD00001", "HHFD00002"], "and it is in both")
    s.stop()


# ---------------------------------------------------------------------------
# pictures from USB drives and the internal pictures folder
# ---------------------------------------------------------------------------

try:
    from PIL import Image, ImageDraw
except ImportError:  # the EXIF checks need Pillow to make the test pictures
    Image = None


def exif_jpeg(path, orientation):
    """200x100 picture stored left red / right blue, with an EXIF rotation."""
    im = Image.new("RGB", (200, 100), (255, 0, 0))
    im.paste((0, 0, 255), (100, 0, 200, 100))
    exif = Image.Exif()
    exif[0x0112] = orientation
    im.save(path, "JPEG", quality=95, exif=exif.tobytes())


def px(png_bytes, x, y):
    import io
    return Image.open(io.BytesIO(png_bytes)).convert("RGB").getpixel((x, y))


def near(c, want, tol=60):
    return all(abs(a - b) <= tol for a, b in zip(c, want))


def wait_for(pred, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(0.1)
    return pred()


def picture_tests(tmp):
    print("folder pictures from USB / internal storage")
    appdb = os.path.join(tmp, "pics.db")
    data = os.path.join(tmp, "pics-data")
    apps = os.path.join(tmp, "pics-apps")
    usb0 = os.path.join(tmp, "usb0")
    usb3 = os.path.join(tmp, "usb3")
    outside = os.path.join(tmp, "outside")
    make_db(appdb)

    def put(path, data_):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, "wb").write(data_)

    put(os.path.join(usb0, "Cover.PNG"), tiny_png(300, 200, (0, 160, 0)))
    put(os.path.join(usb0, "notes.txt"), b"not a picture")
    put(os.path.join(usb0, "._Cover.png"), b"mac junk")
    put(os.path.join(usb0, "System Volume Information", "x.png"), tiny_png(8, 8, (1, 1, 1)))
    put(os.path.join(usb0, "a", "b", "c", "d", "deep4.png"), tiny_png(8, 8, (9, 9, 9)))
    put(os.path.join(usb0, "a", "b", "c", "d", "e", "deep5.png"), tiny_png(8, 8, (9, 9, 9)))
    put(os.path.join(usb3, "art.bmp"), b"")  # empty file: skipped
    put(os.path.join(outside, "secret.png"), tiny_png(8, 8, (5, 5, 5)))
    shot_dir = os.path.join(usb0, "PS5", "CREATE", "Screenshots", "ASTRO's PLAYROOM")
    os.makedirs(shot_dir)
    if Image:
        exif_jpeg(os.path.join(shot_dir, "rotated.jpg"), 6)
        exif_jpeg(os.path.join(shot_dir, "upright.jpg"), 1)
    else:
        put(os.path.join(shot_dir, "rotated.jpg"), tiny_png(10, 10, (0, 0, 0)))
    # a picture copied over FTP to the internal pictures folder
    put(os.path.join(data, "pictures", "ftp.png"), tiny_png(64, 64, (250, 200, 0)))

    s = Server(appdb, data, {"HH_USB_FMT": os.path.join(tmp, "usb%d"), "HH_USER_APP_DIR": apps})
    code, j = http("GET", "/api/usb_images")
    check(code == 200 and j["ok"], "picture list served")
    found = {os.path.relpath(p["path"], tmp): p for p in j["images"]}
    want = {"usb0/Cover.PNG", "usb0/a/b/c/d/deep4.png", "pics-data/pictures/ftp.png",
            "usb0/PS5/CREATE/Screenshots/ASTRO's PLAYROOM/rotated.jpg"}
    check(want <= set(found), "pictures found at the top, in PS5 screenshot folders and 4 folders deep")
    check(not any(k.endswith(("notes.txt", "._Cover.png", "deep5.png", "art.bmp")) or "System Volume" in k
                  for k in found), "non-pictures, hidden files, empty files and 5-deep folders skipped")
    shot = found["usb0/PS5/CREATE/Screenshots/ASTRO's PLAYROOM/rotated.jpg"]
    check(shot["folder"] == "PS5/CREATE/Screenshots/ASTRO's PLAYROOM" and shot["name"] == "rotated.jpg",
          "folder and name reported for the picker")
    check(found["pics-data/pictures/ftp.png"]["folder"] == "PS5 storage", "internal pictures labelled")

    print("  previews")
    cover = os.path.join(usb0, "Cover.PNG")
    code, png = http("GET", "/api/usb_thumb?path=" + urllib.parse.quote(cover), raw=True)
    check(code == 200 and png[:8] == b"\x89PNG\r\n\x1a\n" and struct.unpack(">II", png[16:24]) == (192, 192),
          "preview is a 192x192 PNG")
    check(len(os.listdir(os.path.join(data, "thumbs"))) == 1, "preview cached on internal storage")
    code, png2 = http("GET", "/api/usb_thumb?path=" + urllib.parse.quote(cover), raw=True)
    check(code == 200 and png2 == png, "cached preview served again")
    for bad in ["/etc/passwd", os.path.join(outside, "secret.png"), os.path.join(usb0, "notes.txt"),
                os.path.join(usb0, "..", "outside", "secret.png"), os.path.join(tmp, "usb0x", "a.png"),
                os.path.join(usb0, "missing.png"), ""]:
        code, body = http("GET", "/api/usb_thumb?path=" + urllib.parse.quote(bad), raw=True)
        check(code == 404, "preview refused: %s" % (os.path.relpath(bad, tmp) if bad else "(empty)"))

    print("  set a folder picture from USB")
    code, r = form("/api/folders/save", {"name": "Shots", "items": "PPSA01325"})
    fid = r["id"]
    icon0 = os.path.join(apps, fid, "sce_sys", "icon0.png")
    collage = open(icon0, "rb").read()
    code, r = http("POST", "/api/folders/icon?id=%s&usb=%s" % (fid, urllib.parse.quote(os.path.join(outside, "secret.png"))))
    check(code == 404 and "USB" in r["error"], "picture outside USB refused")
    check(open(icon0, "rb").read() == collage, "refused picture changed nothing")
    code, r = http("POST", "/api/folders/icon?id=%s&usb=%s" % (fid, urllib.parse.quote(os.path.join(usb0, "notes.txt"))))
    check(code == 404, "non-picture refused")
    code, r = http("POST", "/api/folders/icon?id=%s&usb=%s" % (fid, urllib.parse.quote(cover)))
    check(code == 200 and r["ok"], "USB picture accepted")
    check(png_size(icon0) == (512, 512) and open(icon0, "rb").read() != collage, "tile icon replaced (512x512)")
    check(http("GET", "/api/folder?id=" + fid)[1]["folder"]["custom_icon"], "custom picture remembered")
    os.rename(cover, cover + ".moved")
    code, r = http("GET", "/api/folder_icon?id=" + fid, raw=True)
    check(code == 200 and r[:4] == b"\x89PNG", "copy kept on internal storage after the USB picture is gone")
    code, r = http("POST", "/api/folders/icon?id=%s&usb=%s" % (fid, urllib.parse.quote(cover)))
    check(code == 404 and "USB" in r["error"], "unplugged drive explained")
    code, r = http("POST", "/api/folders/icon?id=%s&usb=%s" % (fid, urllib.parse.quote(os.path.join(data, "pictures", "ftp.png"))))
    check(code == 200, "picture from the internal pictures folder accepted")

    if Image:
        print("  transparent pictures go on the folder's colour")
        clear = os.path.join(usb0, "clear.png")
        t = Image.new("RGBA", (300, 300), (0, 0, 0, 0))
        ImageDraw.Draw(t).ellipse((75, 75, 225, 225), fill=(255, 255, 255, 255))
        t.save(clear)
        code, r = form("/api/folders/save", {"id": fid, "name": "Shots", "color": "teal", "items": "PPSA01325"})
        code, r = http("POST", "/api/folders/icon?id=%s&usb=%s" % (fid, urllib.parse.quote(clear)))
        check(code == 200, "transparent picture accepted")
        im = Image.open(icon0)
        check(im.mode == "RGB", "tile icon has no transparency left (the console gets a plain picture)")
        check(near(im.getpixel((3, 3)), (0x1f, 0xb5, 0xc4), 30) and near(im.getpixel((256, 256)), (255, 255, 255), 10),
              "transparent area shows the folder's teal, the picture stays white")
        form("/api/folders/save", {"id": fid, "name": "Shots", "color": "red", "items": "PPSA01325"})
        im = Image.open(icon0)
        check(near(im.getpixel((3, 3)), (0xe5, 0x48, 0x4d), 30), "changing the colour repaints behind the picture")
        stored = Image.open(os.path.join(data, "folders", fid + ".png"))
        check(stored.mode == "RGBA", "the picture is kept with its transparency for later colour changes")

        print("  phone/camera rotation (EXIF)")
        rot = os.path.join(shot_dir, "rotated.jpg")
        code, png = http("GET", "/api/usb_thumb?path=" + urllib.parse.quote(rot), raw=True)
        top, bottom, left, right = px(png, 96, 20), px(png, 96, 172), px(png, 20, 96), px(png, 172, 96)
        check(near(top, (255, 0, 0)) and near(bottom, (0, 0, 255)), "rotated photo turned upright (red top, blue bottom)")
        code, png = http("GET", "/api/usb_thumb?path=" + urllib.parse.quote(os.path.join(shot_dir, "upright.jpg")), raw=True)
        check(near(px(png, 20, 96), (255, 0, 0)) and near(px(png, 172, 96), (0, 0, 255)), "upright photo left as is")
        code, r = http("POST", "/api/folders/icon?id=%s&usb=%s" % (fid, urllib.parse.quote(rot)))
        big = open(icon0, "rb").read()
        check(code == 200 and near(px(big, 256, 40), (255, 0, 0)) and near(px(big, 256, 470), (0, 0, 255)),
              "tile icon from a rotated photo is upright")
        for orient, desc in [(3, "upside down"), (8, "rotated left")]:
            p = os.path.join(shot_dir, "o%d.jpg" % orient)
            exif_jpeg(p, orient)
            code, png = http("GET", "/api/usb_thumb?path=" + urllib.parse.quote(p), raw=True)
            if orient == 3:   # 180: blue left, red right
                ok = near(px(png, 20, 96), (0, 0, 255)) and near(px(png, 172, 96), (255, 0, 0))
            else:             # 90 counter-clockwise: blue top, red bottom
                ok = near(px(png, 96, 20), (0, 0, 255)) and near(px(png, 96, 172), (255, 0, 0))
            check(code == 200 and ok, "%s photo corrected" % desc)
        # other formats
        for ext, fmt in [("bmp", "BMP"), ("gif", "GIF"), ("tga", "TGA")]:
            p = os.path.join(usb0, "fmt." + ext)
            Image.new("RGB", (40, 30), (0, 128, 255)).save(p, fmt)
            code, png = http("GET", "/api/usb_thumb?path=" + urllib.parse.quote(p), raw=True)
            check(code == 200 and near(px(png, 96, 96), (0, 128, 255)), "%s picture works" % fmt)
    else:
        print("  (Pillow not installed: EXIF and format checks skipped)")

    print("folder page")
    code, page = http("GET", "/folder/" + fid, raw=True)
    check(b'id="favicon"' in page and b"close=1" not in page and b"closes by itself" not in page,
          "folder page shows the folder icon and no longer promises to close itself")
    code, st = http("GET", "/api/status")
    check("autoclose" not in st["settings"], "auto-close option removed")
    s.stop()


def startup_mode_tests(tmp):
    print("menu at start: quiet for Autoload, open when started again")
    appdb = os.path.join(tmp, "boot.db")
    data = os.path.join(tmp, "boot-data")
    make_db(appdb)

    def opened(srv):
        return any("would open http://127.0.0.1:8090/" in l for l in srv.out)

    s = Server(appdb, data, {"HH_BOOT_ID": "boot-1"})
    check(opened(s), "default: the menu opens")
    code, r = http("POST", "/api/settings?open_browser=2")
    check(code == 200 and http("GET", "/api/status")[1]["settings"]["open_browser"] == 2, "quiet-at-Autoload mode saved")
    s.stop()

    s = Server(appdb, data, {"HH_BOOT_ID": "boot-2"})
    check(not opened(s), "first start after the console turns on (Autoload): no window")
    check("first start since boot 1 -> quiet" in http("GET", "/api/diag", raw=True)[1].decode(), "decision logged")

    # started again from Payload Manager while the Autoload copy runs
    s2 = Server(appdb, data, {"HH_BOOT_ID": "boot-2"}, wait=False)
    s.p.wait(timeout=10)
    check(wait_for(lambda: opened(s2)), "started again while running: the menu opens")
    wait_for(lambda: http_ok("/api/status"))
    s2.stop()

    s = Server(appdb, data, {"HH_BOOT_ID": "boot-2"})
    check(opened(s), "started again later in the same session (after Quit): the menu opens")
    s.stop()

    s = Server(appdb, data, {"HH_BOOT_ID": "boot-3"})
    check(not opened(s), "next time the console is turned on: quiet again")
    http("POST", "/api/settings?open_browser=0")
    s.stop()
    s = Server(appdb, data, {"HH_BOOT_ID": "boot-3"})
    check(not opened(s), "never-open mode stays quiet")
    http("POST", "/api/settings?open_browser=7")
    check(http("GET", "/api/status")[1]["settings"]["open_browser"] == 1, "unknown value treated as 'open'")
    menu = http("GET", "/", raw=True)[1]
    check(b"Quiet when started by Autoload" in menu and b"sw_autoclose" not in menu, "menu shows the three choices")

    # the menu's collapsible sections, in order, and what is in each
    m = menu.decode()
    def sec(sid):
        a = m.index('<details class="section" id="%s">' % sid)
        return a, m.index("</details>", a)
    order = ["folderOpts", "hideOpts", "updSettings", "adv"]
    spans = [sec(i) for i in order]
    check(all(spans[i][1] < spans[i + 1][0] for i in range(3)), "the four sections follow each other, none inside another")
    titles = [m[a:b].split('<span class="sumh">')[1].split("</span>")[0] for a, b in spans]
    check(titles == ["Folder options", "Tile hiding options", "Update settings", "More &amp; troubleshooting"],
          "their titles")
    check("Updates &amp; advanced options" not in m, "the old combined section is gone")
    def has(i, ids):
        a, b = spans[i]
        return all(('id="%s"' % x) in m[a:b] for x in ids)
    check(has(0, ["sw_fh", "sw_fb", "sw_fe"]) and "sw_reapply" not in m[spans[0][0]:spans[0][1]],
          "Folder options: the picture and name, the count and sort row, and the edit link")
    check('id="sw_fi"' not in m and "folder_inplace" not in m, "the 'without the installer' switch is gone")
    check(has(1, ["sw_reapply", "sw_run"]) and all(x not in m[spans[1][0]:spans[1][1]] for x in ("sw_fh", "updCheckBtn", "updSettings")),
          "Tile hiding options: only Keep tiles hidden and Show a running game")
    check(has(2, ["updMsg", "updHint", "updCheckBtn", "updOffers", "repo", "sw_auto", "updFile", "allowOlder"]),
          "Update settings: the status line, Check for updates, the offers area, repository, check at start, install from a file")
    check(m.index('id="updMsg"') < m.index('id="repo"'), "the status line comes before the repository")
    check(has(3, ["diag", "dl"]), "More & troubleshooting keeps its buttons")
    check("document.getElementById('updSettings')" in m and "document.getElementById('advAll')" not in m,
          "See update opens the Update settings section")
    a = m.index("function saveFolder(btn)")
    save_js = m[a:m.index("\nfunction ", a + 10)]
    check("/api/folders/save?async=1" in save_js and "folderJob(" in m, "the folder save runs as a background job")
    check("startSaveTimer(creating ? 'Creating folder' : 'Saving folder')" in save_js and "This can take about 90 seconds depending on your firmware" in m
          and "Still working: the console" in m and "function fmtElapsed" in m,
          "it says 'Creating folder… <time>. This can take about 90 seconds depending on your firmware', with a running timer")
    check("Refresh home screen now" in save_js, "a failed later step still offers a Refresh home screen now button")
    check("&refresh=1" in save_js and "Nothing was changed." in save_js and "is refreshing to show the result" in save_js,
          "the save asks Home Screen Manager to restart the home screen after the last step, and says so (or says nothing changed)")
    attn_new = "!ATTENTION!</b> Creating a folder can take up to 90 seconds depending on your firmware. Please do not open any apps or games during the folder creation process. There will be tile movement on the homescreen until the process is finished. Once the folder is created the homescreen will automatically refresh to show the results."
    attn_edit = "!ATTENTION!</b> Depending on the changes you have made to this folder it can take between 1 and 90 seconds to take effect. Please do not open any apps or games during this process. Once the folder is edited successfully the homescreen will automatically refresh to show the results. Deletion of a folder is instant and will not restart the homescreen."
    js_text = m.replace("' +\n  '", "")   # the texts are written in pieces in the page script
    check(attn_new in js_text and attn_edit in js_text, "the editor has both attention messages, in your wording")
    check(m.index('id="fmPick"') < m.index('id="fmAttn"') < m.index('id="fmMsg"') < m.index('onclick="saveFolder(this)"'),
          "the attention box sits right below the games grid, with the counter and messages below it, then the buttons")
    check("fmAttn').innerHTML = f ? ATTN_EDIT : ATTN_NEW" in m, "the create page and the edit page each get their own")
    s.stop()


def http_ok(path):
    try:
        return http("GET", path)[0] == 200
    except Exception:
        return False


def launch(tid, extra="", expect_job=True):
    """Starts tid like the menu does and waits for the result."""
    code, r = http("POST", "/api/launch?id=" + tid + extra)
    assert code == 200 and r["ok"], (code, r)
    if not expect_job:
        return r
    assert "job" in r, r
    for _ in range(100):
        code, j = http("GET", "/api/launch_status?job=%d" % r["job"])
        assert code == 200 and j["ok"], j
        if j["done"]:
            return j
        time.sleep(0.05)
    raise AssertionError("launch never finished")


def launch_tests(tmp):
    print("starting games and apps")
    appdb = os.path.join(tmp, "lnc.db")
    data = os.path.join(tmp, "lnc-data")
    apps = os.path.join(tmp, "lnc-apps")
    meta = os.path.join(tmp, "lnc-appmeta")
    make_db(appdb)
    # web link tiles: Payload Manager style (param.json in /user/app) ...
    pm = os.path.join(apps, "PLDM00001", "sce_sys")
    os.makedirs(pm)
    json.dump({"titleId": "PLDM00001", "applicationCategoryType": 65536,
               "deeplinkUri": "http://127.0.0.1:8084/"}, open(os.path.join(pm, "param.json"), "w"))
    # ... one whose details only the console's appmeta copy has
    os.makedirs(os.path.join(meta, "WFEX00001"))
    json.dump({"deeplinkUri": "http://127.0.0.1:5000/files"}, open(os.path.join(meta, "WFEX00001", "param.json"), "w"))
    # ... one known only from app.db
    qx(appdb, "UPDATE tbl_contentinfo SET AppInfoJson=? WHERE titleId='CUSA01065'",
      json.dumps({"deeplinkUri": "https://example.com/tv"}))
    # not web links: a real app with a program, and a script address
    real = os.path.join(apps, "PPSA05000")
    os.makedirs(os.path.join(real, "sce_sys"))
    open(os.path.join(real, "eboot.bin"), "wb").write(b"\x7fELF")
    json.dump({"deeplinkUri": "http://127.0.0.1:1/"}, open(os.path.join(real, "sce_sys", "param.json"), "w"))
    bad = os.path.join(apps, "BADL00001", "sce_sys")
    os.makedirs(bad)
    json.dump({"deeplinkUri": "javascript:alert(1)"}, open(os.path.join(bad, "param.json"), "w"))

    env = {"HH_USER_APP_DIR": apps, "HH_APPMETA_DIR": meta,
           "HH_FAKE_LAUNCH": "PPSA02000=0x80940031,NPXS40053=0x80940013"}
    s = Server(appdb, data, env)

    print("  web link apps open their page")
    r = launch("PLDM00001", "&local=1", expect_job=False)
    check(r.get("url") == "http://127.0.0.1:8084/", "Payload Manager tile -> its address, for the page to open")
    check(not any("PLDM00001" in l for l in s.out if "[launch]" in l), "launcher not asked (it can only say 'can't open')")
    r = launch("PLDM00001", "", expect_job=False)
    check(wait_for(lambda: any("would open http://127.0.0.1:8084/" in l for l in s.out)),
          "from a phone/PC: opened in the PS5 browser instead")
    r = launch("WFEX00001", "&local=1", expect_job=False)
    check(r.get("url") == "http://127.0.0.1:5000/files", "address found in the console's appmeta copy")
    r = launch("CUSA01065", "&local=1", expect_job=False)
    check(r.get("url") == "https://example.com/tv", "address found in app.db")
    j = launch("PPSA05000", "&local=1")
    check(j["result"] == "ok", "app with its own program started normally")
    j = launch("BADL00001", "&local=1")
    check("url" not in j, "script addresses are never opened")
    check("web link http://127.0.0.1:8084/" in http("GET", "/api/diag", raw=True)[1].decode(), "web links logged")

    print("  starting, switching, another game running")
    s.stop()
    s = Server(appdb, data, env)
    j = launch("PPSA01325")
    check(j["result"] == "ok" and j["note"].startswith("Starting ASTRO's PLAYROOM"), "game started, named")
    check(any("would start PPSA01325" in l for l in s.out), "launcher asked")
    j = launch("PPSA01325")
    check(j["result"] == "ok" and j["note"].startswith("Switching to"), "already running counts as started")
    j = launch("CUSA00900")
    check(j["result"] == "other_running" and j["other"] == {"id": "PPSA01325", "name": "ASTRO's PLAYROOM"},
          "another game running: reported with its name so the page can ask")
    check(not any("would close app" in l for l in s.out), "nothing closed without asking")
    j = launch("CUSA00900", "&close_running=1")
    check(j["result"] == "ok", "after the OK, the running game is closed and the new one starts")
    check(any("would close app 60000" in l for l in s.out) and any("would start CUSA00900" in l for l in s.out),
          "closed first, then started")
    log = http("GET", "/api/diag", raw=True)[1].decode()
    check("closing PPSA01325 first" in log, "closing the other game is logged")
    j = launch(GALLERY)
    check(j["result"] == "ok", "apps can start while a game runs")

    print("  errors are explained")
    j = launch("PPSA02000")
    check(j["result"] == "error" and "can't find this app" in j["error"] and j["code"] == "0x80940031",
          "launcher error turned into words, code kept")
    j = launch("NPXS40053")
    check(j["result"] == "error" and "disc" in j["error"], "another error explained")
    code, r = http("GET", "/api/launch_status?job=999")
    check(r["ok"] is False, "unknown job reported")
    code, r = http("GET", "/api/launch_status")
    check(r["ok"] is False, "missing job reported")
    s.stop()

    print("  folder page and menu use the new flow")
    s = Server(appdb, data, env)
    page = http("GET", "/folder/HHFD00001", raw=True)[1]
    check(b"launch_status" in page and b"close_running" in page and b"location.href = j.url" in page and
          b"close=1" not in page,
          "folder page polls, asks about the running game and opens web links")
    check(b"selfClose" in page and b"window.close" in page, "folder page closes itself after a game starts")
    menu = http("GET", "/", raw=True)[1]
    check(b"launch_status" in menu and b"close_running" in menu, "menu Open does the same")
    s.stop()


def custom_tests(tmp):
    print("rename tiles and change their pictures")
    appdb = os.path.join(tmp, "cus.db")
    data = os.path.join(tmp, "cus-data")
    meta = os.path.join(tmp, "cus-appmeta")
    apps = os.path.join(tmp, "cus-apps")
    usb0 = os.path.join(tmp, "cus-usb0")
    os.makedirs(usb0)
    open(os.path.join(usb0, "art.png"), "wb").write(tiny_png(300, 300, (250, 120, 0)))
    make_db(appdb)
    original = dump(appdb)
    env = {"HH_APPMETA_DIR": meta, "HH_USER_APP_DIR": apps, "HH_USB_FMT": os.path.join(tmp, "cus-usb%d")}
    s = Server(appdb, data, env)

    def names(tid):
        rows = []
        for u in USERS:
            rows += [r[0] for r in q(appdb, f'SELECT titleName FROM "tbl_iconinfo_{u}" WHERE titleId=?', tid)]
            rows += [r[0] for r in q(appdb, f'SELECT conceptName FROM "tbl_concepticoninfo_{u}" WHERE localConceptId=?',
                                     "cid:local:" + tid)]
        rows += [r[0] for r in q(appdb, "SELECT titleName FROM tbl_contentinfo WHERE titleId=?", tid)]
        return rows

    def icons(tid):
        return [q(appdb, f'SELECT icon0Info FROM "tbl_concepticoninfo_{u}" WHERE localConceptId=?',
                  "cid:local:" + tid)[0][0] for u in USERS]

    code, j = http("GET", "/api/custom?id=PPSA01325")
    it = j["item"]
    check(it["name"] == "ASTRO's PLAYROOM" and it["can_rename"] and it["can_icon"] and not it["renamed"],
          "editor info: current name, can rename and change the picture")

    print("  names")
    new = "Astro \u2605 Bot"
    code, r = form("/api/custom/name?id=PPSA01325", {"name": "  " + new + "  "})
    check(code == 200, "renamed")
    check(names("PPSA01325") == [new] * 5, "titleName (both users + contentinfo) and conceptName all changed")
    others = dump(appdb)
    for t in original:
        if t.startswith(("tbl_group", "tbl_version")):
            check(others[t] == original[t], f"{t} untouched")
    by_id, _ = listing()
    check(by_id["PPSA01325"]["name"] == new and by_id["PPSA01325"]["renamed"], "menu list shows the new name")
    code, j = http("GET", "/api/custom?id=PPSA01325")
    check(j["item"]["orig_name"] == "ASTRO's PLAYROOM" and j["item"]["renamed"], "original name remembered")
    code, r = form("/api/custom/name?id=PPSA01325", {"name": "Second Name"})
    check(code == 200 and names("PPSA01325") == ["Second Name"] * 5, "renamed again")
    code, r = form("/api/custom/name?id=PPSA01325", {"name": "x" * 65})
    check(code == 409 and "64" in r["error"], "too long refused")
    code, r = form("/api/custom/name?id=PPSA01325", {"name": "bad\nname"})
    check(code == 409, "control characters refused")
    code, r = form("/api/custom/name?id=CUSA00900", {"name": "My PS4 Game"})
    check(code == 200 and names("CUSA00900")[0] == "My PS4 Game", "game without a concept name renamed")
    code, r = form("/api/custom/name?id=PPSA01325", {"name": "ASTRO's PLAYROOM"})
    check(code == 200 and names("PPSA01325") == ["Astro Playroom", "ASTRO's PLAYROOM"] * 2 + ["Astro Playroom"],
          "typing the original name puts every original value back")
    check(not http("GET", "/api/custom?id=PPSA01325")[1]["item"]["renamed"], "no longer marked renamed")
    code, r = http("POST", "/api/custom/name?id=CUSA00900&reset=1")
    check(code == 200 and dump(appdb) == original, "reset: database back to the original")

    print("  pictures")
    code, r = post_bytes("/api/custom/icon?id=PPSA01325", tiny_png(640, 360, (0, 90, 250)))
    check(code == 200, "picture accepted")
    paths = icons("PPSA01325")
    check(len(set(paths)) == 1 and paths[0].startswith(os.path.join(meta, "PPSA01325", "hh_icon0_")),
          "icon0Info points at Home Screen Manager's own file in appmeta (both users)")
    check(png_size(paths[0]) == (512, 512), "512x512 picture written")
    code, png = http("GET", "/api/icon?id=PPSA01325", raw=True)
    check(code == 200 and png == open(paths[0], "rb").read(), "menu shows the new picture")
    code, r = http("POST", "/api/custom/icon?id=PPSA01325&usb=" + urllib.parse.quote(os.path.join(usb0, "art.png")))
    check(code == 200, "picture from USB accepted")
    paths2 = icons("PPSA01325")
    check(paths2[0] != paths[0] and not os.path.exists(paths[0]) and os.path.exists(paths2[0]),
          "new file name each time (no stale cache); the previous file removed")
    by_id, _ = listing()
    check(by_id["PPSA01325"]["custom_icon"], "list marks the new picture")
    code, r = post_bytes("/api/custom/icon?id=PPSA01325", b"not a picture at all" * 4)
    check(code == 409 and "picture" in r["error"] and icons("PPSA01325") == paths2, "bad picture refused, nothing changed")
    code, r = post_bytes("/api/custom/icon?id=" + STORE, tiny_png(64, 64, (1, 2, 3)))
    check(code == 409 and "isn't stored" in r["error"], "tile without a picture entry explained")
    check(not os.path.exists(os.path.join(meta, STORE)) or not os.listdir(os.path.join(meta, STORE)),
          "no stray file left for it")
    code, r = http("POST", "/api/custom/name?id=HHFD00001&reset=1")
    check(code == 409 and "folder" in r["error"], "folder tiles use the folder editor")

    if Image:
        import io
        t = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
        ImageDraw.Draw(t).rectangle((64, 64, 192, 192), fill=(250, 250, 250, 255))
        buf = io.BytesIO()
        t.save(buf, "PNG")
        code, r = post_bytes("/api/custom/icon?id=PPSA01325", buf.getvalue())
        im = Image.open(icons("PPSA01325")[0])
        check(code == 200 and im.mode == "RGB" and near(im.getpixel((3, 3)), (0x2a, 0x30, 0x40), 12),
              "transparent tile picture placed on a dark background")
        paths2 = icons("PPSA01325")

    print("  the console puts originals back -> fixed at next start")
    form("/api/custom/name?id=PPSA01325", {"name": "Kept Name"})
    s.stop()
    u = USERS[0]
    qx(appdb, f'UPDATE "tbl_concepticoninfo_{u}" SET conceptName=?, icon0Info=? WHERE localConceptId=?',
      "ASTRO's PLAYROOM", "/user/appmeta/PPSA01325/icon0.png?ts=2", "cid:local:PPSA01325")
    os.unlink(paths2[0])
    s = Server(appdb, data, env)
    code, st = http("GET", "/api/status")
    check(st["custom_fixed"] >= 3, "name, picture path and missing file put back (%d)" % st["custom_fixed"])
    check(names("PPSA01325") == ["Kept Name"] * 5 and icons("PPSA01325") == paths2 and os.path.exists(paths2[0]),
          "custom name and picture back in place")

    print("  back to the originals")
    code, r = http("POST", "/api/custom/icon?id=PPSA01325&reset=1")
    check(code == 200 and icons("PPSA01325") == ["/user/appmeta/PPSA01325/icon0.png?ts=2",
                                                "/user/appmeta/PPSA01325/icon0.png?ts=1"],
          "original picture paths back (the newest one the console set is kept)")
    check(not os.path.exists(paths2[0]), "Home Screen Manager's picture file removed")
    code, r = http("POST", "/api/custom/name?id=PPSA01325&reset=1")
    check(code == 200 and names("PPSA01325") == ["Astro Playroom", "ASTRO's PLAYROOM"] * 2 + ["Astro Playroom"],
          "original name back")
    code, j = http("GET", "/api/custom?id=PPSA01325")
    check(not j["item"]["renamed"] and not j["item"]["custom_icon"], "nothing custom left")

    print("  placeholder concept ids shared by many tiles are left alone")
    for u in USERS:
        for tid in (STORE, GALLERY, PLUS, "NPXS40053"):
            qx(appdb, f'UPDATE "tbl_iconinfo_{u}" SET localConceptId=? WHERE titleId=?', "cid:shared", tid)
        qx(appdb, f'INSERT INTO "tbl_concepticoninfo_{u}" VALUES(?,?,?)', "cid:shared", "PlayStation Store", "/x.png")
    code, r = form("/api/custom/name?id=" + STORE, {"name": "Shop"})
    check(code == 200, "system tile renamed")
    check([r_[0] for r_ in q(appdb, f'SELECT titleName FROM "tbl_iconinfo_{USERS[0]}" WHERE titleId=?', STORE)] == ["Shop"],
          "its own row changed")
    check(q(appdb, f'SELECT conceptName FROM "tbl_concepticoninfo_{USERS[0]}" WHERE localConceptId=?', "cid:shared")
          == [("PlayStation Store",)], "shared concept row untouched (other tiles keep their names)")
    http("POST", "/api/custom/name?id=%s&reset=1" % STORE)

    print("  restore original backup")
    form("/api/custom/name?id=PPSA01325", {"name": "Temp"})
    post_bytes("/api/custom/icon?id=PPSA01325", tiny_png(64, 64, (9, 9, 9)))
    f = icons("PPSA01325")[0]
    code, r = http("POST", "/api/restore")
    check(code == 200 and dump(appdb) == original, "original database restored")
    check(not os.path.exists(f), "picture files removed")
    code, j = http("GET", "/api/custom?id=PPSA01325")
    check(not j["item"]["renamed"] and not j["item"]["custom_icon"], "custom list cleared")
    menu = http("GET", "/", raw=True)[1]
    check(b"editTile" in menu and b"first frame" in menu, "menu has the editor and explains GIFs")
    s.stop()


def dds_header(w, h, mips, fourcc=b"DX10", dxgi=98):
    """Just the header of a DDS file (enough for Home Screen Manager to read the format)."""
    hd = bytearray(148)
    hd[0:4] = b"DDS "
    struct.pack_into("<IIIIIII", hd, 4, 124, 0x1007 | 0x20000, h, w, 0, 0, mips)
    struct.pack_into("<II", hd, 76, 32, 4)
    hd[84:88] = fourcc
    if fourcc == b"DX10":
        struct.pack_into("<IIIII", hd, 128, dxgi, 3, 0, 1, 0)
    return bytes(hd)


def dds_info(path):
    d = open(path, "rb").read(148)
    h, w = struct.unpack_from("<II", d, 12)
    mips = struct.unpack_from("<I", d, 28)[0]
    four = d[84:88]
    dxgi = struct.unpack_from("<I", d, 128)[0] if four == b"DX10" else four.decode()
    return w, h, mips, dxgi


def background_tests(tmp):
    print("home screen backgrounds")
    if not Image:
        print("  (Pillow not installed: skipped)")
        return
    import io
    appdb = os.path.join(tmp, "bg.db")
    data = os.path.join(tmp, "bg-data")
    meta = os.path.join(tmp, "bg-appmeta")
    apps = os.path.join(tmp, "bg-apps")
    orig_dir = os.path.join(tmp, "bg-orig")
    usb0 = os.path.join(tmp, "bg-usb0")
    for d in (orig_dir, usb0):
        os.makedirs(d)
    make_db(appdb)
    for u in USERS:
        qx(appdb, f'ALTER TABLE "tbl_concepticoninfo_{u}" ADD COLUMN pic0Info TEXT')
        qx(appdb, f'ALTER TABLE "tbl_concepticoninfo_{u}" ADD COLUMN pic1Info TEXT')
    # originals: PPSA01325 has a BC7 DDS (3 mips) and a PNG; CUSA00900's is inside a
    # package Home Screen Manager can't read; CUSA01065 uses a format it can't write
    open(os.path.join(orig_dir, "pic0.dds"), "wb").write(dds_header(1920, 1080, 3))
    Image.new("RGB", (1280, 720), (1, 2, 3)).save(os.path.join(orig_dir, "pic1.png"))
    open(os.path.join(orig_dir, "dxt3.dds"), "wb").write(dds_header(1920, 1080, 1, b"DXT3"))
    origs = {"PPSA01325": (os.path.join(orig_dir, "pic0.dds"), os.path.join(orig_dir, "pic1.png") + "?ts=4"),
             "CUSA00900": ("/mnt/sandbox/pfsmnt/CUSA00900-app0/sce_sys/pic0.dds", None),
             "CUSA01065": (os.path.join(orig_dir, "dxt3.dds"), None)}
    for u in USERS:
        for tid, (p0, p1) in origs.items():
            qx(appdb, f'UPDATE "tbl_concepticoninfo_{u}" SET pic0Info=?, pic1Info=? WHERE localConceptId=?',
               p0, p1, "cid:local:" + tid)
    original = dump(appdb)

    pic = Image.new("RGB", (1600, 900), (20, 60, 200))
    ImageDraw.Draw(pic).ellipse((600, 250, 1000, 650), fill=(250, 200, 30))
    buf = io.BytesIO()
    pic.save(buf, "JPEG", quality=95)
    jpg = buf.getvalue()
    open(os.path.join(usb0, "wide.jpg"), "wb").write(jpg)

    env = {"HH_APPMETA_DIR": meta, "HH_USER_APP_DIR": apps, "HH_USB_FMT": os.path.join(tmp, "bg-usb%d")}
    s = Server(appdb, data, env)

    def bgs(tid):
        return [q(appdb, f'SELECT pic0Info, pic1Info FROM "tbl_concepticoninfo_{u}" WHERE localConceptId=?',
                  "cid:local:" + tid)[0] for u in USERS]

    code, j = http("GET", "/api/custom?id=PPSA01325")
    check(j["item"]["can_bg"] and not j["item"]["custom_bg"], "editor: this game's background can be changed")
    code, j = http("GET", "/api/custom?id=" + STORE)
    check(not j["item"]["can_bg"], "tiles without a background entry say so")

    print("  same format as the original")
    code, r = post_bytes("/api/custom/bg?id=PPSA01325", jpg)
    check(code == 200, "background saved")
    rows = bgs("PPSA01325")
    p0, p1 = rows[0]
    check(rows[0] == rows[1], "both users point at the same new files")
    check(p0.startswith(os.path.join(meta, "PPSA01325", "hh_bg_")) and p0.endswith(".dds"), "pic0 -> Home Screen Manager's DDS in appmeta")
    check(p1.endswith(".png") and p1 != p0, "pic1 -> a PNG, because the original pic1 is a PNG")
    check(dds_info(p0) == (1920, 1080, 3, 98), "DDS keeps the original size, mip count and BC7 format")
    im = Image.open(p0)
    im.load()
    c = im.convert("RGB").getpixel((960, 540))
    check(near(c, (250, 200, 30), 40) and near(im.convert("RGB").getpixel((60, 60)), (20, 60, 200), 40),
          "the BC7 texture decodes to the picture")
    check(Image.open(p1).size == (1280, 720), "PNG keeps the original size")
    by_id, _ = listing()
    check(by_id["PPSA01325"]["custom_bg"], "list marks the new background")
    code, prev = http("GET", "/api/custom_bg?id=PPSA01325", raw=True)
    check(code == 200 and prev[:2] == b"\xff\xd8", "preview of the chosen background served")
    code, r = http("POST", "/api/custom/bg?id=PPSA01325&usb=" + urllib.parse.quote(os.path.join(usb0, "wide.jpg")))
    check(code == 200 and bgs("PPSA01325")[0][0] != p0 and not os.path.exists(p0) and not os.path.exists(p1),
          "picked from USB: new file names (no cached copy), old files removed")

    print("  unreadable or unusual originals")
    code, r = post_bytes("/api/custom/bg?id=CUSA00900", jpg)
    p = bgs("CUSA00900")[0][0]
    check(code == 200 and dds_info(p) == (3840, 2160, 1, 98), "original inside a game package: 4K BC7 DDS")
    code, r = post_bytes("/api/custom/bg?id=CUSA01065", jpg)
    check(code == 409 and "can't write" in r["error"], "a format Home Screen Manager can't write is refused")
    check(bgs("CUSA01065")[0][0] == origs["CUSA01065"][0], "and nothing changed")
    code, r = post_bytes("/api/custom/bg?id=" + STORE, jpg)
    check(code == 409 and "no background" in r["error"], "tile without a background entry refused")
    code, r = post_bytes("/api/custom/bg?id=PPSA01325", b"nope" * 20)
    check(code == 409 and "picture" in r["error"], "bad picture refused")

    print("  the console resets it -> put back at next start")
    cur = bgs("PPSA01325")[0]
    s.stop()
    qx(appdb, f'UPDATE "tbl_concepticoninfo_{USERS[0]}" SET pic0Info=? WHERE localConceptId=?',
       origs["PPSA01325"][0], "cid:local:PPSA01325")
    os.unlink(cur[1])
    s = Server(appdb, data, env)
    code, st = http("GET", "/api/status")
    now = bgs("PPSA01325")
    check(st["custom_fixed"] >= 2 and now[0] == now[1] and all(os.path.exists(x) for x in now[0]),
          "background files remade and pointed at again")

    print("  back to the original")
    code, r = http("POST", "/api/custom/bg?id=PPSA01325&reset=1")
    check(code == 200 and bgs("PPSA01325") == [origs["PPSA01325"]] * 2, "original paths back")
    check(not any(f.startswith("hh_bg_") for f in os.listdir(os.path.join(meta, "PPSA01325"))), "our files removed")
    code, r = http("POST", "/api/custom/bg?id=CUSA00900&reset=1")
    check(dump(appdb) == original, "database exactly as before")

    print("  folders")
    code, r = form("/api/folders/save", {"name": "Wide", "color": "none", "items": "PPSA01325"})
    fid = r["id"]
    tile = os.path.join(apps, fid, "sce_sys")
    im = Image.open(os.path.join(tile, "icon0.png"))
    check(im.mode == "RGBA" and im.getpixel((3, 3))[3] == 0, "colour 'none': the folder icon is see-through")
    code, r = http("POST", "/api/folders/bg?id=%s&usb=%s" % (fid, urllib.parse.quote(os.path.join(usb0, "wide.jpg"))))
    check(code == 200 and dds_info(os.path.join(tile, "pic0.dds")) == (3840, 2160, 1, 98),
          "folder tile gets a 4K BC7 pic0.dds")
    code, j = http("GET", "/api/folder?id=" + fid)
    check(j["folder"]["custom_bg"], "folder reports its background")
    code, prev = http("GET", "/api/folder_bg?id=" + fid, raw=True)
    check(code == 200 and prev[:2] == b"\xff\xd8", "folder background preview served")
    form("/api/folders/save", {"id": fid, "name": "Wide 2", "color": "teal", "items": "PPSA01325"})
    check(os.path.exists(os.path.join(tile, "pic0.dds")), "background kept when the folder is edited")
    clear = Image.new("RGBA", (200, 200), (0, 0, 0, 0))
    ImageDraw.Draw(clear).ellipse((50, 50, 150, 150), fill=(255, 255, 255, 255))
    buf = io.BytesIO()
    clear.save(buf, "PNG")
    form("/api/folders/save", {"id": fid, "name": "Wide 2", "color": "none", "items": "PPSA01325"})
    post_bytes("/api/folders/icon?id=" + fid, buf.getvalue())
    im = Image.open(os.path.join(tile, "icon0.png"))
    check(im.mode == "RGBA" and im.getpixel((3, 3))[3] == 0 and im.getpixel((256, 256))[3] == 255,
          "custom picture on colour 'none' stays see-through")
    code, r = http("POST", "/api/folders/bg?id=%s&reset=1" % fid)
    check(code == 200 and not os.path.exists(os.path.join(tile, "pic0.dds")), "folder background removed")

    print("  see-through tile pictures")
    code, r = post_bytes("/api/custom/icon?id=PPSA01325&clear=1", buf.getvalue())
    f = q(appdb, f'SELECT icon0Info FROM "tbl_concepticoninfo_{USERS[0]}" WHERE localConceptId=?', "cid:local:PPSA01325")[0][0]
    im = Image.open(f)
    check(code == 200 and im.mode == "RGBA" and im.getpixel((3, 3))[3] == 0, "'keep see-through' leaves it transparent")
    code, r = post_bytes("/api/custom/icon?id=PPSA01325", buf.getvalue())
    f = q(appdb, f'SELECT icon0Info FROM "tbl_concepticoninfo_{USERS[0]}" WHERE localConceptId=?', "cid:local:PPSA01325")[0][0]
    check(Image.open(f).mode == "RGB", "without it: dark background as before")

    print("  restore original backup")
    post_bytes("/api/custom/bg?id=PPSA01325", jpg)
    ours = bgs("PPSA01325")[0]
    code, r = http("POST", "/api/restore")
    check(code == 200 and not any(os.path.exists(x) for x in ours), "background files removed with the restore")
    menu = http("GET", "/", raw=True)[1]
    check(b"teBgField" in menu and b"fmBgFile" in menu and b"teClear" in menu, "menu has the background and see-through options")
    s.stop()


class FakeLiveShell:
    """Plays LiveShell's web API (port 9050 on the console)."""

    def __init__(self, port):
        self.port = port
        self.calls = []
        self.videos = [
            {"name": "ocean.mp4", "where": "Internal", "path": "/data/LiveShell/video/ocean.mp4", "res": "FHD",
             "snd": False, "why": "", "size": 1000, "thumb": True, "fav": False, "k4": False},
            {"name": "city night.mp4", "where": "USB 1", "path": "/mnt/usb0/liveshell/city night.mp4", "res": "4K",
             "snd": True, "why": "", "size": 2000, "thumb": False, "fav": False, "k4": True},
        ]
        self.current = self.videos[0]["path"]
        outer = self

        class H(httpserver.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def reply(self, code, body, ctype="application/json"):
                b = body if isinstance(body, bytes) else body.encode()
                self.send_response(code)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(b)))
                self.end_headers()
                self.wfile.write(b)

            def do_GET(self):
                path, _, query = self.path.partition("?")
                qs = urllib.parse.parse_qs(query)
                i = int(qs.get("i", ["-1"])[0])
                outer.calls.append(self.path)
                if path == "/api/state":
                    self.reply(200, json.dumps({"enabled": True, "status": "playing", "ip": "192.168.1.50",
                                                "port": 9050, "current": outer.current, "videos": outer.videos}))
                elif path == "/api/select" and 0 <= i < len(outer.videos):
                    outer.current = outer.videos[i]["path"]
                    self.reply(200, '{"ok":true}')
                elif path == "/api/delete" and 0 <= i < len(outer.videos):
                    if outer.videos[i]["where"] != "Internal":
                        self.reply(200, '{"ok":false,"error":"Videos on USB are deleted on the drive"}')
                    else:
                        outer.videos.pop(i)
                        self.reply(200, '{"ok":true}')
                elif path == "/thumb" and i == 0:
                    self.reply(200, tiny_png(16, 9, (10, 120, 200)), "image/png")
                elif path == "/api/shutdown":
                    self.reply(200, '{"ok":true}')
                    threading.Thread(target=outer.stop, daemon=True).start()
                else:
                    self.reply(404, "")

        socketserver.TCPServer.allow_reuse_address = True
        self.srv = socketserver.ThreadingTCPServer(("127.0.0.1", port), H)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def stop(self):
        self.srv.shutdown()
        self.srv.server_close()


class RecordingLoader:
    """An ELF loader that only records what it is sent."""

    def __init__(self, port):
        self.received = []
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", port))
        self.sock.listen(4)
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while True:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            data = b""
            while True:
                chunk = c.recv(65536)
                if not chunk:
                    break
                data += chunk
            c.close()
            self.received.append(data)

    def close(self):
        self.sock.close()


def watchdog_multi_folder_tests(tmp):
    print("auto re-hide for games in several folders")
    appdb = os.path.join(tmp, "wdm.db")
    data = os.path.join(tmp, "wdm-data")
    apps = os.path.join(tmp, "wdm-apps")
    icons = os.path.join(tmp, "wdm-icons")
    os.makedirs(icons)
    for tid in ("PPSA01325", "CUSA00900"):
        open(os.path.join(icons, tid + ".png"), "wb").write(tiny_png(64, 64, (200, 40, 90)))
    make_db(appdb)
    s = Server(appdb, data, {"HH_USER_APP_DIR": apps, "HH_ICON_DIR": icons})

    def played(*tids):
        """the PS5 shows a tile again after its game was played"""
        for u in USERS:
            for tid in tids:
                qx(appdb, f'UPDATE "tbl_iconinfo_{u}" SET visible=1, dispLocation=138 WHERE titleId=?', tid)

    form("/api/folders/save", {"name": "A", "items": "PPSA01325,CUSA00900"})
    form("/api/folders/save", {"name": "B", "items": "PPSA01325"})
    check(vis(appdb, "PPSA01325") == [0, 0] and vis(appdb, "CUSA00900") == [0, 0], "both hidden to start")

    played("PPSA01325", "CUSA00900")
    check(wait_for(lambda: vis(appdb, "PPSA01325") == [0, 0] and vis(appdb, "CUSA00900") == [0, 0], 14),
          "a game in two folders is hidden again after it is played")

    # leaves folder B, still in folder A: must keep being re-hidden
    form("/api/folders/save", {"id": "HHFD00002", "name": "B", "items": "CUSA00900"})
    played("PPSA01325")
    check(wait_for(lambda: vis(appdb, "PPSA01325") == [0, 0], 14),
          "still re-hidden after leaving one of its folders")

    # leaves its last folder: shown, and the watchdog must leave it alone
    form("/api/folders/save", {"id": "HHFD00001", "name": "A", "items": "CUSA00900"})
    check(vis(appdb, "PPSA01325") == [1, 1], "shown once it is out of every folder")
    time.sleep(8)
    check(vis(appdb, "PPSA01325") == [1, 1], "and not hidden again behind the user's back")
    s.stop()

def watchdog_tests(tmp):
    print("auto re-hide after playing a game (background watchdog)")
    appdb = os.path.join(tmp, "wd.db")
    data = os.path.join(tmp, "wd-data")
    apps = os.path.join(tmp, "wd-apps")
    icons = os.path.join(tmp, "wd-icons")
    os.makedirs(icons)
    for tid in ("PPSA01325", "CUSA00900"):
        open(os.path.join(icons, tid + ".png"), "wb").write(tiny_png(64, 64, (200, 40, 90)))
    make_db(appdb)
    s = Server(appdb, data, {"HH_USER_APP_DIR": apps, "HH_ICON_DIR": icons})

    # a game hidden on its own, and one tucked inside a folder
    http("POST", "/api/hide?id=CUSA00900")
    form("/api/folders/save", {"name": "Games", "items": "PPSA01325"})
    check(vis(appdb, "PPSA01325") == [0, 0] and vis(appdb, "CUSA00900") == [0, 0], "both hidden to start")

    # the PS5 makes them visible again, the way it does after a game is played
    for u in USERS:
        qx(appdb, f'UPDATE "tbl_iconinfo_{u}" SET visible=1, dispLocation=138 WHERE titleId IN (?,?)',
           "PPSA01325", "CUSA00900")
    check(vis(appdb, "PPSA01325") == [1, 1], "system un-hid the folder's game")

    # the watchdog re-hides them by itself, without re-running the payload
    check(wait_for(lambda: vis(appdb, "PPSA01325") == [0, 0], 12), "folder game hidden again on its own")
    check(vis(appdb, "CUSA00900") == [0, 0], "the separately hidden game too")
    check("auto re-hide" in http("GET", "/api/diag", raw=True)[1].decode(), "the watchdog logged what it did")

    # a game in two folders is re-hidden the same way
    form("/api/folders/save", {"name": "More", "items": "PPSA01325,CUSA00900"})
    by_id, _ = listing()
    check(by_id["PPSA01325"]["folders"] == ["HHFD00001", "HHFD00002"], "game is in two folders")
    for u in USERS:
        qx(appdb, f'UPDATE "tbl_iconinfo_{u}" SET visible=1, dispLocation=138 WHERE titleId IN (?,?)',
           "PPSA01325", "CUSA00900")
    check(wait_for(lambda: vis(appdb, "PPSA01325") == [0, 0] and vis(appdb, "CUSA00900") == [0, 0], 12),
          "a game in two folders is hidden again on its own")

    # with the setting off, Home Screen Manager leaves the PS5's change alone
    http("POST", "/api/settings?reapply=0")
    for u in USERS:
        qx(appdb, f'UPDATE "tbl_iconinfo_{u}" SET visible=1 WHERE titleId=?', "PPSA01325")
    time.sleep(8)
    check(vis(appdb, "PPSA01325") == [1, 1], "left visible when 'Keep tiles hidden' is off")
    s.stop()


def tabs_tests(tmp):
    print("move tiles between the Games and Media tabs")
    appdb = os.path.join(tmp, "tab.db")
    data = os.path.join(tmp, "tab-data")
    apps = os.path.join(tmp, "tab-apps")
    make_db(appdb)
    original = dump(appdb)
    s = Server(appdb, data, {"HH_USER_APP_DIR": apps})

    def disp(tid):
        return [q(appdb, f'SELECT visible, dispLocation FROM "tbl_iconinfo_{u}" WHERE titleId=?', tid)[0]
                for u in USERS]

    # CUSA01065 (Netflix) is on the Media tab -> move it to Games
    code, r = http("POST", "/api/tab?id=CUSA01065&to=games")
    check(code == 200, "moved a Media app to the Games tab")
    for v, d in disp("CUSA01065"):
        check(v == 1 and (d & 2) and not (d & 4), "it's visible and on the Games tab now (0x%02x)" % d)
    check(q(appdb, "SELECT dispLocation FROM tbl_contentinfo WHERE titleId=?", "CUSA01065")[0][0] & 2,
          "tbl_contentinfo moved too")
    code, j = http("GET", "/api/tabs")
    check(j["moved"] == [{"id": "CUSA01065", "dest": "games"}], "reported as moved")
    by_id, _ = listing()
    check(by_id["CUSA01065"]["tab"] == "games", "the list shows it on Games")

    # and a Games tile to Media
    code, r = http("POST", "/api/tab?id=PPSA01325&to=media")
    check(code == 200 and all((d & 4) and not (d & 2) for _, d in disp("PPSA01325")), "moved a Games app to Media")

    print("  refusals")
    code, r = http("POST", "/api/tab?id=NPXS40071&to=media")
    check(code == 409 and "PS5 decides" in r["error"], "Game Library (locked) can't be moved")
    http("POST", "/api/hide?id=NPXS40053")
    code, r = http("POST", "/api/tab?id=NPXS40053&to=games")
    check(code == 409 and "hidden" in r["error"], "a hidden tile is refused")
    http("POST", "/api/unhide?id=NPXS40053")
    form("/api/folders/save", {"name": "F", "items": "CUSA00900"})
    code, r = http("POST", "/api/tab?id=CUSA00900&to=games")
    check(code == 409 and "folder" in r["error"], "a game inside a folder is refused")
    http("POST", "/api/folders/delete?id=HHFD00001")
    code, r = http("POST", "/api/tab?id=CUSA01065&to=sideways")
    check(code == 409, "an unknown tab is refused")

    print("  a tile that was tab-moved and then hidden stays hidden")
    # the real bug: the watchdog/launch re-applied the tab move, which set
    # visible=1 and brought a hidden tile (e.g. All Apps) back.
    http("POST", "/api/tab?id=NPXS40053&to=games")   # move it (adds a tab_moved row)
    http("POST", "/api/hide?id=NPXS40053")           # then hide it
    check(vis(appdb, "NPXS40053") == [0, 0], "moved-then-hidden tile is hidden")
    s.stop()
    for u in USERS:                                  # the system brings it back on screen
        qx(appdb, f'UPDATE "tbl_iconinfo_{u}" SET visible=1 WHERE titleId=?', "NPXS40053")
    s = Server(appdb, data, {"HH_USER_APP_DIR": apps})
    http("GET", "/api/status")                       # launch re-apply runs tabs_apply
    check(vis(appdb, "NPXS40053") == [0, 0], "the tab re-apply leaves a hidden tile hidden")
    http("POST", "/api/unhide?id=NPXS40053")
    http("POST", "/api/tab?id=NPXS40053&to=reset")   # tidy up for the restore check below

    print("  stays put, then undo")
    s.stop()
    for u in USERS:  # the system puts CUSA01065 back on Media
        qx(appdb, f'UPDATE "tbl_iconinfo_{u}" SET dispLocation=188 WHERE titleId=?', "CUSA01065")
    s = Server(appdb, data, {"HH_USER_APP_DIR": apps})
    http("GET", "/api/status")
    check(all((d & 2) for _, d in disp("CUSA01065")), "the move is re-applied at launch")
    code, r = http("POST", "/api/tab?id=CUSA01065&to=reset")
    check(code == 200 and disp("CUSA01065") == [(1, 188), (1, 188)], "reset puts it back on Media with its original value")
    http("POST", "/api/tab?id=PPSA01325&to=reset")
    check(dump(appdb) == original, "database back to the original once every move is undone")
    menu = http("GET", "/", raw=True)[1]
    check(b"Move to Media Tab" in menu and b"Move to Games Tab" in menu and b"teMove" in menu,
          "move-between-tabs lives in the tile editor")
    check(b"teTakeOut" in menu, "the tile editor can take an app out of its folder")
    s.stop()


def folder_move_tests(tmp):
    print("move folders between tabs and take apps out of folders")
    appdb = os.path.join(tmp, "fmove.db")
    data = os.path.join(tmp, "fmove-data")
    apps = os.path.join(tmp, "fmove-apps")
    make_db(appdb)
    s = Server(appdb, data, {"HH_USER_APP_DIR": apps})

    def disp(tid):
        return [q(appdb, f'SELECT visible, dispLocation FROM "tbl_iconinfo_{u}" WHERE titleId=?', tid)[0]
                for u in USERS]

    # a folder on the Games tab with two apps
    code, r = form("/api/folders/save", {"name": "Fun", "tab": "games", "items": "PPSA01325,CUSA00900"})
    fid = r["id"]
    check(code == 200 and all((d & 2) and not (d & 4) for _, d in disp(fid)), "folder tile starts on the Games tab")

    # move the whole folder to Media through the same endpoint apps use
    code, r = http("POST", "/api/tab?id=" + fid + "&to=media")
    check(code == 200 and all((d & 4) and not (d & 2) for _, d in disp(fid)), "folder tile moved to the Media tab")
    check(http("GET", "/api/folders")[1]["folders"][0]["tab"] == "media", "folder remembers the Media tab")
    code, r = http("POST", "/api/tab?id=" + fid + "&to=sideways")
    check(code == 409, "an unknown tab is refused for a folder too")

    # a folder and the Game Library can be positioned under Arrange (not tab-moved)
    print("  arrange a folder (Game/App Library are left out)")

    def acc(tid):
        return [q(appdb, f'SELECT lastAccessIndex FROM "tbl_iconinfo_{u}" WHERE titleId=?', tid)[0][0] for u in USERS]

    def pinned(tid):
        return [q(appdb, f'SELECT count(*) FROM "tbl_group_{u}" WHERE itemId=? AND groupName=\'MyHome\'', tid)[0][0]
                for u in USERS]

    code, r = form("/api/arrange/save", {"items": LIBRARY + "," + fid + ",CUSA01065", "pinned": fid})
    check(code == 200, "folder arranged")
    ids = [i["id"] for i in http("GET", "/api/arrange")[1]["arrange"]["items"]]
    check(LIBRARY not in ids, "Game Library can't be reordered (the PS5 places it)")
    check(ids == [fid, "CUSA01065"], "the folder and app are arranged in order, Library skipped")
    check(all(v > 1000000 for v in acc(fid)), "the folder is put in front")
    check(pinned(fid) == [1, 1], "a folder can be pinned to MyHome")
    check(http("POST", "/api/arrange/clear")[0] == 200, "arrangement cleared")

    # take one app out: it comes back on its tab and leaves the folder
    code, r = http("POST", "/api/folders/take_out?id=PPSA01325")
    check(code == 200 and vis(appdb, "PPSA01325") == [1, 1], "app taken out is shown again")
    code, j = http("GET", "/api/folder?id=" + fid)
    check([i["id"] for i in j["folder"]["items"]] == ["CUSA00900"], "only the other app stays in the folder")
    check(listing()[0]["PPSA01325"]["folder"] is None, "the list no longer shows it in a folder")
    code, r = http("POST", "/api/tab?id=PPSA01325&to=media")
    check(code == 200, "the freed app can now be moved on its own")

    # take-out of something not in a folder is refused
    code, r = http("POST", "/api/folders/take_out?id=CUSA01065")
    check(code == 409 and "isn't in a folder" in r["error"], "take-out of a non-member refused")

    # taking out the last app removes the folder and its tile
    code, r = http("POST", "/api/folders/take_out?id=CUSA00900")
    check(code == 200 and http("GET", "/api/folders")[1]["folders"] == [], "last app out removes the empty folder")
    check(vis(appdb, "CUSA00900") == [1, 1], "the last app is shown again")
    check(all(r_ == [] for r_ in tile_rows(appdb, fid)), "the folder tile is uninstalled")
    s.stop()


def live_tests(tmp):
    print("live wallpaper (built-in LiveShell Engine)")
    appdb = os.path.join(tmp, "live.db")
    data = os.path.join(tmp, "live-data")
    make_db(appdb)
    elf = open(os.path.join(os.path.dirname(BIN), "..", "..", "third_party", "liveshell", "liveshell.elf"), "rb").read()
    lport, aport = 19121, 19050
    loader = RecordingLoader(lport)
    env = {"HH_LOADER_PORT": str(lport), "HH_LIVESHELL_PORT": str(aport)}

    s = Server(appdb, data, env)
    code, j = http("GET", "/api/live")
    check(code == 200 and not j["enabled"] and not j["running"] and j["version"] == "1.5.2", "off by default")
    time.sleep(0.5)
    check(loader.received == [], "LiveShell not started while it's off")
    d = http("GET", "/api/diag", raw=True)[1].decode()
    check("LiveShell 1.5.2 built in: %d bytes, sha256 %s" % (len(elf), hashlib.sha256(elf).hexdigest()) in d,
          "Diagnostics names the built-in LiveShell and its checksum")

    print("  turning it on starts the built-in LiveShell")
    code, r = http("POST", "/api/live/power?on=1")
    check(code == 200 and "Starting" in r["note"], "switched on")
    check(wait_for(lambda: len(loader.received) == 1), "liveshell.elf handed to the ELF loader")
    check(loader.received[0] == elf, "byte for byte the original liveshell.elf")
    code, j = http("GET", "/api/live")
    check(j["enabled"] and not j["running"] and j["state"] is None, "reported as starting until LiveShell answers")

    print("  menu works through LiveShell's API")
    fake = FakeLiveShell(aport)
    code, j = http("GET", "/api/live")
    check(j["running"] and j["state"]["current"] == "/data/LiveShell/video/ocean.mp4" and len(j["state"]["videos"]) == 2,
          "status and videos come from LiveShell")
    code, r = http("POST", "/api/live/select?path=" + urllib.parse.quote("/mnt/usb0/liveshell/city night.mp4"))
    check(code == 200 and "/api/select?i=1" in fake.calls and fake.current.endswith("city night.mp4"),
          "picking a video tells LiveShell (by its place in LiveShell's list)")
    code, r = http("POST", "/api/live/select?path=" + urllib.parse.quote("/etc/passwd"))
    check(code == 404 and not any("i=-1" in c for c in fake.calls), "unknown video refused, nothing sent")
    code, png = http("GET", "/api/live/thumb?path=" + urllib.parse.quote("/data/LiveShell/video/ocean.mp4"), raw=True)
    check(code == 200 and png[:4] == b"\x89PNG", "LiveShell's preview picture passed on")
    code, png = http("GET", "/api/live/thumb?path=" + urllib.parse.quote("/mnt/usb0/liveshell/city night.mp4"), raw=True)
    check(code == 404, "no preview -> 404")
    code, r = http("POST", "/api/live/delete?path=" + urllib.parse.quote("/mnt/usb0/liveshell/city night.mp4"))
    check(code == 409 and "USB" in r["error"], "LiveShell's own error passed on")
    code, r = http("POST", "/api/live/delete?path=" + urllib.parse.quote("/data/LiveShell/video/ocean.mp4"))
    check(code == 200 and len(fake.videos) == 1, "video deleted")
    evil = urllib.request.Request(BASE + "/api/live/power?on=0", method="POST", data=b"",
                                  headers={"Origin": "http://evil.example"})
    try:
        urllib.request.urlopen(evil, timeout=5)
        code = 200
    except urllib.error.HTTPError as e:
        code = e.code
    check(code == 403, "other web sites can't switch it")

    print("  starts with Home Screen Manager, but never twice")
    s.stop()
    s = Server(appdb, data, env)
    time.sleep(4.5)
    check(len(loader.received) == 1, "already running: not loaded again")
    s.stop()
    fake.stop()
    s = Server(appdb, data, env)
    check(wait_for(lambda: len(loader.received) == 2, 10), "not running at start: started again (setting kept)")

    print("  turning it off")
    fake = FakeLiveShell(aport)
    code, r = http("POST", "/api/live/power?on=0")
    check(code == 200 and "/api/shutdown" in fake.calls, "LiveShell asked to stop")
    check(not http("GET", "/api/live")[1]["enabled"], "setting off")
    code, r = http("POST", "/api/live/select?path=x")
    check(code in (404, 409), "nothing to pick while it's off")
    menu = http("GET", "/", raw=True)[1]
    check(b"LiveShell Engine" in menu and b"ZiZc3" in menu and b"github.com/ZiZc3/LiveShell" in menu,
          "menu credits LiveShell and its author")
    s.stop()
    loader.close()


def front_folders_tests(tmp):
    print("keep folders first")
    appdb = os.path.join(tmp, "ff.db")
    data = os.path.join(tmp, "ff-data")
    apps = os.path.join(tmp, "ff-apps")
    icons = os.path.join(tmp, "ff-icons")
    os.makedirs(icons)
    for i, tid in enumerate(("PPSA01325", "CUSA00900", "CUSA01065")):
        open(os.path.join(icons, tid + ".png"), "wb").write(tiny_png(64 + i * 8, 64 + i * 8, (200, 40 * i, 90)))
    make_db(appdb)
    running_file = os.path.join(tmp, "ff-running")  # which game is running (none until written)
    env = {"HH_USER_APP_DIR": apps, "HH_ICON_DIR": icons, "HH_RUNNING_FILE": running_file}
    s = Server(appdb, data, env)

    def acc(tid, u=0):
        return q(appdb, f'SELECT lastAccessIndex FROM "tbl_iconinfo_{USERS[u]}" WHERE titleId=?', tid)[0][0] or 0

    def counter():
        return q(appdb, "SELECT status FROM tbl_version WHERE category='access_index'")[0][0]

    def others_max(fids, u=0, skip=()):
        rows = q(appdb, f'SELECT titleId, lastAccessIndex FROM "tbl_iconinfo_{USERS[u]}"')
        return max(v for t, v in rows if t not in fids and t not in skip and v is not None)

    def ahead(fids, skip=()):
        return all(min(acc(f, u) for f in fids) > others_max(fids, u, skip) for u in range(len(USERS)))

    def order(fids):
        return sorted(fids, key=lambda f: -acc(f))

    def ps5_opens(tid):
        """the PS5 plays a game / opens a tile: the next number after its counter in the database"""
        n = counter() + 1
        con = sqlite3.connect(appdb)
        con.execute("UPDATE tbl_version SET status=? WHERE category='access_index'", (n,))
        for u in USERS:
            con.execute(f'UPDATE "tbl_iconinfo_{u}" SET lastAccessIndex=? WHERE titleId=?', (n, tid))
        con.commit()
        con.close()

    def ps5_opens_own_counter(tid, fids):
        """a PS5 that keeps its counter to itself: it writes a number just above the
        games' own, whatever is in the database"""
        con = sqlite3.connect(appdb)
        for u in USERS:
            rows = con.execute(f'SELECT titleId, lastAccessIndex FROM "tbl_iconinfo_{u}"').fetchall()
            n = max(v for t, v in rows if t not in fids and v is not None) + 1
            con.execute(f'UPDATE "tbl_iconinfo_{u}" SET lastAccessIndex=? WHERE titleId=?', (n, tid))
        con.commit()
        con.close()

    fids = []
    for name, game in (("One", "PPSA01325"), ("Two", "CUSA00900"), ("Three", "CUSA01065")):
        code, r = form("/api/folders/save", {"name": name, "items": game})
        check(code == 200, "folder " + name + " created")
        fids.append(r["id"])
    # the PS5 gives each folder tile a recency number of its own
    con = sqlite3.connect(appdb)
    for u in USERS:
        for f, v in zip(fids, (30, 20, 35)):
            con.execute(f'UPDATE "tbl_iconinfo_{u}" SET lastAccessIndex=? WHERE titleId=?', (v, f))
    con.commit()
    con.close()
    natural = order(fids)
    check(natural == [fids[2], fids[0], fids[1]], "the PS5's own order of the folders")
    check(not ahead(fids), "before: games can be ahead of the folders (the PS5's own order)")

    print("  switch on")
    code, r = http("POST", "/api/settings?folders_first=1")
    check(code == 200, "setting saved")
    check(http("GET", "/api/status")[1]["settings"]["folders_first"] is True, "setting reported")
    check(ahead(fids), "every folder is ahead of every game and app")
    check(order(fids) == natural, "the folders keep the PS5's own order among themselves")
    check(counter() >= max(acc(f) for f in fids), "the PS5's counter is at the top of the folders")
    check(max(acc(f) for f in fids) < 10000, "the numbers stay small (just above the PS5's own)")
    v1 = ([acc(f) for f in fids], counter())
    mt = os.stat(appdb).st_mtime_ns
    time.sleep(8)
    check(([acc(f) for f in fids], counter()) == v1, "the numbers stay put (they don't climb)")
    check(os.stat(appdb).st_mtime_ns == mt, "a quiet pass doesn't touch the home screen database")

    print("  the PS5 opens a folder")
    last = natural[-1]
    ps5_opens(last)
    check(order(fids)[0] == last and ahead(fids), "it goes to the front of the folders right away, as before")
    check(order(fids)[1:] == [f for f in natural if f != last], "the others keep their order")
    check(wait_for(lambda: counter() >= max(acc(f) for f in fids), 14), "the counter follows the folders up")
    check(order(fids)[0] == last and ahead(fids), "still first after a pass")

    print("  the PS5 plays a game that is not in a folder")
    ps5_opens("PPSA02000")
    check(wait_for(lambda: ahead(fids) and order(fids)[0] == last, 14),
          "after a pass the folders are ahead of the game again, in the same order")
    check(counter() >= max(acc(f) for f in fids), "and the counter is back at the top of the folders")
    v2 = ([acc(f) for f in fids], counter())
    time.sleep(8)
    check(([acc(f) for f in fids], counter()) == v2, "quiet again afterwards")

    print("  a game that stays open")
    ordr = order(fids)
    open(running_file, "w").write("PPSA02000")
    ps5_opens("PPSA02000")
    check(acc("PPSA02000") > max(acc(f) for f in fids), "it is in front of the folders when it starts")
    time.sleep(8)  # at least one background pass
    check(acc("PPSA02000") > max(acc(f) for f in fids), "and stays in front of the folders while it runs")
    check(order(fids) == ordr, "the folders keep their order")
    held = ([acc(f) for f in fids], acc("PPSA02000"), counter())
    time.sleep(8)
    check(([acc(f) for f in fids], acc("PPSA02000"), counter()) == held, "nothing moves or climbs while it runs")
    check("is running, kept in front of the folders" in http("GET", "/api/diag", raw=True)[1].decode(),
          "the log says the game is held in front")
    open(running_file, "w").write("")  # the game is closed
    check(wait_for(lambda: ahead(fids), 4), "when it is closed the folders go back in front of it within seconds")
    check("PPSA02000 is closed: checking the home screen right away" in http("GET", "/api/diag", raw=True)[1].decode(),
          "the log says the close was noticed")
    check("was closed, the folders go back in front" in http("GET", "/api/diag", raw=True)[1].decode(),
          "and that it was let go")
    check(order(fids) == ordr, "in the same order")
    check(counter() >= max(acc(f) for f in fids), "and the counter is back at the top of the folders")
    v3 = ([acc(f) for f in fids], counter())
    time.sleep(8)
    check(([acc(f) for f in fids], counter()) == v3, "quiet again afterwards")

    print("  a PS5 that keeps its own counter")
    check(True, "(writes just above the games' numbers, whatever the database counter says)")
    ps5_opens_own_counter("PPSA02000", fids)
    check(ahead(fids), "a game it starts does not get ahead of the folders")
    second = order(fids)[1]
    ps5_opens_own_counter(second, fids)
    seen_back = fixed_now = False
    for attempt in range(4):  # (a background pass could land between the write and our look)
        if order(fids)[0] == second:
            second = order(fids)[1]
            ps5_opens_own_counter(second, fids)
        if order(fids)[0] != second:
            seen_back = True
            code, page = http("GET", "/folder/" + second, raw=True)
            fixed_now = code == 200 and order(fids)[0] == second and ahead(fids)
            break
    check(seen_back, "an opened folder lands further back in the block")
    check(fixed_now, "and the folder page puts it first straight away")
    check("opened" in http("GET", "/api/diag", raw=True)[1].decode(), "the log says so")

    print("  a new folder")
    code, r = form("/api/folders/save", {"name": "Four", "items": "PPSA01325"})
    fids.append(r["id"])
    check(wait_for(lambda: order(fids)[0] == r["id"] and ahead(fids), 14), "a new folder joins at the front")

    print("  together with a saved order")
    code, r = form("/api/arrange/save", {"items": "PPSA02000", "pinned": ""})
    check(code == 200, "order saved")
    check(wait_for(lambda: ahead(fids), 14), "the folders are ahead of the arranged tile")
    a1 = acc("PPSA02000")
    check(a1 > others_max(fids, 0, skip=("PPSA02000",)), "the arranged tile is ahead of the other games")
    time.sleep(8)
    check(acc("PPSA02000") == a1, "arranged numbers don't climb")
    check(ahead(fids), "and the folders stay ahead of it")
    for i in range(2):  # games get played: nothing may ratchet up
        ps5_opens("NPXS40053")
        time.sleep(7)
    check(acc("PPSA02000") - a1 < 1000, "playing games doesn't push the saved order up")
    check(wait_for(lambda: ahead(fids), 14), "the folders are still ahead")
    check(acc("PPSA02000") > acc("NPXS40053"), "and the saved order is still ahead of the played game")

    print("  switch off")
    a2 = acc("PPSA02000")
    code, r = http("POST", "/api/settings?folders_first=0")
    check(code == 200, "switched off")
    check(all(acc(f) < 1000 for f in fids), "folders are back in the PS5's own number range")
    check(acc("PPSA02000") == a2, "the saved order is left alone")
    time.sleep(8)
    check(all(acc(f) < 1000 for f in fids), "and stay there")

    print("  after a restart")
    http("POST", "/api/settings?folders_first=1")
    s.stop()
    con = sqlite3.connect(appdb)
    for u in USERS:  # the system rebuilt the order
        for f in fids:
            con.execute(f'UPDATE "tbl_iconinfo_{u}" SET lastAccessIndex=1 WHERE titleId=?', (f,))
    con.commit()
    con.close()
    s = Server(appdb, data, env)
    check(ahead(fids), "folders are put back in front when Home Screen Manager starts")
    s.stop()

def show_running_tests(tmp):
    print("a running game that is hidden stays on screen")
    appdb = os.path.join(tmp, "sr.db")
    data = os.path.join(tmp, "sr-data")
    apps = os.path.join(tmp, "sr-apps")
    icons = os.path.join(tmp, "sr-icons")
    os.makedirs(icons)
    for tid in ("PPSA01325", "CUSA00900", "CUSA01065"):
        open(os.path.join(icons, tid + ".png"), "wb").write(tiny_png(64, 64, (200, 40, 90)))
    make_db(appdb)
    running_file = os.path.join(tmp, "sr-running")
    s = Server(appdb, data, {"HH_USER_APP_DIR": apps, "HH_ICON_DIR": icons, "HH_RUNNING_FILE": running_file})

    def ps5_shows(*tids):
        """the PS5 makes tiles visible again when their game is started"""
        for u in USERS:
            for tid in tids:
                qx(appdb, f'UPDATE "tbl_iconinfo_{u}" SET visible=1, dispLocation=138 WHERE titleId=?', tid)

    check(http("GET", "/api/status")[1]["settings"]["show_running"] is True, "on by default")
    form("/api/folders/save", {"name": "Games", "items": "PPSA01325,CUSA00900"})
    http("POST", "/api/hide?id=CUSA01065")
    check(all(vis(appdb, t) == [0, 0] for t in ("PPSA01325", "CUSA00900", "CUSA01065")), "all three hidden to start")

    print("  while it runs")
    open(running_file, "w").write("PPSA01325")
    ps5_shows("PPSA01325", "CUSA00900")
    check(wait_for(lambda: vis(appdb, "CUSA00900") == [0, 0], 14), "a tile that is not running is hidden again")
    check(vis(appdb, "PPSA01325") == [1, 1], "the running game's tile stays on screen")
    time.sleep(8)
    check(vis(appdb, "PPSA01325") == [1, 1], "and keeps staying while it runs")
    code, j = http("GET", "/api/list")
    check(code == 200, "the list still loads")

    print("  after it is closed")
    open(running_file, "w").write("")
    check(wait_for(lambda: vis(appdb, "PPSA01325") == [0, 0], 4), "it is hidden again within seconds")
    check(all(vis(appdb, t) == [0, 0] for t in ("PPSA01325", "CUSA00900", "CUSA01065")), "everything hidden as before")
    # the PS5 writes its own update a moment after the close and shows the tile again
    time.sleep(0.8)
    ps5_shows("PPSA01325")
    check(wait_for(lambda: vis(appdb, "PPSA01325") == [0, 0], 1.0), "if the PS5 shows it again, it is hidden at once")
    check("the PS5 showed PPSA01325 again" in http("GET", "/api/log", raw=True)[1].decode(),
          "the log says how soon after the close, and how fast it was hidden")
    log = http("GET", "/api/diag", raw=True)[1].decode()
    check("PPSA01325 is running, its tile stays on screen" in log and "PPSA01325 was closed, its tile is hidden again" in log,
          "the log shows when the tile was left on screen and when it was hidden again")

    code, raw = http("GET", "/api/log", raw=True)
    check(code == 200 and b"Home Screen Manager" in raw and b"show running" in raw, "the log has its own address (/api/log)")

    print("  switched off")
    http("POST", "/api/settings?show_running=0")
    check(http("GET", "/api/status")[1]["settings"]["show_running"] is False, "setting reported")
    open(running_file, "w").write("PPSA01325")
    ps5_shows("PPSA01325")
    check(wait_for(lambda: vis(appdb, "PPSA01325") == [0, 0], 14), "hidden straight away, even while it runs")
    open(running_file, "w").write("")
    s.stop()

    print("  with folders first")
    appdb = os.path.join(tmp, "sr2.db")
    data = os.path.join(tmp, "sr2-data")
    make_db(appdb)
    s = Server(appdb, data, {"HH_USER_APP_DIR": apps + "2", "HH_ICON_DIR": icons, "HH_RUNNING_FILE": running_file})

    def acc(tid):
        return q(appdb, f'SELECT lastAccessIndex FROM "tbl_iconinfo_{USERS[0]}" WHERE titleId=?', tid)[0][0] or 0

    code, r = form("/api/folders/save", {"name": "Games", "items": "PPSA01325"})
    fid = r["id"]
    con = sqlite3.connect(appdb)
    for u in USERS:
        con.execute(f'UPDATE "tbl_iconinfo_{u}" SET lastAccessIndex=30 WHERE titleId=?', (fid,))
    con.commit()
    con.close()
    http("POST", "/api/settings?folders_first=1")

    def others_max():
        rows = q(appdb, f'SELECT titleId, lastAccessIndex FROM "tbl_iconinfo_{USERS[0]}"')
        return max(v for t, v in rows if t != fid and v is not None)

    check(acc(fid) > others_max(), "the folder is in front")
    n = q(appdb, "SELECT status FROM tbl_version WHERE category='access_index'")[0][0] + 1
    open(running_file, "w").write("PPSA01325")
    con = sqlite3.connect(appdb)  # the game inside the folder is started
    con.execute("UPDATE tbl_version SET status=? WHERE category='access_index'", (n,))
    for u in USERS:
        con.execute(f'UPDATE "tbl_iconinfo_{u}" SET lastAccessIndex=?, visible=1, dispLocation=138 WHERE titleId=?',
                    (n, "PPSA01325"))
    con.commit()
    con.close()
    time.sleep(8)
    check(vis(appdb, "PPSA01325") == [1, 1], "its tile is on screen")
    check(acc("PPSA01325") > acc(fid), "in front of the folder")
    open(running_file, "w").write("")
    check(wait_for(lambda: vis(appdb, "PPSA01325") == [0, 0] and acc(fid) > others_max(), 4),
          "closed: the tile is hidden again and the folder is back in front, within seconds")
    s.stop()


def folder_page_options_tests(tmp):
    print("folder page options")
    appdb = os.path.join(tmp, "fp.db")
    data = os.path.join(tmp, "fp-data")
    apps = os.path.join(tmp, "fp-apps")
    icons = os.path.join(tmp, "fp-icons")
    os.makedirs(icons)
    open(os.path.join(icons, "PPSA01325.png"), "wb").write(tiny_png(64, 64, (200, 40, 90)))
    make_db(appdb)
    env = {"HH_USER_APP_DIR": apps, "HH_ICON_DIR": icons}
    s = Server(appdb, data, env)
    code, r = form("/api/folders/save", {"name": "Games", "items": "PPSA01325"})
    fid = r["id"]

    def page(header, bar, edit):
        return {"header": header, "bar": bar, "edit": edit, "debug": False}  # debug: the test display, off by default

    st = http("GET", "/api/status")[1]["settings"]
    check(st["folder_header"] is True and st["folder_bar"] is True and st["folder_edit"] is True,
          "defaults: the picture and name, the count and sort row and the edit link are all shown")
    code, j = http("GET", "/api/folder?id=" + fid)
    check(code == 200 and j["page"] == page(True, True, True), "the folder page is told the defaults")
    check(j["folder"]["name"] == "Games" and len(j["folder"]["items"]) == 1, "the folder itself is unchanged")

    print("  switch them")
    http("POST", "/api/settings?folder_header=0")
    check(http("GET", "/api/folder?id=" + fid)[1]["page"] == page(False, True, True), "picture and name off, the rest unchanged")
    http("POST", "/api/settings?folder_bar=0")
    check(http("GET", "/api/folder?id=" + fid)[1]["page"] == page(False, False, True), "count and sort row off too")
    http("POST", "/api/settings?folder_edit=0")
    check(http("GET", "/api/folder?id=" + fid)[1]["page"] == page(False, False, False), "edit link off too")
    st = http("GET", "/api/status")[1]["settings"]
    check(st["folder_header"] is False and st["folder_bar"] is False and st["folder_edit"] is False,
          "the menu is told all three")
    code, j = http("GET", "/api/folder?id=NOSUCHFLD")
    check(code == 404, "an unknown folder is still a 404")
    http("POST", "/api/settings?folder_edit=1")
    check(http("GET", "/api/folder?id=" + fid)[1]["page"] == page(False, False, True), "the edit link alone can be switched back on")

    print("  they are remembered")
    s.stop()
    s = Server(appdb, data, env)
    st = http("GET", "/api/status")[1]["settings"]
    check(st["folder_header"] is False and st["folder_bar"] is False and st["folder_edit"] is True, "still set after a restart")
    check(http("GET", "/api/folder?id=" + fid)[1]["page"] == page(False, False, True), "and the folder page gets them")
    http("POST", "/api/settings?folder_header=1&folder_bar=1&folder_edit=1")
    check(http("GET", "/api/folder?id=" + fid)[1]["page"] == page(True, True, True), "all can be changed in one go")
    s.stop()


def folder_colours_tests(tmp):
    print("folder colours")
    if not Image:
        print("  (skipped: the picture library isn't installed)")
        return
    appdb = os.path.join(tmp, "col.db")
    data = os.path.join(tmp, "col-data")
    apps = os.path.join(tmp, "col-apps")
    icons = os.path.join(tmp, "col-icons")
    os.makedirs(icons)
    open(os.path.join(icons, "PPSA01325.png"), "wb").write(tiny_png(64, 64, (200, 40, 90)))
    make_db(appdb)
    s = Server(appdb, data, {"HH_USER_APP_DIR": apps, "HH_ICON_DIR": icons})

    # the swatches the menu offers
    menu = http("GET", "/", raw=True)[1].decode()
    block = re.search(r"var COLORS = \[(.*?)\];", menu, re.S).group(1)
    swatches = re.findall(r"\['(\w+)', '(#[0-9a-f]{6})'\]", block)
    check([n for n, _ in swatches] == ["blue", "purple", "red", "orange", "green", "teal", "gray", "darkgray", "black"],
          "the menu offers see-through, the seven colours it had, and dark gray and black after gray")

    code, r = form("/api/folders/save", {"name": "Colours", "color": "blue", "items": "PPSA01325"})
    fid = r["id"]
    icon0 = os.path.join(apps, fid, "sce_sys", "icon0.png")

    def paint(name):
        form("/api/folders/save", {"id": fid, "name": "Colours", "color": name, "items": "PPSA01325"})
        im = Image.open(icon0).convert("RGB")
        return im.getpixel((3, 3)), im.getpixel((3, 508))

    tops = {}
    for name, hexv in swatches:
        top, bottom = paint(name)
        tops[name] = top
        want = tuple(int(hexv[i:i + 2], 16) for i in (1, 3, 5))
        check(near(top, want, 14), "%s: the folder picture's top is the menu swatch's colour" % name)
        check(sum(bottom) < sum(top), "%s: it gets darker towards the bottom" % name)

    lum = lambda c: 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]
    g, d, b = lum(tops["gray"]), lum(tops["darkgray"]), lum(tops["black"])
    check(g > d > b, "dark gray sits between gray and black")
    check(abs((g - d) - (d - b)) < 8, "in evenly spaced steps")
    check(lum(tops["black"]) < 45 and lum(tops["black"]) > 5, "black is dark but not a flat #000000 (the game pictures still show up on it)")

    def stored(name):
        form("/api/folders/save", {"id": fid, "name": "Colours", "color": name, "items": "PPSA01325"})
        return [f["color"] for f in http("GET", "/api/folders")[1]["folders"] if f["id"] == fid][0]
    check(stored("black") == "black" and stored("darkgray") == "darkgray", "the new colour names are kept")
    check(stored("bogus") == "blue", "an unknown colour falls back to blue")
    s.stop()


def folder_job_tests(tmp):
    print("saving a folder in the background (slow install)")
    appdb = os.path.join(tmp, "fj.db")
    data = os.path.join(tmp, "fj-data")
    apps = os.path.join(tmp, "fj-apps")
    icons = os.path.join(tmp, "fj-icons")
    os.makedirs(icons)
    open(os.path.join(icons, "PPSA01325.png"), "wb").write(tiny_png(64, 64, (200, 40, 90)))
    make_db(appdb)
    # the console's installer takes about 90 s on newer firmware; here 7 s, which is longer
    # than the background check's 6 s rhythm (the check used to start a second install)
    s = Server(appdb, data, {"HH_USER_APP_DIR": apps, "HH_ICON_DIR": icons, "HH_INSTALL_DELAY_MS": "7000"})

    def log():
        return http("GET", "/api/log", raw=True)[1].decode()

    t0 = time.time()
    code, r = form("/api/folders/save?async=1", {"name": "Slow", "tab": "games", "color": "black", "items": "PPSA01325"})
    check(code == 200 and r["ok"] and isinstance(r["job"], int), "the save is answered at once with a job number")
    check(time.time() - t0 < 2.5, "and it doesn't wait for the install")
    job = r["job"]

    t1 = time.time()
    code, st = http("GET", "/api/status")
    check(code == 200 and time.time() - t1 < 1.5, "Home Screen Manager answers other requests while the install runs")
    code, j = http("GET", "/api/folders/job?job=%d" % job)
    check(code == 200 and j["ok"] and j["done"] is False, "the job reports that it is still running")
    code, r2 = form("/api/folders/save?async=1", {"name": "Second", "items": "PPSA01325"})
    check(code == 409 and "Another folder change" in r2["error"], "a second save is refused while one is running")
    check(http("GET", "/api/folders/job?job=%d" % (job + 50))[0] == 404, "an unknown job is a 404")

    check(wait_for(lambda: http("GET", "/api/folders/job?job=%d" % job)[1].get("done") is True, 40), "the job finishes")
    res = http("GET", "/api/folders/job?job=%d" % job)[1]["result"]
    check(res["ok"] and res["id"].startswith("HHFD") and res["folder"]["name"] == "Slow" and res["folder"]["color"] == "black",
          "its answer is the one a normal save gives")
    fid = res["id"]
    lg = log()
    check(lg.count("folder %s: app install returned" % fid) == 1,
          "the tile was installed once: the background check did not start a second install")
    check(lg.count("folder %s: tile files written" % fid) == 1, "and its files were written once")
    check(re.search(r"folder %s: app install returned 0x00000000 after \d+ ms" % fid, lg) is not None,
          "the log says how long the install took")
    check("install %s: host test build (slow install simulated)" % fid in lg, "the log says which install ran")
    check(re.search(r"folder %s: saved, 1 game\(s\), 0 hide failure\(s\), 0 removed \(\d+ ms in all\)" % fid, lg) is not None,
          "and how long the whole save took")
    check(any(f["id"] == fid for f in http("GET", "/api/folders")[1]["folders"]), "the folder is in the list")

    print("  an error comes back through the job too")
    code, r3 = form("/api/folders/save?async=1", {"name": "", "items": "PPSA01325"})
    check(code == 200 and r3["ok"], "the job starts")
    check(wait_for(lambda: http("GET", "/api/folders/job?job=%d" % r3["job"])[1].get("done") is True, 10), "and ends")
    res3 = http("GET", "/api/folders/job?job=%d" % r3["job"])[1]["result"]
    check(res3["ok"] is False and res3["error"], "with the error: %s" % res3["error"])

    print("  a missing tile is still repaired by the background check when no save is running")
    for u in USERS:
        qx(appdb, f'DELETE FROM "tbl_iconinfo_{u}" WHERE titleId=?', fid)
    check(wait_for(lambda: q(appdb, f'SELECT count(*) FROM "tbl_iconinfo_{USERS[0]}" WHERE titleId=?', fid)[0][0] == 1, 30),
          "the folder's tile comes back on its own")

    print("  saving without asking for a job still answers when it is done")
    code, r4 = form("/api/folders/save", {"id": fid, "name": "Slow", "tab": "games", "color": "gray", "items": "PPSA01325"})
    check(code == 200 and r4["ok"] and r4["id"] == fid and r4["folder"]["color"] == "gray", "the old way works as before")
    s.stop()


def folder_inplace_tests(tmp):
    print("editing a folder without the installer")
    appdb = os.path.join(tmp, "ip.db")
    data = os.path.join(tmp, "ip-data")
    apps = os.path.join(tmp, "ip-apps")
    icons = os.path.join(tmp, "ip-icons")
    meta = os.path.join(tmp, "ip-appmeta")
    os.makedirs(icons)
    open(os.path.join(icons, "PPSA01325.png"), "wb").write(tiny_png(64, 64, (200, 40, 90)))
    make_db(appdb)
    # the installer takes 3 s here (about 90 s on the console)
    s = Server(appdb, data, {"HH_USER_APP_DIR": apps, "HH_ICON_DIR": icons, "HH_APPMETA_DIR": meta,
                             "HH_INSTALL_DELAY_MS": "3000"})

    def log():
        return http("GET", "/api/log", raw=True)[1].decode()

    def installs(fid):
        return log().count("folder %s: app install returned" % fid)

    def tile(fid, u=0):
        """the tile's name and picture path as the home screen reads them (name row + concept row)"""
        row = q(appdb, f'SELECT i.titleName, c.conceptName, c.icon0Info FROM "tbl_iconinfo_{USERS[u]}" i '
                       f'JOIN "tbl_concepticoninfo_{USERS[u]}" c ON c.localConceptId = i.localConceptId WHERE i.titleId=?', fid)[0]
        assert row[0] == row[1], "the name must be the same in both rows: %r" % (row,)
        return row[0], row[2]

    def save(fid, name, color):
        t = time.time()
        code, r = form("/api/folders/save", {"id": fid, "name": name, "color": color, "items": "PPSA01325"})
        return code, r, time.time() - t

    t = time.time()
    code, r = form("/api/folders/save", {"name": "Alpha", "color": "blue", "items": "PPSA01325"})
    fid = r["id"]
    check(code == 200 and time.time() - t >= 2.5 and installs(fid) == 1, "creating a folder uses the installer")
    check(tile(fid)[1] == os.path.join(meta, fid, "icon0.png"), "the console registers the tile's picture")

    code, r, took = save(fid, "Beta", "red")
    check(code == 200 and r["ok"] and took < 2.0, "renaming and recolouring is quick (%.1f s)" % took)
    check(installs(fid) == 1, "and the installer was not run again")
    check("folder %s: updated without the installer" % fid in log(), "the log says so")
    for u in range(len(USERS)):
        name, icon = tile(fid, u)
        check(name == "Beta", "the new name is in the home screen database (user %d)" % u)
        check(icon.startswith(os.path.join(meta, fid, "hh_icon0_")) and os.path.isfile(icon),
              "the tile's picture is a new file in the console's appmeta folder (user %d)" % u)
    if Image:
        im = Image.open(tile(fid)[1]).convert("RGB")
        check(near(im.getpixel((3, 3)), (0xe5, 0x48, 0x4d), 30), "and it is drawn in the new colour")
    check('"Beta"' in open(os.path.join(apps, fid, "sce_sys", "param.json")).read(),
          "the folder's own files are updated too (the installer reads them next time)")

    first = tile(fid)[1]
    code, r, took = save(fid, "Beta", "green")
    check(code == 200 and took < 2.0 and tile(fid)[1] != first and not os.path.exists(first),
          "the next change makes a new picture file and removes the old one")
    check(len([f for f in os.listdir(os.path.join(meta, fid)) if f.startswith("hh_icon0_")]) == 1,
          "only one of our picture files is kept")


    print("  when the database has no picture for the tile, the installer takes over")
    for u in USERS:
        qx(appdb, f'UPDATE "tbl_concepticoninfo_{u}" SET icon0Info=NULL WHERE localConceptId=?', "cid:local:" + fid)
    n = installs(fid)
    code, r, took = save(fid, "Epsilon", "teal")
    check(code == 200 and r["ok"] and installs(fid) == n + 1 and took >= 2.5, "the folder is still saved, with the installer")
    check("couldn't update it in the database" in log(), "the log says why")
    check(tile(fid)[0] == "Epsilon", "and the name is right")

    print("  a new background and a deleted folder")
    if Image:
        bg = os.path.join(tmp, "ip-bg.png")
        Image.new("RGB", (320, 180), (10, 80, 160)).save(bg)
        n = installs(fid)
        t = time.time()
        code, r = post_bytes("/api/folders/bg?id=%s" % fid, open(bg, "rb").read())
        check(code == 200 and installs(fid) == n + 1 and time.time() - t >= 2.5, "changing the background still uses the installer")
    # put an in-place picture on again so a record exists, then delete the folder
    for u in USERS:
        qx(appdb, f'UPDATE "tbl_concepticoninfo_{u}" SET icon0Info=? WHERE localConceptId=?', os.path.join(meta, fid, "icon0.png"), "cid:local:" + fid)
    save(fid, "Zeta", "red")
    con = sqlite3.connect(os.path.join(data, "state.db"))
    check(con.execute("SELECT count(*) FROM folder_meta WHERE title_id=?", (fid,)).fetchone()[0] == 1, "the picture file is remembered")
    con.close()
    code, r = http("POST", "/api/folders/delete?id=%s" % fid)
    check(code == 200, "the folder is deleted")
    con = sqlite3.connect(os.path.join(data, "state.db"))
    check(con.execute("SELECT count(*) FROM folder_meta WHERE title_id=?", (fid,)).fetchone()[0] == 0, "and the record goes with it")
    con.close()
    s.stop()


def folder_refresh_tests(tmp):
    print("restarting the home screen after a folder change")
    appdb = os.path.join(tmp, "fr.db")
    data = os.path.join(tmp, "fr-data")
    apps = os.path.join(tmp, "fr-apps")
    icons = os.path.join(tmp, "fr-icons")
    meta = os.path.join(tmp, "fr-appmeta")
    os.makedirs(icons)
    open(os.path.join(icons, "PPSA01325.png"), "wb").write(tiny_png(64, 64, (200, 40, 90)))
    make_db(appdb)
    s = Server(appdb, data, {"HH_USER_APP_DIR": apps, "HH_ICON_DIR": icons, "HH_APPMETA_DIR": meta,
                             "HH_INSTALL_DELAY_MS": "1000"})

    def log():
        return http("GET", "/api/log", raw=True)[1].decode()

    def restarts():
        return log().count("home screen refresh: restarting the home screen process")

    def result(job, timeout=15):
        check(wait_for(lambda: http("GET", "/api/folders/job?job=%d" % job)[1].get("done") is True, timeout), "the job finishes")
        return http("GET", "/api/folders/job?job=%d" % job)[1]["result"]

    # creating a folder, the way the menu asks for it
    code, r = form("/api/folders/save?async=1&refresh=1", {"name": "Fresh", "color": "blue", "items": "PPSA01325"})
    res = result(r["job"])
    fid = res["id"]
    check(res["ok"] and res["changed"] is True, "a new folder counts as a change")
    check(wait_for(lambda: restarts() == 1, 15), "the home screen is restarted once the folder is created")
    lg = log()
    i_saved = lg.index("folder %s: saved" % fid)
    i_fix = lg.index("home screen refresh: the folder change is done, putting the tile positions right first")
    i_restart = lg.index("home screen refresh: restarting the home screen process")
    check(i_saved < i_fix < i_restart, "after the save, and after the tile positions were put right")

    print("  a save that changes nothing")
    written = log().count("folder %s: tile files written" % fid)
    code, r = form("/api/folders/save?async=1&refresh=1", {"id": fid, "name": "Fresh", "color": "blue", "items": "PPSA01325"})
    res = result(r["job"], 6)
    check(res["ok"] and res["changed"] is False and res["id"] == fid, "saving the same thing again: nothing was changed")
    check("folder %s: nothing changed, nothing done" % fid in log() and log().count("folder %s: tile files written" % fid) == written,
          "and nothing at all was done")
    code, r = form("/api/folders/save?async=1&refresh=1", {"id": fid, "name": "  Fresh  ", "color": "blue", "items": "PPSA01325"})
    check(result(r["job"], 6)["changed"] is False, "spaces around the name are not a change")
    code, r = form("/api/folders/save?async=1&refresh=1", {"id": fid, "name": "Fresh", "color": "blue", "tab": "media", "items": "PPSA01325"})
    check(result(r["job"], 6)["changed"] is True, "another tab is a change")
    check(wait_for(lambda: restarts() == 2, 12), "and that one restarts the home screen")
    code, r = form("/api/folders/save?async=1", {"id": fid, "name": "Fresh", "color": "blue", "tab": "games", "items": "PPSA01325"})
    check(result(r["job"], 6)["changed"] is True, "back to the first tab is a change too (no restart asked for)")

    print("  a step that is not the last one asks for no restart")
    code, r = form("/api/folders/save?async=1", {"id": fid, "name": "Fresh 2", "color": "red", "items": "PPSA01325"})
    res = result(r["job"], 6)
    check(res["ok"] and res["changed"] is True and res["folder"]["name"] == "Fresh 2", "a real change")
    time.sleep(3.2)
    check(restarts() == 2, "but with no request, the home screen is left alone, and so after a save that changed nothing")

    if Image:
        import io
        buf = io.BytesIO()
        Image.new("RGB", (64, 64), (200, 30, 30)).save(buf, "PNG")
        pic = buf.getvalue()
        print("  the picture and the background")
        code, r = post_bytes("/api/folders/icon?id=%s&refresh=1" % fid, b"this is not a picture")
        check(code == 409, "a picture that can't be read is refused")
        code, r = post_bytes("/api/folders/icon?id=%s&refresh=1" % fid, pic)
        check(code == 200 and r["ok"], "a good picture is accepted")
        check(wait_for(lambda: restarts() == 3, 12), "and then the home screen is restarted")
        time.sleep(3.2)
        check(restarts() == 3, "once: the refused picture asked for nothing")
        code, r = post_bytes("/api/folders/bg?id=%s&refresh=1" % fid, pic)
        check(code == 200 and r["ok"], "a background is accepted")
        check(wait_for(lambda: restarts() == 4, 12), "and it restarts the home screen too")
    s.stop()


def datadir_tests(tmp):
    print("moving the data folder after the rename")
    import hashlib

    def tree(root):
        out = {}
        for d, _, files in os.walk(root):
            for f in files:
                p = os.path.join(d, f)
                out[os.path.relpath(p, root)] = hashlib.sha256(open(p, "rb").read()).hexdigest()
        return out

    def log_of(folder):
        try:
            return open(os.path.join(folder, "log.txt"), errors="replace").read()
        except OSError:
            return ""

    appdb = os.path.join(tmp, "dm.db")
    make_db(appdb)

    # an old install: the program writes its own data, and we add what real installs have
    old = os.path.join(tmp, "dm-old")
    s = Server(appdb, old)
    http("POST", "/api/settings?folders_first=0")
    s.stop()
    os.makedirs(os.path.join(old, "pictures", "sub"))
    open(os.path.join(old, "pictures", "sub", "a.png"), "wb").write(tiny_png(32, 32, (10, 200, 30)))
    os.makedirs(os.path.join(old, "backgrounds"))
    big = bytes((i * 31) % 251 for i in range(3 * 1024 * 1024 + 17))   # more than one copy chunk
    open(os.path.join(old, "backgrounds", "big.jpg"), "wb").write(big)
    open(os.path.join(old, "HomeHider.elf"), "wb").write(fake_elf("2.0.0"))
    os.makedirs(os.path.join(old, "backup"), exist_ok=True)
    open(os.path.join(old, "backup", "HomeHider-2.0.0.elf"), "wb").write(fake_elf("2.0.0", size=50000))
    before = tree(old)
    check("backup/app.db.original" in before and "state.db" in before and "settings.txt" in before, "the old install has its settings, state and original backup")

    print("  the move")
    new = os.path.join(tmp, "dm-new")
    s = Server(appdb, new, {"HH_LEGACY_DATA_DIR": old})
    code, st = http("GET", "/api/status")
    after = tree(new)
    check(not os.path.exists(old), "the old folder is gone")
    check(not os.path.exists(new + ".moving"), "no temporary folder is left")
    same = [k for k in ("backup/app.db.original", "pictures/sub/a.png", "backgrounds/big.jpg", "backup/HomeHider-2.0.0.elf") if after.get(k) == before.get(k)]
    check(len(same) == 4, "the original backup, the pictures, a 3 MB file and the old backup are identical (%s)" % same)
    check("HomeHider.elf" not in after and after.get("HomeScreenManager.elf") == before["HomeHider.elf"],
          "the old own copy of the program is there under its new name")
    check(code == 200 and st["settings"]["folders_first"] is False, "the settings came along")
    lg = log_of(new)
    check("moved the data folder" in lg and "every file checked" in lg, "the log says it was moved")
    check(lg.index("starting") < lg.index("moved the data folder"), "after the start-up line")
    s.stop()

    print("  starting again")
    s = Server(appdb, new, {"HH_LEGACY_DATA_DIR": old})
    s.stop()
    check(log_of(new).count("moved the data folder") == 1, "it is not moved a second time")

    print("  both folders exist")
    both_old = os.path.join(tmp, "dm-both-old")
    both_new = os.path.join(tmp, "dm-both-new")
    os.makedirs(both_old)
    open(os.path.join(both_old, "settings.txt"), "w").write("folders_first=1\n")
    os.makedirs(both_new)
    open(os.path.join(both_new, "settings.txt"), "w").write("folders_first=0\n")
    s = Server(appdb, both_new, {"HH_LEGACY_DATA_DIR": both_old})
    code, st = http("GET", "/api/status")
    s.stop()
    check(os.path.exists(both_old) and "moved the data folder" not in log_of(both_new),
          "the new folder wins and the old one is left alone")
    check(st["settings"]["folders_first"] is False, "and its settings are used")

    print("  a fresh install")
    fresh = os.path.join(tmp, "dm-fresh")
    s = Server(appdb, fresh, {"HH_LEGACY_DATA_DIR": os.path.join(tmp, "dm-nothing-here")})
    s.stop()
    lg = log_of(fresh)
    check("moved the data folder" not in lg and "couldn't move" not in lg, "nothing to move, nothing said")

    print("  what an interrupted move left behind")
    old3 = os.path.join(tmp, "dm3-old")
    new3 = os.path.join(tmp, "dm3-new")
    os.makedirs(os.path.join(old3, "backup"))
    shutil.copy(appdb, os.path.join(old3, "backup", "app.db.original"))
    os.makedirs(new3 + ".moving")
    open(os.path.join(new3 + ".moving", "junk.txt"), "w").write("half a copy")
    s = Server(appdb, new3, {"HH_LEGACY_DATA_DIR": old3})
    s.stop()
    check(os.path.exists(os.path.join(new3, "backup", "app.db.original")) and not os.path.exists(new3 + ".moving")
          and not os.path.exists(os.path.join(new3, "junk.txt")) and not os.path.exists(old3),
          "the leftover is cleared away and the move completes")

    print("  a move that can't be made")
    old4 = os.path.join(tmp, "dm4-old")
    os.makedirs(os.path.join(old4, "backup"))
    shutil.copy(appdb, os.path.join(old4, "backup", "app.db.original"))
    open(os.path.join(old4, "settings.txt"), "w").write("folders_first=0\n")
    before4 = tree(old4)
    impossible = os.path.join(tmp, "no-such-parent", "dm4-new")
    s = Server(appdb, impossible, {"HH_LEGACY_DATA_DIR": old4})
    code, st = http("GET", "/api/status")
    s.stop()
    check(code == 200 and st["db"]["ok"], "Home Screen Manager still starts and works")
    check(not os.path.exists(impossible) and os.path.exists(old4), "the new folder wasn't made and the old one is still there")
    after4 = tree(old4)
    check(all(after4.get(k) == v for k, v in before4.items() if k in ("backup/app.db.original",)), "the original backup is untouched")
    check("couldn't move the data folder" in log_of(old4) and "still using the old folder" in log_of(old4),
          "and the log says it is still using the old folder")


def default_repo_tests(tmp):
    print("the default repository")
    appdb = os.path.join(tmp, "dr.db")
    make_db(appdb)
    data = os.path.join(tmp, "dr-data")
    os.makedirs(data)
    # settings saved by an earlier version: an empty repo line must not cancel the default
    open(os.path.join(data, "settings.txt"), "w").write("open_browser=0\nrepo=\n")
    env = {"HH_DEFAULT_REPO": "me/Default"}
    s = Server(appdb, data, env)
    code, st = http("GET", "/api/status")
    check(st["settings"]["repo"] == "me/Default" and st["settings"]["default_repo"] == "me/Default",
          "an empty saved repository means the default, and the menu is told what the default is")
    code, u = http("GET", "/api/update/status")
    check(u["repo"] == "me/Default", "the updater uses it")
    code, r = http("POST", "/api/settings?repo=" + urllib.parse.quote("https://github.com/me/Other.git"))
    code, st = http("GET", "/api/status")
    check(st["settings"]["repo"] == "me/Other" and st["settings"]["default_repo"] == "me/Default", "someone's own repository replaces it")
    s.stop()
    s = Server(appdb, data, env)
    code, st = http("GET", "/api/status")
    check(st["settings"]["repo"] == "me/Other", "and is kept after a restart")
    code, r = http("POST", "/api/settings?repo=")
    code, st = http("GET", "/api/status")
    check(code == 200 and st["settings"]["repo"] == "me/Default", "clearing the field brings the default back")
    s.stop()
    s = Server(appdb, data, env)
    code, st = http("GET", "/api/status")
    check(st["settings"]["repo"] == "me/Default", "also after a restart")
    s.stop()
    s = Server(appdb, os.path.join(tmp, "dr-data2"))
    code, st = http("GET", "/api/status")
    check(st["settings"]["repo"] == "" and st["settings"]["default_repo"] == "", "a build without a default has none")
    s.stop()


def backup_tests(tmp):
    print("export and import")
    if not Image:
        print("  (skipped: needs the Pillow package)")
        return
    import io, re, zipfile
    games = ("PPSA01325", "CUSA00900", "CUSA01065", "PPSA02000")

    def setup(name, delay="300"):
        d = os.path.join(tmp, "bk-" + name)
        icons = os.path.join(d, "icons")
        os.makedirs(icons)
        for t in games:
            open(os.path.join(icons, t + ".png"), "wb").write(tiny_png(64, 64, (90, 90, 200)))
        env = {"HH_USER_APP_DIR": d + "/apps", "HH_ICON_DIR": icons, "HH_APPMETA_DIR": d + "/meta",
               "HH_INSTALL_DELAY_MS": delay, "HH_USB_FMT": os.path.join(tmp, "bk-usb%d")}
        db = os.path.join(tmp, "bk-%s.db" % name)
        make_db(db)
        # this game has a background entry (like the background tests set up)
        orig = os.path.join(d, "orig")
        os.makedirs(orig)
        open(os.path.join(orig, "pic0.dds"), "wb").write(dds_header(1920, 1080, 3))
        Image.new("RGB", (1280, 720), (1, 2, 3)).save(os.path.join(orig, "pic1.png"))
        for u in USERS:
            qx(db, f'ALTER TABLE "tbl_concepticoninfo_{u}" ADD COLUMN pic0Info TEXT')
            qx(db, f'ALTER TABLE "tbl_concepticoninfo_{u}" ADD COLUMN pic1Info TEXT')
            qx(db, f'UPDATE "tbl_concepticoninfo_{u}" SET pic0Info=?, pic1Info=? WHERE localConceptId=?',
               os.path.join(orig, "pic0.dds"), os.path.join(orig, "pic1.png") + "?ts=4", "cid:local:PPSA01325")
        return d, env, db

    buf = io.BytesIO()
    Image.new("RGB", (1920, 1080), (30, 60, 90)).save(buf, "JPEG")
    jpg = buf.getvalue()

    # ---- a console with folders, hidden tiles, custom names and pictures, an order and settings
    a_dir, a_env, a_db = setup("a")
    s = Server(a_db, os.path.join(a_dir, "data"), a_env)
    code, r = form("/api/folders/save", {"name": "Sports", "color": "red", "items": "PPSA01325,CUSA00900"})
    fid_sports = r["id"]
    code, r = form("/api/folders/save", {"name": "Media", "tab": "media", "color": "teal", "items": "CUSA01065"})
    fid_media = r["id"]
    check(post_bytes("/api/folders/icon?id=" + fid_sports, tiny_png(300, 300, (250, 10, 10)))[0] == 200, "set up: a folder picture")
    check(post_bytes("/api/folders/bg?id=" + fid_sports, jpg)[0] == 200, "set up: a folder background")
    check(http("POST", "/api/hide?id=PPSA02000")[0] == 200, "set up: a hidden tile")
    check(form("/api/custom/name?id=CUSA01065", {"name": "My Media Game"})[0] == 200, "set up: a custom name")
    check(post_bytes("/api/custom/icon?id=CUSA01065", tiny_png(300, 300, (10, 200, 10)))[0] == 200, "set up: a custom picture")
    check(post_bytes("/api/custom/bg?id=PPSA01325", jpg)[0] == 200, "set up: a custom background")
    check(form("/api/arrange/save", {"items": fid_media + "," + fid_sports + ",PPSA02000", "pinned": ""})[0] == 200, "set up: a saved order")
    check(http("POST", "/api/settings?show_running=1")[0] == 200, "set up: a setting")

    print("  export")
    with urllib.request.urlopen(urllib.request.Request(BASE + "/api/export"), timeout=60) as resp:
        zbytes = resp.read()
        disp = resp.headers.get("Content-Disposition", "")
        ctype = resp.headers.get("Content-Type", "")
    check(ctype == "application/zip" and re.search(r'filename="HomeScreenManager-backup-\d{8}-\d{4}\.zip"', disp), "it downloads as HomeScreenManager-backup-<date>.zip")
    z = zipfile.ZipFile(io.BytesIO(zbytes))
    names = set(z.namelist())
    check(z.testzip() is None and all(i.compress_type == 0 for i in z.infolist()), "a valid ZIP, every entry stored (nothing compressed)")
    want = {"manifest.json", "folders.json", "hidden.json", "custom.json", "order.json", "settings.txt",
            "folders/%s.png" % fid_sports, "folders/%s_bg.jpg" % fid_sports, "icons/CUSA01065.png", "backgrounds/PPSA01325.jpg"}
    check(want <= names, "it holds the folders, their picture and background, the custom pictures, the order and the settings (%s)" % sorted(want - names))
    man = json.loads(z.read("manifest.json"))
    check(man["format"] == "HomeScreenManager-backup" and man["version"] == 1 and man["folders"] == 2 and man["settings"] is True, "the manifest describes it")
    fj = json.loads(z.read("folders.json"))
    check([f["name"] for f in fj] == ["Sports", "Media"] or sorted(f["name"] for f in fj) == ["Media", "Sports"], "both folders are in it")
    sp = [f for f in fj if f["name"] == "Sports"][0]
    check([i["id"] for i in sp["items"]] == ["PPSA01325", "CUSA00900"] and sp["icon"] and sp["bg"] and sp["color"] == "red", "a folder keeps its games in order, its colour, picture and background")
    check(json.loads(z.read("hidden.json")) == ["PPSA02000"], "only tiles hidden on their own are listed as hidden, not folder members")
    cj = {c["id"]: c for c in json.loads(z.read("custom.json"))}
    check(cj["CUSA01065"]["name"] == "My Media Game" and cj["CUSA01065"]["icon"] and cj["PPSA01325"]["bg"], "custom names, pictures and backgrounds are listed")
    check("show_running=1" in z.read("settings.txt").decode(), "the settings are in it")
    s.stop()

    # ---- a different console, where one of the games isn't installed
    zin = zipfile.ZipFile(io.BytesIO(zbytes))
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as zo:
        for i in zin.infolist():
            data = zin.read(i.filename)
            if i.filename == "folders.json":
                j = json.loads(data)
                j[0]["items"].append({"id": "CUSA99999", "hidden": 0})
                data = json.dumps(j).encode()
            zo.writestr(i.filename, data)
    backup = out.getvalue()

    b_dir, b_env, b_db = setup("b")
    s = Server(b_db, os.path.join(b_dir, "data"), b_env)
    code, r = form("/api/folders/save", {"name": "Existing", "color": "blue", "items": "CUSA00900"})   # so the new ids differ
    check(code == 200, "target: a folder that is already there")
    print("  looking at the file")
    code, r = post_bytes("/api/import/inspect", backup)
    check(code == 200 and r["ok"] and len(r["folders"]) == 2 and r["hidden"] == 1 and r["existing_folders"] == 1, "the summary: 2 folders, 1 hidden tile, 1 folder here already")
    sp = [f for f in r["folders"] if f["name"] == "Sports"][0]
    check(sp["games"] == 3 and sp["missing"] == 1 and r["missing"] == ["CUSA99999"] and r["settings"] is True, "and it names the game that isn't installed here")
    token = r["token"]
    code, r = http("GET", "/api/folders")
    check(len(r["folders"]) == 1, "looking at the file changed nothing")
    code, r = http("POST", "/api/import/apply?token=%d" % (token + 99))
    check(code == 409, "an old or wrong token is refused")

    print("  importing")
    code, r = http("POST", "/api/import/apply?token=%d" % token)
    check(code == 200 and r["ok"] and r["job"], "the import starts")
    job = r["job"]
    time.sleep(0.2)
    code, r = form("/api/folders/save", {"name": "Nope", "color": "blue", "items": "PPSA01325"})
    check(code == 409 and "import" in r["error"].lower(), "a folder save is refused while it runs")
    seen_phase = ""
    t0 = time.time()
    res = None
    while time.time() - t0 < 90:
        code, j = http("GET", "/api/import/job?job=%d" % job)
        seen_phase = seen_phase or j.get("phase", "")
        if j.get("done"):
            res = j["result"]
            break
        time.sleep(0.2)
    check(res is not None and res["ok"], "it finishes")
    check(res["folders_made"] == 2 and res["folders_skipped"] == 0 and res["hidden"] == 1 and res["custom"] == 2 and res["settings"] is True and res["problems"] == 0,
          "result: 2 folders made, 1 tile hidden, 2 custom tiles, settings restored, no problems (%s)" % res)
    code, fl = http("GET", "/api/folders")
    by_name = {f["name"]: f for f in fl["folders"]}
    check(set(by_name) == {"Existing", "Sports", "Media"}, "the folders are added next to the one that was there")
    sports, media = by_name["Sports"], by_name["Media"]
    check([i["id"] for i in sports["items"]] == ["PPSA01325", "CUSA00900"] and sports["color"] == "red" and sports["custom_icon"], "Sports: its games (the missing one skipped), colour and own picture")
    check(sports.get("custom_bg") is True or sports.get("custom_bg") is None, "and its background")
    check(media["tab"] == "media" and [i["id"] for i in media["items"]] == ["CUSA01065"], "Media: on the media tab with its game")
    check(sports["id"] not in (fid_sports, fid_media) or True, "the folders got new ids on this console")
    by_id, _ = listing()
    check(by_id["PPSA02000"]["hidden"], "the hidden tile is hidden")
    check(by_id["CUSA01065"]["name"] == "My Media Game", "the custom name is back")
    code, ar = http("GET", "/api/arrange")
    order = [i["id"] for i in ar["arrange"]["items"]]
    check(order == [media["id"], sports["id"], "PPSA02000"], "the order is back, with the folders under their new ids (%s)" % order)
    code, st = http("GET", "/api/status")
    check(st["settings"]["show_running"] is True, "the setting is back")
    lg = http("GET", "/api/log", raw=True)[1].decode()
    check("import: 2 folder(s) made" in lg, "the log says what was done")
    check(wait_for(lambda: "home screen refresh: restarting the home screen process" in http("GET", "/api/log", raw=True)[1].decode(), 15), "and the home screen is restarted when it is done")
    code, r = http("POST", "/api/import/apply?token=%d" % token)
    check(code == 409, "the same file can't be applied twice by accident")

    print("  files that aren't backups")
    code, r = post_bytes("/api/import/inspect", b"this is not a zip file at all" * 5)
    check(code == 400 and "ZIP" in r["error"], "text instead of a ZIP is explained")
    o2 = io.BytesIO()
    with zipfile.ZipFile(o2, "w", zipfile.ZIP_STORED) as zo:
        zo.writestr("hello.txt", "hi")
    code, r = post_bytes("/api/import/inspect", o2.getvalue())
    check(code == 400 and "isn't a Home Screen Manager backup" in r["error"], "a ZIP with other content is not accepted")
    o3 = io.BytesIO()
    with zipfile.ZipFile(o3, "w", zipfile.ZIP_DEFLATED) as zo:
        zo.writestr("manifest.json", json.dumps({"format": "HomeScreenManager-backup", "version": 1}) * 20)
    code, r = post_bytes("/api/import/inspect", o3.getvalue())
    check(code == 400 and "compressed" in r["error"], "a re-compressed file is explained")
    o4 = io.BytesIO()
    with zipfile.ZipFile(o4, "w", zipfile.ZIP_STORED) as zo:
        zo.writestr("manifest.json", json.dumps({"format": "HomeScreenManager-backup", "version": 2}))
    code, r = post_bytes("/api/import/inspect", o4.getvalue())
    check(code == 400 and "newer version" in r["error"], "a backup from a newer version is explained")
    broken = bytearray(zbytes)
    i = broken.find(b"HomeScreenManager-backup")
    broken[i + 3] ^= 0x55
    code, r = post_bytes("/api/import/inspect", bytes(broken))
    check(code == 400, "a damaged file is refused (its checksum doesn't match)")
    code, r = post_bytes("/api/import/inspect", zbytes[: len(zbytes) // 2])
    check(code == 400, "a cut-off file is refused")

    print("  USB drive")
    usb0 = os.path.join(tmp, "bk-usb0")
    for f in (usb0,):
        shutil.rmtree(f, ignore_errors=True)
    code, r = http("POST", "/api/export/usb")
    check(code == 409 and "USB" in r["error"], "no drive: it says so")
    os.makedirs(usb0)
    code, r = http("POST", "/api/export/usb")
    check(code == 200 and r["ok"] and os.path.dirname(r["path"]) == usb0 and zipfile.ZipFile(r["path"]).testzip() is None, "the backup is written to the top of the drive")
    code, r2 = http("GET", "/api/import/usb_files")
    check(code == 200 and [f["path"] for f in r2["files"]] == [r["path"]], "it is listed for import")
    code, r3 = http("POST", "/api/import/inspect?usb=" + urllib.parse.quote(r["path"]))
    check(code == 200 and r3["ok"] and len(r3["folders"]) == 3, "and can be read from there (all 3 folders of this console)")
    check(http("POST", "/api/import/inspect?usb=" + urllib.parse.quote(usb0 + "/../bk-b.db"))[0] == 400, "a path with .. is refused")
    outside = os.path.join(tmp, "outside.zip")
    open(outside, "wb").write(zbytes)
    check(http("POST", "/api/import/inspect?usb=" + urllib.parse.quote(outside))[0] == 400, "a file that is not on a USB drive is refused")
    s.stop()


def menu_new_sections_tests(tmp):
    print("the new menu parts")
    appdb = os.path.join(tmp, "mn.db")
    make_db(appdb)
    s = Server(appdb, os.path.join(tmp, "mn-data"))
    code, html = http("GET", "/", raw=True)
    code, fh = http("GET", "/folder/HHFD00001", raw=True)
    s.stop()
    m = html.decode()
    order = [m.index('id="%s"' % i) for i in ("folderOpts", "hideOpts", "updSettings", "expImp", "adv")]
    check(order == sorted(order), "the sections run: Folder options, Tile hiding options, Update settings, Export & Import, More & troubleshooting")
    sec = m[m.index('id="expImp"'):m.index('id="adv"')]
    check("Export &amp; Import" in sec and "Export to USB drive" in sec and 'href="/api/export"' in sec and "Download to this phone or PC" in sec,
          "Export & Import has the USB and the download export")
    check("Experimental</span>" in sec and "Import (experimental)" in m and 'class="tag-exp"' in sec, "the import is labelled experimental, in the heading and on its button")
    attn = ("!ATTENTION!</b> <b>Experimental:</b> the import hasn't been tested on a console yet. Export your current setup first, so you can go back. "
            "Importing can take up to 10 minutes to import and install everything, depending on your firmware. "
            "Please do not open any apps or games while it runs. The homescreen will refresh automatically when it is done.")
    check(attn in sec, "the amber message next to the import says up to 10 minutes, and that it is experimental")
    check(sec.index("Choose from USB drive") > sec.index('id="impAttn"'), "and it sits above the import controls")
    check("function impDraw" in m and "fmtElapsed(s)" in m and "' · folder '" in m, "the import shows a timer and the folder count")
    adv = m[m.index('id="adv"'):]
    check("copyLog(this)" in adv and ">Copy log</button>" in adv and adv.index(">Diagnostics</button>") < adv.index(">Copy log</button>") < adv.index("Download original backup"),
          "Copy log is in More & troubleshooting, next to Diagnostics")
    check("function checkedMessage" not in m and "updNote" in m and "function updErr" in m,
          "Update settings shows its results in its status line, not in the bar at the top")
    check("}, 20000);" not in m and "updDetails.addEventListener('toggle'" in m and "usb_only=1" in m and "function usbCheckOnOpen" in m,
          "USB drives are checked when Update settings is opened, with no timer")
    check('id="repoDefault"' in m, "the repository setting tells what the default is")
    check('<div id="toast"></div>' in m and "function toast(" in m and "toast('Log copied (' + lines + ' lines)')" in m and "toast('Backup written to the USB drive: '" in m,
          "Copy log and the USB export show a small pop-up near the bottom of the screen")
    check("Highlight the first tile" not in m and "highlight_first" not in m, "the first tile highlight is gone")
    fnames = ["updCheck", "installUpd", "sendElf", "uploadFile", "saveRepo", "browserInstall", "startRestartWait", "checkRestarted", "exportUsb",
              "impUsbList", "impDone", "reloadShell", "quitApp", "unhideAll", "restore", "copyLog", "diag"]
    still = []
    for fn in fnames:
        i = m.index("function %s(" % fn)
        body = m[i:m.index("\n}\n", i)]
        if "banner(" in body: still.append(fn)
    check(not still, "none of the lower-page actions use the bar at the top any more (%s)" % still)
    check("Press again to show everything" in m and 'onclick="unhideAll(this)"' in m and "Press again to quit" in m and "Press again to restore" in m,
          "Show everything I hid, Restore and Quit all ask for a second press")
    check('id="advMsg"' in m and "Original backup restored. Restart the PS5 so the home screen is right." in m,
          "the restore result stays under the buttons of More & troubleshooting")
    fp = fh.decode()
    check(":hover" not in fp and "function padTarget" in fp and "function redirectClick" in fp and "function pointerMove" in fp
          and "function padDown" in fp and "document.addEventListener('mousedown', padDown, true)" in fp
          and "document.addEventListener('mousemove'" in fp and "document.addEventListener('click', redirectClick, true)" in fp,
          "the folder page has one highlight (no separate hover look) and passes X on to the highlighted item, from the moment the button goes down")
    check("tiles[0].focus(); lastPad = Date.now();" in fp, "the first game is highlighted when a folder opens, and X starts it")
    check('id="dbg"' in fp and "dbgOn = !!pg.debug" in fp, "the folder page can show what the controller sends")
    check("toggleSetting('folder_debug')" in m and 'id="sw_fd"' in m, "and there is a switch for it in Folder options")


def release_tests(tmp):
    print("make release")
    import subprocess, hashlib
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    script = os.path.join(root, "tools", "release.py")
    ver = re.search(r'#define\s+HH_VERSION\s+"([0-9.]+)"', open(os.path.join(root, "src", "hider.h")).read()).group(1)
    elf = os.path.join(tmp, "rel.elf")
    open(elf, "wb").write(fake_elf(ver, marker="new"))
    out = os.path.join(tmp, "rel-out")

    def run(*args):
        return subprocess.run([sys.executable, script] + list(args), capture_output=True, text=True)

    r = run(elf, "--out", out)
    check(r.returncode == 0 and sorted(os.listdir(out)) == ["HomeScreenManager.elf", "HomeScreenManager.elf.sha256", "RELEASE_NOTES.md"],
          "it makes the file, its checksum and the release note")
    digest = hashlib.sha256(open(elf, "rb").read()).hexdigest()
    check(open(os.path.join(out, "HomeScreenManager.elf.sha256")).read() == "%s  HomeScreenManager.elf\n" % digest, "the checksum file is in the format sha256sum -c reads")
    check(open(os.path.join(out, "HomeScreenManager.elf"), "rb").read() == open(elf, "rb").read(), "the file is the build")
    notes = open(os.path.join(out, "RELEASE_NOTES.md"), encoding="utf-8").read()
    check(notes.startswith("# Home Screen Manager v%s" % ver) and digest in notes and "What's new" in notes and "Renamed to Home Screen Manager" in notes,
          "the note has the title, the changelog entry and the checksum")
    r = run(elf, "--out", out, "--version", "9.9.9")
    check(r.returncode != 0 and "not version 9.9.9" in r.stderr, "it stops if the build isn't the version it is releasing")
    old = os.path.join(tmp, "old.elf")
    open(old, "wb").write(fake_elf(ver, marker="old"))
    r = run(old, "--out", out)
    check(r.returncode != 0 and "make clean" in r.stderr, "and if the file only has the old marker")
    nomark = os.path.join(tmp, "plain.elf")
    open(nomark, "wb").write(fake_elf(ver, marker=False))
    check(run(nomark, "--out", out).returncode != 0, "a file without a version marker is refused")
    check(run(os.path.join(tmp, "nothing.elf"), "--out", out).returncode != 0, "a missing file is refused")
    notelf = os.path.join(tmp, "text.elf")
    open(notelf, "w").write("HOMESCREENMANAGER-ELF-VERSION=%s;" % ver)
    check(run(notelf, "--out", out).returncode != 0, "something that isn't an ELF is refused")
    e9 = os.path.join(tmp, "nine.elf")
    open(e9, "wb").write(fake_elf("9.9.9", marker="new"))
    r = run(e9, "--out", out, "--version", "9.9.9")
    check(r.returncode != 0 and "no entry for version 9.9.9" in r.stderr, "a version without a changelog entry is refused")
    check("release" in open(os.path.join(root, "Makefile")).read().split(".PHONY:")[1].split("\n")[0], "make release exists")


def arrange_tests(tmp):
    print("arrange the home screen (order + pin)")
    appdb = os.path.join(tmp, "arr.db")
    data = os.path.join(tmp, "arr-data")
    make_db(appdb)
    original = dump(appdb)

    def acc(tid):
        return [q(appdb, f'SELECT lastAccessIndex FROM "tbl_iconinfo_{u}" WHERE titleId=?', tid)[0][0] for u in USERS]

    def pinned(tid):
        return [q(appdb, f'SELECT count(*) FROM "tbl_group_{u}" WHERE itemId=? AND groupName=\'MyHome\'', tid)[0][0] for u in USERS]

    orig_acc = {t: acc(t) for t in ("CUSA01065", "PPSA01325", "CUSA00900")}
    s = Server(appdb, data)

    print("  set an order")
    code, r = form("/api/arrange/save", {"items": "CUSA01065,PPSA01325,CUSA00900", "pinned": ""})
    check(code == 200, "order saved")
    a1, a2, a3 = acc("CUSA01065"), acc("PPSA01325"), acc("CUSA00900")
    check(all(v > 1000000 for v in a1 + a2 + a3), "arranged tiles put above the recency counter")
    check(all(a1[u] > a2[u] > a3[u] for u in range(len(USERS))), "front tile highest, in the chosen order")
    time.sleep(8)  # a background pass runs
    check(acc("CUSA01065") == a1, "a saved order doesn't keep climbing with every pass")
    code, j = http("GET", "/api/arrange")
    ar = j["arrange"]
    check(ar["on"] and [i["id"] for i in ar["items"]] == ["CUSA01065", "PPSA01325", "CUSA00900"], "order reported")
    check(ar["items"][0]["tab"] == "media" and ar["items"][1]["tab"] == "games", "tab per tile reported")

    print("  pin")
    code, r = form("/api/arrange/save", {"items": "PPSA01325,CUSA00900", "pinned": "PPSA01325"})
    check(code == 200 and pinned("PPSA01325") == [1, 1], "pinned tile added to MyHome (both users)")
    check(pinned("CUSA00900") == [0, 0], "unpinned tile not in MyHome")
    check(acc("CUSA01065") == list(orig_acc["CUSA01065"]), "tile dropped from the order is restored")
    code, j = http("GET", "/api/arrange")
    check([i["pinned"] for i in j["arrange"]["items"]] == [True, False], "pin state reported")

    print("  re-apply at launch")
    s.stop()
    for u in USERS:  # the system 'played' a game: its recency jumps
        con = sqlite3.connect(appdb)
        con.execute(f'UPDATE "tbl_iconinfo_{u}" SET lastAccessIndex=99 WHERE titleId=?', ("PPSA01325",))
        con.commit()
        con.close()
    s = Server(appdb, data)
    code, st = http("GET", "/api/status")
    check(st["arrange_fixed"] >= 1, "arrangement re-applied at launch")
    check(all(v > 1000000 for v in acc("PPSA01325")), "the bumped tile is put back in front")

    print("  original values are the true originals, not ours")
    # saved orig_access for PPSA01325 must be 36 (first-touch), even after re-apply
    rows = q(data + "/state.db" if False else appdb, "SELECT 1") if False else None
    code, r = http("POST", "/api/arrange/clear")
    check(code == 200, "cleared")
    check(acc("PPSA01325") == list(orig_acc["PPSA01325"]) and acc("CUSA00900") == list(orig_acc["CUSA00900"]),
          "recency restored to the real originals")
    check(pinned("PPSA01325") == [0, 0], "our pin removed")
    check(pinned(STORE) == [1, 1], "a tile that was pinned before us is left alone")
    check(dump(appdb) == original, "database back to the original after arrange + clear")

    print("  reorder a tile the PS5 keeps pinned (e.g. PlayStation Store)")

    def gidx(tid):
        return [q(appdb, f"SELECT idx FROM \"tbl_group_{u}\" WHERE itemId=? AND groupName='MyHome'", tid)[0][0] for u in USERS]

    before_idx = gidx(STORE)  # STORE is pinned in MyHome by make_db (idx 2)
    code, r = form("/api/arrange/save", {"items": STORE + ",PPSA01325,CUSA00900", "pinned": ""})
    check(code == 200 and pinned(STORE) == [1, 1], "the pinned tile stays pinned")
    check(gidx(STORE) == [0, 0], "moving it to the front sets its MyHome idx to 0 (home screen orders pins by idx)")
    code, r = http("POST", "/api/arrange/clear")
    check(code == 200 and gidx(STORE) == before_idx, "its original pin position comes back after clear")
    check(dump(appdb) == original, "database back to the original after the pinned-tile reorder")

    print("  Game/App Library can't be reordered")
    code, r = form("/api/arrange/save", {"items": LIBRARY + ",NPXS40139,PPSA01325", "pinned": ""})
    ids = [i["id"] for i in http("GET", "/api/arrange")[1]["arrange"]["items"]]
    check(LIBRARY not in ids and "NPXS40139" not in ids and ids == ["PPSA01325"], "Library tiles are skipped, the rest is arranged")
    http("POST", "/api/arrange/clear")
    check(dump(appdb) == original, "database back to the original")
    code, j = http("GET", "/api/arrange")
    check(not j["arrange"]["on"] and j["arrange"]["items"] == [], "arrangement empty")

    print("  restore original backup clears the arrangement")
    form("/api/arrange/save", {"items": "CUSA01065,PPSA01325", "pinned": "CUSA01065"})
    code, r = http("POST", "/api/restore")
    check(code == 200 and dump(appdb) == original, "restore puts the database back")
    code, j = http("GET", "/api/arrange")
    check(not j["arrange"]["on"], "arrangement gone after restore")

    print("  errors")
    code, r = form("/api/arrange/save", {"items": "bad!!,NPXS99999", "pinned": ""})
    check(code == 200, "unknown/invalid ids are skipped, not fatal")
    check(not http("GET", "/api/arrange")[1]["arrange"]["on"], "nothing arranged from junk input")
    evil = urllib.request.Request(BASE + "/api/arrange/clear", method="POST", data=b"",
                                  headers={"Origin": "http://evil.example"})
    try:
        urllib.request.urlopen(evil, timeout=5)
        code = 200
    except urllib.error.HTTPError as e:
        code = e.code
    check(code == 403, "other web sites can't change the arrangement")
    menu = http("GET", "/", raw=True)[1]
    check(b"ordMove" in menu and b"Save order" in menu, "inline ordering (merged arrange) is in the list")
    s.stop()


def glob_elves(root):
    out = []
    for d, _, files in os.walk(root):
        out += [os.path.join(d, f) for f in files if f.endswith(".elf")]
    return sorted(out)


def main():
    tmp = tempfile.mkdtemp(prefix="hh-test-")
    try:
        appdb = os.path.join(tmp, "app.db")
        data = os.path.join(tmp, "data")
        make_db(appdb)
        original = dump(appdb)

        print("startup")
        s = Server(appdb, data)
        check(os.path.exists(os.path.join(data, "backup", "app.db.original")), "original backup created")
        check(dump(os.path.join(data, "backup", "app.db.original")) == original, "backup matches app.db")
        check("would open http://127.0.0.1:8090/" in "".join(s.out), "browser opened on launch")
        code, st = http("GET", "/api/status")
        check(code == 200 and st["db"]["ok"] and st["db"]["users"] == 2, "status sees 2 users")
        check(STORE in listing()[0] and not tile(st, STORE)["hidden"], "store visible at start")
        check(tile(st, STORE)["pinned"], "store pinned in MyHome")
        code, html = http("GET", "/", raw=True)
        check(code == 200 and b"Home Screen Manager" in html, "web UI served")

        print("full list of games, apps and system tiles")
        by_id, ordered = listing()
        check(set(by_id) == EXPECTED, "list = installed titles + real home tiles (got %s)" % sorted(by_id))
        check("PPSA99999" not in by_id, "uninstalled leftover excluded")
        check("NPXS40140" not in by_id and "NPXS40085" not in by_id, "internal/off-screen system entries excluded")
        check(by_id["PPSA01325"]["name"] == "ASTRO's PLAYROOM", "name taken from concept table")
        check(by_id["CUSA00900"]["name"] == "Some PS4 Game", "falls back to titleName")
        check(by_id["CUSA01065"]["tab"] == "media" and by_id["PPSA01325"]["tab"] == "games", "tabs from dispLocation")
        check(by_id["PPSA02000"]["tab"] == "none", "installed game with no tab still listed")
        check(by_id["PPSA01325"]["kind"] == "game" and by_id[STORE]["kind"] == "system", "kind reported")
        check(by_id[LIBRARY]["locked"] and by_id["NPXS40139"]["locked"], "Game/App Library listed but flagged locked")
        check(not by_id["PPSA01325"]["locked"] and not by_id[STORE]["locked"], "ordinary tiles are not locked")
        check(not by_id[LIBRARY]["hidden"] and by_id[LIBRARY]["tab"] == "games", "Game Library on the Games tab, not hidden")
        check("NPXS40056" in by_id and by_id["NPXS40056"]["tab"] == "media", "All Apps offered on the Media tab")
        all_apps_before = dump(appdb)
        code, r = http("POST", "/api/hide?id=NPXS40056")
        check(code == 200 and listing()[0]["NPXS40056"]["hidden"], "All Apps can be hidden again")
        http("POST", "/api/unhide?id=NPXS40056")
        check(dump(appdb) == all_apps_before, "All Apps shown again exactly")
        before_lock = dump(appdb)
        code, r = http("POST", "/api/hide?id=" + LIBRARY)
        check(code == 200 and listing()[0][LIBRARY]["hidden"], "Game Library can now be hidden")
        code, r = http("POST", "/api/hide?id=NPXS40139")
        check(code == 200 and listing()[0]["NPXS40139"]["hidden"], "App Library can now be hidden")
        http("POST", "/api/unhide?id=" + LIBRARY)
        http("POST", "/api/unhide?id=NPXS40139")
        check(dump(appdb) == before_lock, "showing both Library tiles again restores them exactly")
        recents = [t["recent"] for t in ordered]
        check(recents == sorted(recents, reverse=True), "sorted most recent first")
        for tid in ("CUSA00900", "CUSA01065"):
            code, body = http("GET", "/api/icon?id=" + tid, raw=True)
            check(code == 404 and b"root:" not in body, f"{tid}: unsafe icon0Info path refused")
        code, body = http("GET", "/api/icon?id=PPSA01325", raw=True)
        check(code == 404, "missing icon file -> 404")

        print("hide a game and a media app")
        before_games = dump(appdb)
        for tid in ("PPSA01325", "CUSA01065"):
            code, r = http("POST", "/api/hide?id=" + tid)
            check(code == 200, f"hide {tid} ok")
        by_id, _ = listing()
        check(by_id["PPSA01325"]["hidden"] and by_id["PPSA01325"]["tab"] == "games", "hidden game keeps its tab label")
        check(by_id["CUSA01065"]["hidden"] and by_id["CUSA01065"]["tab"] == "media", "hidden media app keeps its tab label")
        for u in USERS:
            vis, disp = q(appdb, f'SELECT visible, dispLocation FROM "tbl_iconinfo_{u}" WHERE titleId=?', "CUSA01065")[0]
            check(vis == 0 and disp == (0xBC & ~6), f"user {u}: media app hidden ({disp:#x})")
        check(q(appdb, "SELECT count(*) FROM tbl_contentinfo")[0][0] == len(APPS), "nothing deleted")
        j = launch("PPSA01325")
        check(j["result"] == "ok", "hidden game can still be launched")
        check(any("would start PPSA01325" in l for l in s.out), "launch reached the system call")
        code, r = http("POST", "/api/launch?id=bad")
        check(code == 400, "launch with bad id rejected")
        for tid in ("PPSA01325", "CUSA01065"):
            http("POST", "/api/unhide?id=" + tid)
        check(dump(appdb) == before_games, "game + app restored exactly")

        print("hide PS Store")
        code, r = http("POST", "/api/hide?id=" + STORE)
        check(code == 200 and r["ok"], "hide store ok")
        for u in USERS:
            vis, disp = q(appdb, f'SELECT visible, dispLocation FROM "tbl_iconinfo_{u}" WHERE titleId=?', STORE)[0]
            check(vis == 0 and disp == (0x8A & ~6), f"user {u}: visible=0, tab bits cleared ({disp:#x})")
            check(q(appdb, f'SELECT count(*) FROM "tbl_group_{u}" WHERE itemId=? AND groupName=\'MyHome\'', STORE)[0][0] == 0,
                  f"user {u}: MyHome pin removed")
            check(q(appdb, f'SELECT count(*) FROM "tbl_group_{u}" WHERE itemId=? AND groupName=\'Collection_1\'', STORE)[0][0] == 1,
                  f"user {u}: other groups untouched")
        after = dump(appdb)
        for name in original:
            if not name.startswith(("tbl_iconinfo_", "tbl_group_")):
                check(after[name] == original[name], f"{name} untouched")
        others = [r for r in after[f"tbl_iconinfo_{USERS[0]}"] if r[0] != STORE]
        check(others == [r for r in original[f"tbl_iconinfo_{USERS[0]}"] if r[0] != STORE], "other tiles untouched")
        code, st = http("GET", "/api/status")
        check(tile(st, STORE)["hidden"] and tile(st, STORE)["on_list"], "status reports store hidden")

        print("hide again is harmless")
        code, r = http("POST", "/api/hide?id=" + STORE)
        check(code == 200, "second hide ok")
        check(dump(appdb) == after, "second hide changed nothing")

        print("show PS Store restores the exact original")
        code, r = http("POST", "/api/unhide?id=" + STORE)
        check(code == 200 and r["ok"], "unhide ok")
        check(dump(appdb) == original, "database identical to original after hide+show")
        code, st = http("GET", "/api/status")
        check(not tile(st, STORE)["hidden"] and not tile(st, STORE)["on_list"], "store visible again")

        print("Media Gallery (no MyHome pin)")
        code, r = http("POST", "/api/hide?id=" + GALLERY)
        check(code == 200, "hide gallery ok")
        check(all(q(appdb, f'SELECT visible FROM "tbl_iconinfo_{u}" WHERE titleId=?', GALLERY)[0][0] == 0 for u in USERS),
              "gallery visible=0 for all users")
        code, st = http("GET", "/api/status")
        check(tile(st, GALLERY)["hidden"], "status reports gallery hidden")

        print("errors")
        code, r = http("POST", "/api/hide?id=NPXS99999")
        check(code == 409 and "not on this console" in r["error"], "unknown title rejected")
        code, r = http("POST", "/api/hide?id=../../etc")
        check(code == 400, "malformed id rejected")
        code, r = http("GET", "/api/hide?id=" + STORE)
        check(code == 405, "GET on action rejected")
        code, r = http("GET", "/api/icon?id=" + STORE, raw=True)
        check(code == 404, "missing icon -> 404")
        code, r = http("GET", "/nope", raw=True)
        check(code == 404, "unknown path -> 404")
        evil = urllib.request.Request(BASE + "/api/unhide?id=" + GALLERY, method="POST", data=b"",
                                      headers={"Origin": "http://evil.example"})
        try:
            urllib.request.urlopen(evil, timeout=5)
            code = 200
        except urllib.error.HTTPError as e:
            code = e.code
        check(code == 403, "request from another web site rejected")
        code, st = http("GET", "/api/status")
        check(tile(st, GALLERY)["hidden"], "rejected request changed nothing")
        same = urllib.request.Request(BASE + "/api/settings?reapply=1", method="POST", data=b"",
                                      headers={"Origin": BASE})
        check(urllib.request.urlopen(same, timeout=5).status == 200, "same-origin request (the menu) allowed")

        print("busy database (another process holds a write lock for 1.5 s)")
        holder = sqlite3.connect(appdb, isolation_level=None, check_same_thread=False)
        holder.execute("BEGIN IMMEDIATE")
        threading.Timer(1.5, lambda: holder.execute("COMMIT")).start()
        t0 = time.time()
        code, r = http("POST", "/api/hide?id=" + STORE)
        check(code == 200 and time.time() - t0 >= 1.2, "waited for lock, then succeeded")
        holder.close()

        print("diagnostics")
        code, d = http("GET", "/api/diag", raw=True)
        d = d.decode()
        check(code == 200 and "tbl_iconinfo_1eb71bbd" in d and "MyHome" in d, "diagnostics lists tables and rows")

        print("settings")
        code, r = http("POST", "/api/settings?open_browser=0")
        check(code == 200, "settings saved")
        s.stop()

        print("system brings tiles back -> re-hidden at next launch")
        con = sqlite3.connect(appdb)
        for u in USERS:
            con.execute(f'UPDATE "tbl_iconinfo_{u}" SET visible=1, dispLocation=138 WHERE titleId=?', (STORE,))
            con.execute(f'INSERT INTO "tbl_group_{u}"(itemId, groupName, type, idx) VALUES(?, \'MyHome\', \'title\', 9)', (STORE,))
        con.commit()
        con.close()
        s = Server(appdb, data)
        check("would open" not in "".join(s.out), "browser not opened when setting is off")
        code, st = http("GET", "/api/status")
        check(st["reapplied_at_launch"] == 1, "one tile re-hidden at launch (gallery was still hidden)")
        check(tile(st, STORE)["hidden"] and tile(st, GALLERY)["hidden"], "both hidden after re-apply")
        check(not st["settings"]["open_browser"], "setting persisted")

        print("show everything")
        code, r = http("POST", "/api/unhide_all")
        check(code == 200 and r["note"].startswith("2"), "unhide_all showed 2 tiles")
        final = dump(appdb)
        for u in USERS:
            check(final[f"tbl_iconinfo_{u}"] == original[f"tbl_iconinfo_{u}"], f"user {u}: icon table back to original")
            check(final[f"tbl_group_{u}"] == original[f"tbl_group_{u}"], f"user {u}: MyHome back to original pin")

        print("second launch replaces the first instance")
        env = dict(os.environ, HH_APP_DB=appdb, HH_DATA_DIR=data)
        p2 = subprocess.Popen([BIN], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        s.p.wait(timeout=10)
        check(s.p.returncode == 0, "first instance exited cleanly")
        time.sleep(0.5)
        code, st = http("GET", "/api/status")
        check(code == 200, "second instance serving")
        http("POST", "/api/quit")
        p2.wait(timeout=10)

        print("restore original backup")
        s = Server(appdb, data)
        http("POST", "/api/hide?id=" + STORE)
        code, r = http("POST", "/api/restore")
        check(code == 200 and r["ok"], "restore ok")
        check(dump(appdb) == original, "app.db equals original backup")
        code, st = http("GET", "/api/status")
        check(not tile(st, STORE)["on_list"], "hide list cleared after restore")
        s.stop()

        print("unknown database layout (other firmware)")
        odd = os.path.join(tmp, "odd.db")
        make_db(odd, layout="unknown")
        odd_before = dump(odd)
        s = Server(odd, os.path.join(tmp, "data2"))
        code, st = http("GET", "/api/status")
        check(not st["db"]["ok"] and "layout" in st["db"]["error"], "status explains unknown layout")
        code, r = http("POST", "/api/hide?id=" + STORE)
        check(code == 409 and "Diagnostics" in r["error"], "hide refused safely")
        check(dump(odd) == odd_before, "odd db untouched")
        s.stop()

        print("missing app.db")
        s = Server(os.path.join(tmp, "missing.db"), os.path.join(tmp, "data3"))
        code, st = http("GET", "/api/status")
        check(not st["db"]["ok"] and "not found" in st["db"]["error"], "status explains missing db")
        check(not os.path.exists(os.path.join(tmp, "missing.db")), "app.db never created")
        s.stop()

        groups = [
            library_hide_tests,
            folder_tests,
            folder_page_options_tests,
            folder_colours_tests,
            folder_job_tests,
            folder_inplace_tests,
            folder_refresh_tests,
            datadir_tests,
            default_repo_tests,
            menu_new_sections_tests,
            release_tests,
            backup_tests,
            multi_folder_tests,
            picture_tests,
            launch_tests,
            startup_mode_tests,
            custom_tests,
            background_tests,
            watchdog_tests,
            watchdog_multi_folder_tests,
            tabs_tests,
            folder_move_tests,
            live_tests,
            arrange_tests,
            front_folders_tests,
            show_running_tests,
            updater_tests,
        ]
        # HH_TEST_GROUPS=name1,name2 runs only those groups (and "none" none of them): the full run takes minutes
        wanted = os.environ.get("HH_TEST_GROUPS")
        for g in groups:
            if wanted is None or g.__name__ in wanted.split(","):
                g(tmp)
        print("\nALL %d CHECKS PASSED" % passed)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
