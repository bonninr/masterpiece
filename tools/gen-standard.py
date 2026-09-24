# Stand-ins for the gauges, pedals and list items of "Hauptwerk Standard
# Components".
#
#   python tools/gen-standard.py        -> resources/standard/*.png
#
# Sets built with Hauptwerk's custom-organ template draw their Keyboards,
# Wind, Enclosures and Controls pages from that package, which is installed
# with Hauptwerk and not shipped with the set: wind-pressure dials with a
# needle in 81 positions, a 12-stage expression shoe, list items and small
# buttons. Without them the needles and shoes that the set wires to its
# continuous controls have nothing to draw, and the labels written for them
# float over bare wood.
#
# These are drawn here from nothing but geometry, under the same file names as
# the originals (with .png), and embedded in the build. The console looks them
# up by name when the real package is not installed. Sizes are read off how
# the sets place them: a needle sits 33,44 into its dial, labels centre on
# x=66 of it and occupy its top 26 pixels.
import math
import os

from PIL import Image, ImageDraw, ImageFont

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   'resources', 'standard')
SS = 4  # supersampling, for edges without a jagged staircase

BRASS = (196, 164, 96, 255)
BRASS_DARK = (122, 96, 48, 255)
FACE = (238, 230, 208, 255)
INK = (40, 34, 28, 255)
PLAQUE = (43, 33, 24, 255)


def canvas(w, h):
    img = Image.new('RGBA', (w * SS, h * SS), (0, 0, 0, 0))
    return img, ImageDraw.Draw(img)


def save(img, w, h, name):
    img.resize((w, h), Image.LANCZOS).save(os.path.join(OUT, name + '.png'))


def font(px):
    for f in ('DejaVuSans.ttf', 'arial.ttf', 'Arial.ttf'):
        try:
            return ImageFont.truetype(f, px * SS)
        except OSError:
            pass
    return ImageFont.load_default()


# --- dials -----------------------------------------------------------------
# A dark plaque carrying the set's two label lines on top, and a brass-rimmed
# face below. The needle pivots at the face's centre.
DIAL_W, DIAL_H = 132, 118
PIVOT = (66, 77)
FACE_R = 36
SWEEP = 270.0  # degrees, 0 at the bottom-left stop


def angle_for(fraction):
    # 0 points down-left, 1 down-right, through straight up.
    return math.radians(-135.0 + SWEEP * fraction)


def dial(name, scale):
    img, d = canvas(DIAL_W, DIAL_H)
    s = SS
    d.rounded_rectangle([0, 0, DIAL_W * s - 1, DIAL_H * s - 1], radius=6 * s, fill=PLAQUE)
    cx, cy = PIVOT[0] * s, PIVOT[1] * s
    r = FACE_R * s
    d.ellipse([cx - r - 4 * s, cy - r - 4 * s, cx + r + 4 * s, cy + r + 4 * s], fill=BRASS_DARK)
    d.ellipse([cx - r - 3 * s, cy - r - 3 * s, cx + r + 3 * s, cy + r + 3 * s], fill=BRASS)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=FACE)
    ticks = 20 if scale else 10
    for i in range(ticks + 1):
        a = angle_for(i / ticks)
        major = scale and i % 5 == 0 or not scale and i % 5 == 0
        inner = r - (8 if major else 4) * s
        outer = r - 2 * s
        d.line([cx + inner * math.sin(a), cy - inner * math.cos(a),
                cx + outer * math.sin(a), cy - outer * math.cos(a)],
               fill=INK, width=(2 if major else 1) * s)
        if scale and major:
            f = font(7)
            txt = str(i)
            tr = r - 15 * s
            tx, ty = cx + tr * math.sin(a), cy - tr * math.cos(a)
            box = d.textbbox((0, 0), txt, font=f)
            d.text((tx - (box[2] - box[0]) / 2, ty - (box[3] - box[1]) / 2 - box[1]),
                   txt, font=f, fill=INK)
    if not scale:
        # A red arc near the empty end: a reservoir running low.
        d.arc([cx - r + 3 * s, cy - r + 3 * s, cx + r - 3 * s, cy + r - 3 * s],
              start=90 + 45, end=90 + 45 + 40, fill=(170, 40, 30, 255), width=3 * s)
    save(img, DIAL_W, DIAL_H, name)


# --- needles ---------------------------------------------------------------
# 81 frames, 0.00 to 20.00 in quarters: the set maps its control's 0..127 onto
# them. Transparent except for the needle, whose image centre is the pivot.
NEEDLE = 66


