#!/usr/bin/env python3
"""Reports pinned dependencies that fall behind upstream or carry advisories.

Every FetchContent_Declare in the CMake files names a GitHub repository and a
GIT_TAG: a release tag or a commit, written out or held in a variable. This
script reads them all, so a new pin needs no change here, and asks GitHub,
through the gh command, for each repository's latest release and its
published security advisories.

  python tools/check-pins.py            report, Markdown, on standard output
  python tools/check-pins.py --check    as the report, and exit 1 if a pin is
                                        behind or affected by an advisory
  python tools/check-pins.py --json     the findings as JSON
  python tools/check-pins.py --issue    as the report, and open, update or close
                                        the tracking issue (dependency-pins.yml)

A tag pin is behind when the latest release has a higher version. A commit pin
is behind when it lacks the latest release, or, in a repository without
releases, commits on the default branch. An advisory counts when its patched
version is newer than the pin; for a commit pin, when the pin lacks the
release that patched it.

Needs gh (https://cli.github.com), signed in: gh auth login.
"""
import argparse
import functools
import json
import os
import re
import subprocess
import sys
import urllib.parse
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TITLE = "Pinned dependencies behind upstream"
LABEL = "dependencies"
MARKER = re.compile(r"<!-- check-pins: ([^>]*) -->")
SEVERITY = {"critical": 0, "high": 1, "medium": 2, "moderate": 2, "low": 3}

DECLARE = re.compile(r"\bFetchContent_Declare\s*\(([^)]*)\)", re.S)
SET = re.compile(r'^[ \t]*set\(\s*([A-Za-z0-9_]+)\s+(?:"([^"]*)"|([^\s)"]+))', re.M)
VAR = re.compile(r"\$\{([A-Za-z0-9_]+)\}")
TOKEN = re.compile(r'"[^"]*"|[^\s"]+')
GITHUB = re.compile(r"github\.com[/:]([^/\s]+)/([^/\s]+?)(?:\.git)?/?$")
SHA = re.compile(r"^[0-9a-f]{7,40}$")


class GhError(Exception):
    pass


# ---- the pins, from the CMake files ----

def cmake_files():
    """Tracked CMake files, the top-level CMakeLists.txt first."""
    try:
        out = subprocess.run(["git", "ls-files", "-z", "--", "*CMakeLists.txt", "*.cmake"],
                             cwd=ROOT, capture_output=True, text=True, check=True).stdout
        names = [n for n in out.split("\0") if n]
    except (OSError, subprocess.CalledProcessError):
        names = []
    if not names:  # not a git checkout
        for d, dirs, files in os.walk(ROOT):
            if "CMakeCache.txt" in files:  # a build folder
                dirs[:] = []
                continue
            dirs[:] = [x for x in dirs if not x.startswith(".")]
            names += [Path(d, f).relative_to(ROOT).as_posix() for f in files
                      if f == "CMakeLists.txt" or f.endswith(".cmake")]
    names = [n for n in names if (ROOT / n).is_file()]
    return sorted(names, key=lambda n: (n.count("/"), n))


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def read_pins():
    """(pins, skipped): pins with a GitHub repository and a GIT_TAG."""
    texts = {}
    variables = {}  # name -> (value, file, line); the first set() wins
    for name in cmake_files():
        text = (ROOT / name).read_text(encoding="utf-8", errors="replace")
        text = re.sub(r"#[^\n]*", lambda m: " " * len(m.group()), text)  # comments, same offsets
        texts[name] = text
        for m in SET.finditer(text):
            value = m.group(2) if m.group(2) is not None else m.group(3)
            variables.setdefault(m.group(1), (value, name, line_of(text, m.start())))

    def resolve(value, where):
        """The value with ${...} replaced, and where its literal is written."""
        for _ in range(10):
            m = VAR.fullmatch(value)
            if m and m.group(1) in variables:
                value, f, line = variables[m.group(1)]
                where = (m.group(1), f, line)
                continue
            value = VAR.sub(lambda v: variables.get(v.group(1), ("",))[0], value)
            break
        return value, where

    pins, skipped, seen = [], [], set()
    for name, text in texts.items():
        for m in DECLARE.finditer(text):
            body = m.group(1)
            tokens = [(t.group().strip('"'), m.start(1) + t.start()) for t in TOKEN.finditer(body)]
            if not tokens:
                continue
            dep = tokens[0][0]
            args = {}
            for i, (tok, pos) in enumerate(tokens[1:-1], 1):
                if tok in ("GIT_REPOSITORY", "GIT_TAG", "URL"):
                    args[tok] = (tokens[i + 1][0], line_of(text, pos))
            if "GIT_REPOSITORY" not in args or "GIT_TAG" not in args:
                skipped.append((dep, f"{name}:{line_of(text, m.start())}", "no GIT_REPOSITORY and GIT_TAG"))
                continue
            url, _ = resolve(args["GIT_REPOSITORY"][0], None)
            tag, where = resolve(args["GIT_TAG"][0], (None, name, args["GIT_TAG"][1]))
            gm = GITHUB.search(url)
            if not gm:
                skipped.append((dep, f"{name}:{args['GIT_REPOSITORY'][1]}", f"not on GitHub ({url})"))
                continue
            repo = f"{gm.group(1)}/{gm.group(2)}"
            if (repo, tag) in seen:
                continue
            seen.add((repo, tag))
            pins.append({
                "name": gm.group(2), "repo": repo, "pinned": tag,
                "kind": "commit" if SHA.match(tag) else "tag",
                "variable": where[0], "where": f"{where[1]}:{where[2]}",
            })
    return pins, skipped


