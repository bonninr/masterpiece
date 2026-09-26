# Stand-ins for GrandOrgue's built-in console images.
#
#   python tools/gen-grandorgue.py   -> resources/grandorgue/*.png
#                                       src/mp_core/GrandOrgueStockImages.h
#
# A GrandOrgue organ that does not ship its own artwork is drawn by
# GrandOrgue's automatic layout from stock images it names but does not ship:
# drawstops, pistons, nameplates, swell shoes and keys in a few styles. Those
# images are GrandOrgue's own, so they are not used here. These are drawn from
# nothing but geometry, under the names a definition asks for, at the pixel
# sizes the layout was written for -- the sizes are what make a stop land on
# its row, so they are the one thing kept.
#
# The importer writes them into the organ definition as images from a
# standard component package, which the console draws from these stand-ins
# when the package is not installed -- the same way it covers Hauptwerk's
# own standard components. The wood the layout tiles behind everything is
# Masterpiece's own, from tools/gen-wood.py, reached the same way.
import math
import os

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'resources', 'grandorgue')
HEADER = os.path.join(ROOT, 'src', 'mp_core', 'GrandOrgueStockImages.h')
SS = 4  # supersampling
# The stand-ins are found by file name among everything embedded, so theirs
# carry a prefix no other set's file names use.
PREFIX = 'GOStock-'

sizes = {}


def canvas(w, h):
    img = Image.new('RGBA', (w * SS, h * SS), (0, 0, 0, 0))
    return img, ImageDraw.Draw(img)


def save(img, w, h, name):
    img = img.resize((w, h), Image.LANCZOS)
    img.save(os.path.join(OUT, PREFIX + name + '.png'))
    sizes[name] = (w, h)


def shade(c, f):
    return tuple(max(0, min(255, int(v * f))) for v in c[:3]) + (c[3] if len(c) > 3 else 255,)


# ---- drawstops: a round knob seen from the front; drawn out, it is lit ------
KNOB_STYLES = [
    # rim, face, ring
    ((70, 40, 22), (236, 226, 200), (120, 82, 50)),     # 1 ivory on walnut
    ((24, 22, 20), (236, 226, 200), (60, 56, 50)),      # 2 ivory on ebony
    ((70, 40, 22), (180, 40, 34), (120, 82, 50)),       # 3 red face
    ((70, 40, 22), (60, 110, 60), (120, 82, 50)),       # 4 green face (read-only)
    ((150, 118, 60), (236, 226, 200), (196, 164, 96)),  # 5 ivory on brass
    ((24, 22, 20), (40, 70, 140), (60, 56, 50)),        # 6 blue face
    ((70, 40, 22), (230, 190, 70), (120, 82, 50)),      # 7 gold face
]


def drawstop(n, on):
    w = h = 65
    img, d = canvas(w, h)
    rim, face, ring = KNOB_STYLES[n - 1]
    s = SS
    cx, cy, r = 32.5 * s, 32.5 * s, 30 * s
    # shadow: a drawn knob stands out from the jamb and throws a longer one
    off = (4 if on else 1.5) * s
    d.ellipse((cx - r + off, cy - r + off, cx + r + off, cy + r + off), fill=(0, 0, 0, 110))
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=rim + (255,))
    d.ellipse((cx - r * 0.86, cy - r * 0.86, cx + r * 0.86, cy + r * 0.86), fill=ring + (255,))
    f = face if on else shade(face, 0.72)[:3]
    d.ellipse((cx - r * 0.78, cy - r * 0.78, cx + r * 0.78, cy + r * 0.78), fill=tuple(f) + (255,))
    # a soft highlight, stronger on a drawn knob
    hl = Image.new('RGBA', img.size, (0, 0, 0, 0))
    hd = ImageDraw.Draw(hl)
    hd.ellipse((cx - r * 0.62, cy - r * 0.70, cx + r * 0.20, cy - r * 0.05),
               fill=(255, 255, 255, 90 if on else 35))
    img = Image.alpha_composite(img, hl.filter(ImageFilter.GaussianBlur(3 * s)))
    save(img, w, h, 'drawstop%02d_%s' % (n, 'on' if on else 'off'))