def needle(value):
    img, d = canvas(NEEDLE, NEEDLE)
    s = SS
    c = NEEDLE / 2 * s
    a = angle_for(value / 20.0)
    tip = (FACE_R - 5) * s
    tail = 7 * s
    side = 2.2 * s
    tx, ty = c + tip * math.sin(a), c - tip * math.cos(a)
    bx, by = c - tail * math.sin(a), c + tail * math.cos(a)
    px, py = math.cos(a) * side, math.sin(a) * side
    d.polygon([(tx, ty), (c + px, c + py), (bx, by), (c - px, c - py)], fill=(150, 26, 20, 255))
    d.ellipse([c - 4 * s, c - 4 * s, c + 4 * s, c + 4 * s], fill=BRASS_DARK)
    d.ellipse([c - 2 * s, c - 2 * s, c + 2 * s, c + 2 * s], fill=BRASS)
    save(img, NEEDLE, NEEDLE, 'WindDialNeedle%05.2f' % value)


# --- expression shoe -------------------------------------------------------
# 44x120, stage 1 closed (toe raised: the shoe seen at its full length) to
# stage 12 open (toe down: foreshortened). Heel at the bottom.
PED_W, PED_H = 44, 120


def pedal(stage):
    img, d = canvas(PED_W, PED_H)
    s = SS
    t = (stage - 1) / 11.0
    length = (PED_H - 6) * (1.0 - 0.45 * t)
    top = (PED_H - 3) * s - length * s
    bottom = (PED_H - 3) * s
    inset = 5 * s * (1.0 - t)  # the raised toe narrows in perspective
    d.rectangle([2 * s, bottom - 6 * s, (PED_W - 2) * s, bottom + 2 * s], fill=(30, 26, 22, 255))
    shade = int(70 + 40 * (1.0 - t))
    body = (shade + 10, shade, shade - 12, 255)
    d.polygon([(3 * s + inset, top), ((PED_W - 3) * s - inset, top),
               ((PED_W - 3) * s, bottom), (3 * s, bottom)], fill=body, outline=(20, 18, 16, 255))
    # Rubber ridges across the tread.
    n = 9
    for i in range(1, n):
        y = top + (bottom - top) * i / n
        f = (y - top) / max(1.0, bottom - top)
        x0 = 3 * s + inset * (1.0 - f) + 3 * s
        x1 = (PED_W - 3) * s - inset * (1.0 - f) - 3 * s
        d.line([x0, y, x1, y], fill=(shade - 30, shade - 34, shade - 40, 255), width=2 * s)
    save(img, PED_W, PED_H, 'ExpressionPedalLargeStage%02d' % stage)


# --- list items and buttons ------------------------------------------------
def bevel(w, h, fill, light, dark, radius, name):
    img, d = canvas(w, h)
    s = SS
    d.rounded_rectangle([0, 0, w * s - 1, h * s - 1], radius=radius * s, fill=dark)
    d.rounded_rectangle([0, 0, w * s - 1 - s, h * s - 1 - s], radius=radius * s, fill=light)
    d.rounded_rectangle([s, s, w * s - 1 - s, h * s - 1 - s], radius=radius * s, fill=fill)
    save(img, w, h, name)


def main():
    os.makedirs(OUT, exist_ok=True)
    dial('WindPressureDialGauge', scale=True)
    dial('WindReserveGauge', scale=False)
    for q in range(81):
        needle(q * 0.25)
    for stage in range(1, 13):
        pedal(stage)
    # Off is pale; on is warm gold. The set writes black labels on both.
    bevel(152, 14, (226, 220, 204, 255), (250, 246, 236, 255), (120, 110, 94, 255), 2,
          'CustomOrgan_VisualAppearanceCode01_ListItemLight')
    bevel(152, 14, (232, 196, 110, 255), (250, 226, 160, 255), (120, 92, 40, 255), 2,
          'CustomOrgan_VisualAppearanceCode01_ListItemDark')
    # The heading strip is tiled under light lettering.
    img = Image.new('RGBA', (16, 14), (38, 30, 23, 255))
    ImageDraw.Draw(img).line([0, 13, 15, 13], fill=(18, 14, 10, 255))
    img.save(os.path.join(OUT, 'CustomOrgan_VisualAppearanceCode01_ListItemBlack.png'))
    bevel(15, 15, (222, 216, 200, 255), (250, 246, 236, 255), (110, 100, 84, 255), 3,
          'CustomOrgan_VisualAppearanceCode01_ButtonOut')
    bevel(15, 15, (226, 186, 98, 255), (120, 92, 40, 255), (250, 226, 160, 255), 3,
          'CustomOrgan_VisualAppearanceCode01_ButtonIn')


if __name__ == '__main__':
    main()