# ---- GitHub ----

def gh_get(path, paginate=False):
    """A GET through gh api: the parsed JSON, or None for a 404."""
    cmd = ["gh", "api", "-H", "Accept: application/vnd.github+json", path]
    if paginate:
        cmd.insert(2, "--paginate")
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8")
    except OSError:
        raise GhError("needs the gh command, https://cli.github.com, signed in (gh auth login)")
    if p.returncode != 0:
        if "HTTP 404" in p.stderr:
            return None
        raise GhError(f"gh api {path}: {p.stderr.strip()}")
    if not paginate:
        return json.loads(p.stdout)
    # Pages come one after another: [...][...]
    items, pos, dec = [], 0, json.JSONDecoder()
    out = p.stdout.strip()
    while pos < len(out):
        value, pos = dec.raw_decode(out, pos)
        items += value if isinstance(value, list) else [value]
        while pos < len(out) and out[pos].isspace():
            pos += 1
    return items


def q(ref):
    return urllib.parse.quote(ref, safe="")


def day(iso):
    return (iso or "")[:10]


def version(text):
    m = re.search(r"\d+(?:\.\d+)*", text or "")
    return tuple(int(x) for x in m.group().split(".")) if m else None


def older(a, b):
    n = max(len(a), len(b))
    return a + (0,) * (n - len(a)) < b + (0,) * (n - len(b))


@functools.lru_cache(maxsize=None)
def contains(repo, ref, commit):
    """Whether commit has ref in its history; None if ref is unknown."""
    c = gh_get(f"repos/{repo}/compare/{q(ref)}...{commit}")
    return None if c is None else c["status"] in ("ahead", "identical")


def release_tag(repo, wanted, cache):
    """The repository's tag for a version such as "1.2.0", or None."""
    if repo not in cache:
        cache[repo] = [t["name"] for t in gh_get(f"repos/{repo}/tags?per_page=100", paginate=True) or []]
    v = version(wanted)
    for name in cache[repo]:
        if v and version(name) == v and not re.search(r"(?i)rc|alpha|beta|pre", name):
            return name
    return None