# ---- pistons: a small round button; pressed, it lights ----------------------
PISTON_STYLES = [
    ((236, 226, 200), (140, 120, 90)),   # 1 ivory
    ((200, 60, 50), (110, 30, 25)),      # 2 red
    ((120, 120, 120), (60, 60, 60)),     # 3 grey (read-only)
    ((60, 110, 60), (30, 60, 30)),       # 4 green
    ((24, 22, 20), (90, 84, 76)),        # 5 black
]


def piston(n, on):
    w = h = 32
    img, d = canvas(w, h)
    face, rim = PISTON_STYLES[n - 1]
    s = SS
    cx, cy, r = 16 * s, 16 * s, 13 * s
    d.ellipse((cx - r + s, cy - r + 2 * s, cx + r + s, cy + r + 2 * s), fill=(0, 0, 0, 100))
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=rim + (255,))
    rr = r * (0.70 if on else 0.78)
    f = shade(face + (255,), 1.25 if on else 1.0)
    d.ellipse((cx - rr, cy - rr, cx + rr, cy + rr), fill=f)
    if on:
        glow = Image.new('RGBA', img.size, (0, 0, 0, 0))
        ImageDraw.Draw(glow).ellipse((cx - r, cy - r, cx + r, cy + r), fill=(255, 240, 180, 90))
        img = Image.alpha_composite(img, glow.filter(ImageFilter.GaussianBlur(2 * s)))
    save(img, w, h, 'piston%02d_%s' % (n, 'on' if on else 'off'))


# ---- nameplates -------------------------------------------------------------
LABEL_SIZES = [(80, 25), (80, 50), (80, 25), (160, 25), (200, 50), (80, 50), (80, 25),
               (160, 25), (80, 50), (80, 25), (160, 25), (200, 50), (400, 50), (400, 50),
               (400, 50)]
LABEL_STYLES = [
    ((236, 226, 200), (150, 118, 60)),  # ivory in brass
    ((196, 164, 96), (122, 96, 48)),    # brass
    ((30, 28, 26), (150, 118, 60)),     # black in brass
]


def label(n):
    w, h = LABEL_SIZES[n - 1]
    img, d = canvas(w, h)
    face, frame = LABEL_STYLES[(n - 1) % 3]
    s = SS
    rad = min(w, h) * s * 0.18
    d.rounded_rectangle((0, 0, w * s - 1, h * s - 1), radius=rad, fill=frame + (255,))
    b = 2.5 * s
    d.rounded_rectangle((b, b, w * s - 1 - b, h * s - 1 - b), radius=rad * 0.7, fill=face + (255,))
    save(img, w, h, 'label%02d' % n)


# ---- swell shoes: frame 0 closed, the last one fully open ------------------
SHOE_STYLES = {'A': ((58, 36, 22), (30, 20, 14)), 'B': ((24, 22, 20), (60, 56, 50)),
               'C': ((150, 118, 60), (90, 70, 36)), 'D': ((110, 70, 40), (50, 32, 20))}


def shoe(style, frame, frames):
    w, h = 46, 61
    img, d = canvas(w, h)
    top, edge = SHOE_STYLES[style]
    s = SS
    t = frame / max(1, frames - 1)          # 0 closed (toe down) .. 1 open (heel down)
    # The shoe seen from the player: its visible length shrinks and its shade
    # changes as it tilts from toe-down to heel-down.
    span = (0.30 + 0.62 * t) * h * s
    cy = h * s * 0.5
    y0, y1 = cy - span / 2, cy + span / 2
    x0, x1 = 5 * s, (w - 5) * s
    d.rounded_rectangle((x0 + 2 * s, y0 + 3 * s, x1 + 2 * s, y1 + 3 * s), radius=5 * s, fill=(0, 0, 0, 90))
    d.rounded_rectangle((x0, y0, x1, y1), radius=5 * s, fill=edge + (255,))
    f = 0.75 + 0.35 * t
    d.rounded_rectangle((x0 + 2 * s, y0 + 2 * s, x1 - 2 * s, y1 - 2 * s), radius=4 * s,
                        fill=shade(top + (255,), f))
    for i in range(1, 6):                   # the ribs of the tread
        yy = y0 + (y1 - y0) * i / 6
        d.line((x0 + 5 * s, yy, x1 - 5 * s, yy), fill=shade(top + (255,), f * 0.7), width=s)
    save(img, w, h, 'enclosure%s%02d' % (style, frame))


