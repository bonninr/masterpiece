#!/usr/bin/env python3
"""Tiny Masterpiece control-API client (stdlib only).

Reads MP_BASE (default http://127.0.0.1:8080/api/v1) and MP_TOKEN from the
environment, or --base URL / --token TOKEN anywhere on the line. Changes send
Authorization: Bearer <token>.

Usage:
  mp_api.py identify
  mp_api.py program                          # all program properties
  mp_api.py program-set <index> <value>
  mp_api.py wait-organ [timeout_s]           # until organState == 2
  mp_api.py list     <type> [--all]          # count + describe a type
  mp_api.py describe <type> <index>
  mp_api.py names    <type> [regex]          # index, name, role, division for every object
  mp_api.py get      <type> <index>
  mp_api.py dump     <type>                  # all values (bitfield for 0/1 types)
  mp_api.py set      <type> <index> <value>  # a number, or 'toggle' for a switch
  mp_api.py press    <type> <index>
  mp_api.py events   [type,type,...]         # stream server-sent changes
  mp_api.py raw GET|PUT|POST <path> [json]   # escape hatch

<type> may be singular (switch) or the plural the API uses (switches).
The live list of types is in `identify`; the full spec is GET /api/v1/openapi.json.
"""
import json, os, re, signal, sys, time, urllib.request, urllib.error

# Quiet when piped into head and the like.
if hasattr(signal, "SIGPIPE"):
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)

PLURAL = {
    "switch": "switches", "control": "controls", "division": "divisions",
    "keyboard": "keyboards", "general": "generals", "divisional": "divisionals",
    "eqpreset": "eqpresets", "temperament": "temperaments", "command": "commands",
    "rank": "ranks", "note": "notes", "pipe": "pipes", "enclosure": "enclosures",
    "tremulant": "tremulants", "windchest": "windchests", "route": "routes",
    "pair": "pairs", "ranklevel": "ranklevels", "brightness": "brightnesses",
    "bass": "basses", "pan": "pans", "width": "widths", "mute": "mutes",
    "solo": "solos", "organ": "organs",
}

OPTS = {"base": None, "token": None, "all": False}

def parse_args(argv):
    rest, i = [], 0
    while i < len(argv):
        a = argv[i]
        if a in ("--base", "--token") and i + 1 < len(argv):
            OPTS[a[2:]] = argv[i + 1]; i += 2; continue
        if a == "--all":
            OPTS["all"] = True; i += 1; continue
        rest.append(a); i += 1
    return rest

def plural(t):
    return PLURAL.get(t, t)

def base():
    return (OPTS["base"] or os.environ.get("MP_BASE") or "http://127.0.0.1:8080/api/v1").rstrip("/")

def token():
    return OPTS["token"] or os.environ.get("MP_TOKEN", "")

def call(method, path, body=None, stream=False):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(base() + path, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    if method != "GET" and token():
        req.add_header("Authorization", "Bearer " + token())
    try:
        r = urllib.request.urlopen(req, timeout=None if stream else 15)
    except urllib.error.HTTPError as e:
        print(f"HTTP {e.code}: {e.read().decode(errors='replace')}", file=sys.stderr)
        sys.exit(2)
    except urllib.error.URLError as e:
        print(f"cannot reach {base()}: {e.reason}", file=sys.stderr)
        sys.exit(3)
    if stream:
        for line in r:
            sys.stdout.write(line.decode(errors="replace")); sys.stdout.flush()
        return None
    txt = r.read().decode(errors="replace")
    try:
        return json.loads(txt)
    except ValueError:
        return txt

def show(x):
    print(x if isinstance(x, str) else json.dumps(x, indent=1, ensure_ascii=False))

def main():
    a = parse_args(sys.argv[1:])
    if not a:
        print(__doc__); sys.exit(1)
    cmd = a[0]
    if cmd == "identify":
        show(call("GET", ""))
    elif cmd == "program":
        show(call("GET", "/program"))
    elif cmd == "program-set":
        show(call("PUT", f"/program/{a[1]}", {"value": int(a[2])}))
    elif cmd == "wait-organ":
        limit = float(a[1]) if len(a) > 1 else 600
        t0 = time.time()
        while time.time() - t0 < limit:
            p = call("GET", "/program")
            if p.get("organState") == 2:
                print("loaded"); return
            time.sleep(1)
        print("timeout", file=sys.stderr); sys.exit(4)
    elif cmd in ("list", "describe"):
        t = plural(a[1])
        if cmd == "describe" and len(a) > 2:
            show(call("GET", f"/{t}/{a[2]}"))
        else:
            show(call("GET", f"/{t}" + ("?all=1" if OPTS["all"] else "")))
    elif cmd == "names":
        t = plural(a[1]); pat = re.compile(a[2], re.I) if len(a) > 2 else None
        n = call("GET", f"/{t}?all=1").get("count", 0)
        for i in range(n):
            d = call("GET", f"/{t}/{i}")
            if pat and not pat.search(d.get("name", "")):
                continue
            print(f"{i}\t{d.get('name','')}\t{d.get('role','')}\t{d.get('division','')}")
    elif cmd == "get":
        show(call("GET", f"/{plural(a[1])}/{a[2]}/value"))
    elif cmd == "dump":
        show(call("GET", f"/{plural(a[1])}/values"))
    elif cmd == "set":
        v = a[3]
        show(call("PUT", f"/{plural(a[1])}/{a[2]}/value", {"value": v if v == "toggle" else int(v)}))
    elif cmd == "press":
        show(call("POST", f"/{plural(a[1])}/{a[2]}/press", {}))
    elif cmd == "events":
        call("GET", f"/events?types={a[1] if len(a) > 1 else 'switch,control,program'}", stream=True)
    elif cmd == "raw":
        show(call(a[1], a[2], json.loads(a[3]) if len(a) > 3 else None))
    else:
        print(__doc__); sys.exit(1)

if __name__ == "__main__":
    main()