def check(pin):
    repo, pinned = pin["repo"], pin["pinned"]
    latest = gh_get(f"repos/{repo}/releases/latest")
    pin["latest"] = latest and {"tag": latest["tag_name"], "date": day(latest["published_at"]),
                                "url": latest["html_url"]}
    pin["behind"] = False
    pin["detail"] = ""

    if pin["kind"] == "tag":
        rel = gh_get(f"repos/{repo}/releases/tags/{q(pinned)}")
        if rel:
            pin["date"] = day(rel["published_at"])
        else:
            c = gh_get(f"repos/{repo}/commits/{q(pinned)}")
            pin["date"] = day(c["commit"]["committer"]["date"]) if c else ""
        if not latest:
            pin["detail"] = "no releases published"
        elif latest["tag_name"] != pinned:
            vp, vl = version(pinned), version(latest["tag_name"])
            pin["behind"] = older(vp, vl) if vp and vl else pin["date"] < pin["latest"]["date"]
    else:
        c = gh_get(f"repos/{repo}/commits/{pinned}")
        pin["date"] = day(c["commit"]["committer"]["date"]) if c else ""
        if latest:
            pin["behind"] = contains(repo, latest["tag_name"], pinned) is False
            if pin["behind"]:
                pin["detail"] = "not in the pin"
        else:
            branch = gh_get(f"repos/{repo}")["default_branch"]
            head = gh_get(f"repos/{repo}/commits/{q(branch)}")
            cmp = gh_get(f"repos/{repo}/compare/{pinned}...{q(branch)}")
            pin["latest"] = {"tag": f"{branch} {head['sha'][:7]}",
                             "date": day(head["commit"]["committer"]["date"]), "url": head["html_url"]}
            if cmp and cmp["ahead_by"]:
                pin["behind"] = True
                pin["detail"] = f"{cmp['ahead_by']} commits on {branch} since the pin"

    tags = {}
    pin["advisories"] = []
    for a in gh_get(f"repos/{repo}/security-advisories?per_page=100", paginate=True) or []:
        if a.get("state") != "published" or a.get("withdrawn_at"):
            continue
        hit = None
        for v in a.get("vulnerabilities") or [{}]:
            patched = (v.get("patched_versions") or "").strip()
            vp = version(patched)
            if not vp:
                affected = day(a.get("published_at")) >= pin["date"]  # no fix named: by date
            elif pin["kind"] == "tag":
                mine = version(pinned)
                affected = older(mine, vp) if mine else day(a.get("published_at")) >= pin["date"]
            else:
                tag = release_tag(repo, patched, tags)
                has = contains(repo, tag, pinned) if tag else None
                affected = (not has) if has is not None else day(a.get("published_at")) >= pin["date"]
            if affected:
                hit = v
                break
        if hit is not None:
            pin["advisories"].append({
                "id": a["ghsa_id"], "cve": a.get("cve_id"), "url": a["html_url"],
                "severity": a.get("severity") or "unknown", "summary": (a.get("summary") or "").strip(),
                "published": day(a.get("published_at")),
                "vulnerable": (hit.get("vulnerable_version_range") or "").strip(),
                "patched": (hit.get("patched_versions") or "").strip(),
            })
    pin["advisories"].sort(key=lambda x: x["published"], reverse=True)
    pin["advisories"].sort(key=lambda x: SEVERITY.get(x["severity"], 9))  # stable: newest first in each
    return pin


# ---- the report ----

def short(pin):
    return pin["pinned"][:7] if pin["kind"] == "commit" else pin["pinned"]


def findings(pins):
    """Key -> one line, for each thing that wants attention."""
    out = {}
    for p in pins:
        if p["behind"]:
            out[f"{p['repo']}@{p['latest']['tag']}"] = f"{p['name']} {p['latest']['tag']} ({p['latest']['date']})"
        for a in p["advisories"]:
            out[f"{p['repo']}#{a['id']}"] = f"{p['name']}: {a['severity']} [{a['id']}]({a['url']}), {a['summary']}"
    return out


def cell(text):
    return (text or "").replace("|", "\\|").replace("\n", " ")


def report(pins, skipped, today):
    flagged = [p for p in pins if p["behind"] or p["advisories"]]
    current = [p for p in pins if p not in flagged]
    lines = [f"{len(pins)} pins in the CMake files, checked {today} (`tools/check-pins.py`): "
             f"{len(flagged)} behind or with advisories.", ""]
    if flagged:
        lines += ["| Dependency | Pinned | Set in | Latest | Advisories |", "|---|---|---|---|---|"]
        for p in flagged:
            var = f"`{p['variable']}`, " if p["variable"] else ""
            latest = []
            if p["latest"]:
                latest.append(f"[{p['latest']['tag']}]({p['latest']['url']}) ({p['latest']['date']})")
            if p["detail"]:
                latest.append(p["detail"])
            if not p["behind"]:
                latest.append("pin current")
            lines.append(f"| [{p['name']}](https://github.com/{p['repo']}) "
                         f"| {short(p)} ({p['date'] or 'date unknown'}) | {var}`{p['where']}` "
                         f"| {', '.join(latest)} | {len(p['advisories']) or '–'} |")
        for p in flagged:
            if not p["advisories"]:
                continue
            lines += ["", f"### {p['name']} {short(p)}: advisories fixed after the pin", "",
                      "| Severity | Advisory | Summary | Vulnerable | Patched |", "|---|---|---|---|---|"]
            for a in p["advisories"]:
                cve = f"<br>{a['cve']}" if a["cve"] else ""
                lines.append(f"| {a['severity']} | [{a['id']}]({a['url']}){cve} | {cell(a['summary'])} "
                             f"| {cell(a['vulnerable']) or '–'} | {cell(a['patched']) or 'no release yet'} |")
        lines.append("")
    if current:
        lines.append("Current: " + ", ".join(f"{p['name']} {short(p)}" for p in current) + ".")
    for dep, where, why in skipped:
        lines.append(f"Not checked: {dep} (`{where}`), {why}.")
    return "\n".join(lines).rstrip() + "\n"


