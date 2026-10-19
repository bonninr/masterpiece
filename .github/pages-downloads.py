"""Point the site's downloads at the latest release's files.

Four things, each time the page is published (release.yml publishes it again
after every release):

- A link written as <a data-asset="windows-setup.exe" href=".../releases/latest">
  is given the address of masterpiece-<version>-windows-setup.exe on the latest
  release, so a click on the page downloads the file.
- A short address per platform, masterpieceorgan.com/download/windows and the
  others in SHORT, is written as a page that sends the browser on to the file.
- The page's structured data is given the release's version.
- version.txt holds that version, for the program's once-a-day update check.

Release files carry their version in the name, so no fixed address reaches
them. A file the release does not have keeps its link to the release page, and
so does everything when the release cannot be read: the site is published
either way.

    python3 .github/pages-downloads.py site
"""
import html as markup
import json
import re
import subprocess
import sys
from pathlib import Path

LATEST = "https://github.com/bonninr/masterpiece/releases/latest"

# Short address -> (the file's name after masterpiece-<version>-, its platform).
SHORT = {
    "windows": ("windows-setup.exe", "Windows"),
    "windows-zip": ("windows.zip", "Windows"),
    "macos": ("macos-arm64.zip", "macOS, Apple silicon"),
    "macos-intel": ("macos-x86_64.zip", "macOS, Intel"),
    "linux": ("linux-x86_64.flatpak", "Linux"),
    "linux-deb": ("linux-amd64.deb", "Linux"),
    "linux-tar": ("linux-x86_64.tar.gz", "Linux"),
    "raspberry-pi": ("linux-arm64.deb", "Raspberry Pi, 64-bit"),
    "raspberry-pi-32": ("linux-armhf.deb", "Raspberry Pi, 32-bit"),
    "android": ("android.apk", "Android"),
    "ios": ("ios.ipa", "iPhone and iPad"),
}

STUB = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="robots" content="noindex">
<title>Masterpiece for {what}</title>
<meta http-equiv="refresh" content="0; url={url}">
<style>
  body {{ margin: 0; min-height: 100vh; display: grid; place-items: center; background: #15171c;
         color: #ece9e3; font: 17px/1.6 -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Arial, sans-serif; }}
  main {{ max-width: 520px; padding: 0 16px; }}
  h1 {{ margin: 0 0 8px; font-size: 28px; font-weight: 600; }}
  p {{ margin: 8px 0; color: #b3b0a8; }}
  a {{ color: #c8a97e; text-decoration: none; }}
</style>
</head>
<body>
<main>
  <h1>Masterpiece {version}for {what}.</h1>
  <p>The download starts by itself. If it does not, <a href="{url}">download {file}</a>.</p>
  <p><a href="/">Back to Masterpiece</a> &middot; <a href="{latest}">Release notes</a></p>
</main>
</body>
</html>
"""


def main(site):
    page = site / "index.html"
    text = page.read_text(encoding="utf-8")
    try:
        out = subprocess.run(["gh", "release", "view", "--json", "tagName,assets"],
                             capture_output=True, text=True, check=True).stdout
        release = json.loads(out)
        version = release["tagName"].lstrip("v")
        files = {a["name"]: a["url"] for a in release["assets"]}
        print(f"latest release {release['tagName']}, {len(files)} files")
    except (OSError, subprocess.CalledProcessError, ValueError, KeyError) as e:
        print(f"the latest release cannot be read ({e}); downloads stay on the release page")
        version, files = "", {}

    def url_of(asset):
        return files.get(f"masterpiece-{version}-{asset}")

    def link(tag):
        a = tag.group(0)
        asset = re.search(r'data-asset="([^"]+)"', a).group(1)
        url = url_of(asset)
        print(f"  link {asset}: {url or 'not on this release, left on the release page'}")
        return re.sub(r'href="[^"]*"', f'href="{url}"', a, count=1) if url else a

    text = re.sub(r'<a\b[^>]*\bdata-asset="[^"]+"[^>]*>', link, text)
    # The structured data names the version that the links lead to.
    if version:
        text = re.sub(r'"softwareVersion": "[^"]*"', f'"softwareVersion": "{version}"', text)
    page.write_text(text, encoding="utf-8", newline="")
    # The number the program's once-a-day check reads (src/mp_ui/UpdateCheck),
    # and nothing else: a few bytes. Left out when the release cannot be read,
    # so a failed lookup never tells anyone an older version is the newest.
    if version:
        (site / "version.txt").write_text(version + "\n", encoding="utf-8", newline="")
        print(f"  version.txt: {version}")

    for slug, (asset, what) in SHORT.items():
        url = url_of(asset) or LATEST
        stub = STUB.format(what=markup.escape(what), url=markup.escape(url),
                           version=markup.escape(version + " ") if version else "",
                           file=markup.escape(url.rsplit("/", 1)[-1] if url != LATEST else "it from the release page"),
                           latest=LATEST)
        out = site / "download" / slug / "index.html"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(stub, encoding="utf-8", newline="")
        print(f"  /download/{slug}/ -> {url}")


main(Path(sys.argv[1]))