# ---- keys -------------------------------------------------------------------
WHITE, BLACK, WOOD = (236, 232, 218), (30, 28, 26), (176, 128, 78)


def key(w, h, colour, down, notch_left=False, notch_right=False, sharp=False):
    img, d = canvas(w, h)
    s = SS
    c = shade(colour + (255,), 0.82 if down else 1.0)
    edge = shade(colour + (255,), 0.55)
    if sharp:
        d.rectangle((0, 0, w * s - 1, h * s - 1), fill=edge)
        d.rectangle((s, 0, w * s - 1 - s, h * s - (3 if down else 4) * s), fill=c)
    else:
        d.rectangle((0, 0, w * s - 1, h * s - 1), fill=edge)
        d.rectangle((s, 0, w * s - 1 - s, h * s - (1 if down else 2) * s), fill=c)
        # where a sharp sits over the natural, nothing is drawn: the sharp
        # itself is drawn on top, and the gap keeps the join clean
        if notch_left:
            d.rectangle((0, 0, 3 * s, 19 * s), fill=(0, 0, 0, 0))
        if notch_right:
            d.rectangle(((w - 3) * s, 0, w * s, 19 * s), fill=(0, 0, 0, 0))
    return img


def keys():
    # (prefix, natural colour, sharp colour) -- GrandOrgue's manual schemes
    schemes = [('', WHITE, BLACK), ('Inverted', BLACK, WHITE), ('Wood', WOOD, BLACK),
               ('InvertedWood', BLACK, WOOD)]
    for prefix, nat, shp in schemes:
        for num in ('01', '02'):
            for down in (False, True):
                state = 'On' if down else 'Off'
                base = 'Manual%s%s%s_' % (prefix, num, state)
                for shape, nl, nr in (('C', False, True), ('D', True, True), ('E', True, False),
                                      ('Natural', False, False)):
                    save(key(12, 32, nat, down, nl, nr), 12, 32, base + shape)
                save(key(6, 19, shp, down, sharp=True), 6, 19, base + 'Sharp')
    for prefix, nat, shp in (('', WOOD, BLACK), ('Inverted', BLACK, WOOD)):
        for num, sharp_h in (('01', 18), ('02', 40)):
            for down in (False, True):
                state = 'On' if down else 'Off'
                base = 'Pedal%s%s%s_' % (prefix, num, state)
                save(key(7, 40, nat, down), 7, 40, base + 'Natural')
                save(key(7, sharp_h, shp, down, sharp=True), 7, sharp_h, base + 'Sharp')


def header():
    lines = [
        '// Generated by tools/gen-grandorgue.py -- do not edit.',
        '//',
        "// The pixel sizes of Masterpiece's stand-ins for GrandOrgue's stock console",
        '// images, by the names a definition uses. The importer lays a console out',
        '// with them; the pictures themselves are embedded in the application as',
        '// resources/grandorgue/<prefix><name>.png.',
        '#define MP_GRANDORGUE_STOCK_PREFIX "GOStock-"',
        '#pragma once',
        '',
        'namespace mp {',
        '',
        'struct GrandOrgueStockImage {',
        '  const char* name;',
        '  int width;',
        '  int height;',
        '};',
        '',
        'inline constexpr GrandOrgueStockImage kGrandOrgueStockImages[] = {',
    ]
    for name in sorted(sizes):
        w, h = sizes[name]
        lines.append('    {"%s", %d, %d},' % (name, w, h))
    lines += ['};', '', '}  // namespace mp', '']
    with open(HEADER, 'w', newline='\n') as f:
        f.write('\n'.join(lines))


def main():
    os.makedirs(OUT, exist_ok=True)
    for n in range(1, 8):
        drawstop(n, False)
        drawstop(n, True)
    for n in range(1, 6):
        piston(n, False)
        piston(n, True)
    for n in range(1, 16):
        label(n)
    for style in 'ABCD':
        for frame in range(18):
            shoe(style, frame, 18 if style == 'C' else 16)
    keys()
    header()
    print('%d images in %s' % (len(sizes), OUT))


if __name__ == '__main__':
    main()