# ---- the tracking issue ----

def gh_run(args, body=None):
    p = subprocess.run(["gh"] + args, input=body, capture_output=True, text=True, encoding="utf-8")
    if p.returncode != 0:
        raise GhError(f"gh {' '.join(args[:2])}: {p.stderr.strip()}")
    return p.stdout


def update_issue(repo, body, found, today):
    """One open issue with TITLE and LABEL: opened, edited in place, or closed."""
    keys = " ".join(sorted(found))
    body += f"\n<!-- check-pins: {keys} -->\n"
    try:
        gh_run(["label", "create", LABEL, "--repo", repo, "--color", "0366d6",
                "--description", "Pinned dependencies and their updates"])
    except GhError as e:
        if "already exists" not in str(e):
            raise
    issues = json.loads(gh_run(["issue", "list", "--repo", repo, "--label", LABEL, "--state", "open",
                                "--json", "number,title,body", "--limit", "100"]))
    issue = next((i for i in issues if i["title"] == TITLE), None)
    if not found:
        if issue:
            gh_run(["issue", "close", str(issue["number"]), "--repo", repo,
                    "--comment", f"Every pin current, checked {today}."])
            print(f"Closed #{issue['number']}: every pin current.", file=sys.stderr)
        else:
            print("Every pin current; no issue open.", file=sys.stderr)
        return
    if issue is None:
        url = gh_run(["issue", "create", "--repo", repo, "--title", TITLE, "--label", LABEL,
                      "--body-file", "-"], body).strip()
        print(f"Opened {url}", file=sys.stderr)
        return
    n = str(issue["number"])
    if (issue["body"] or "").strip() != body.strip():
        gh_run(["issue", "edit", n, "--repo", repo, "--body-file", "-"], body)
    m = MARKER.search(issue["body"] or "")
    new = [k for k in sorted(found) if k not in (m.group(1).split() if m else [])]
    if new:
        gh_run(["issue", "comment", n, "--repo", repo, "--body-file", "-"],
               "New since the last check:\n\n" + "\n".join(f"- {found[k]}" for k in new) + "\n")
    print(f"Updated #{n}" + (f", {len(new)} new" if new else ""), file=sys.stderr)


def this_repo():
    repo = os.environ.get("GH_REPO") or os.environ.get("GITHUB_REPOSITORY")
    if repo:
        return repo
    url = subprocess.run(["git", "remote", "get-url", "origin"], cwd=ROOT,
                         capture_output=True, text=True).stdout.strip()
    m = GITHUB.search(url)
    if not m:
        raise GhError("no GitHub repository for the issue: pass --repo owner/name")
    return f"{m.group(1)}/{m.group(2)}"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true", help="exit 1 if a pin is behind or has advisories")
    ap.add_argument("--json", action="store_true", help="print the findings as JSON")
    ap.add_argument("--issue", action="store_true", help="open, update or close the tracking issue")
    ap.add_argument("--repo", help="repository of the issue (default: GH_REPO, or the origin remote)")
    args = ap.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    today = datetime.now(timezone.utc).date().isoformat()
    try:
        pins, skipped = read_pins()
        pins = [check(p) for p in pins]
        found = findings(pins)
        body = report(pins, skipped, today)
        if args.json:
            print(json.dumps({"checked": today, "pins": pins,
                              "skipped": [dict(zip(("name", "where", "why"), s)) for s in skipped]}, indent=2))
        else:
            print(body, end="")
        if args.issue:
            update_issue(args.repo or this_repo(), body, found, today)
    except GhError as e:
        print(f"check-pins: {e}", file=sys.stderr)
        return 2
    return 1 if args.check and found else 0


if __name__ == "__main__":
    sys.exit(main())
