# Seamless wood tiles for console pages whose own backgrounds are missing.
#
#   python tools/gen-wood.py            -> resources/wood/wood-{h,v}-{01..08}.png
#
# Some sets paint their control pages with textures from "Hauptwerk Standard
# Components", a package installed with Hauptwerk and not shipped with the
# set. Masterpiece cannot include those, so it draws its own: these tiles,
# generated here from nothing but noise, and embedded in the build.
#
# Each tile repeats without a seam: every noise layer is periodic over the
# tile. The grain is a set of growth rings bent by low-frequency turbulence,
# with fine streaks along it and a little pore noise across it. Eight tones,
# from pale oak to dark walnut; "h" runs the grain across, "v" down.
import os
import numpy as np
from PIL import Image

SIZE = 256
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'resources', 'wood')

# (light, dark) RGB pairs: the ring's pale early wood and its darker late wood.
TONES = [
    ((214, 176, 128), (168, 122, 76)),   # pale oak
    ((201, 154, 102), (146, 98, 57)),    # oak
    ((187, 128, 83), (128, 76, 43)),     # cherry
    ((170, 108, 70), (112, 60, 34)),     # mahogany
    ((158, 112, 72), (98, 62, 36)),      # teak
    ((140, 96, 62), (84, 52, 31)),       # walnut
    ((120, 80, 52), (70, 43, 26)),       # dark walnut
    ((192, 150, 104), (136, 96, 60)),    # elm
]


def periodic_noise(rng, size, cells_y, cells_x):
    # Value noise on a lattice that wraps, smoothly interpolated: periodic
    # over the tile by construction. Separate cell counts across and along
    # the grain make it anisotropic, which is what makes it read as wood.
    lattice = rng.random((cells_y, cells_x))

    def axis(cells):
        t = np.arange(SIZE) * cells / SIZE
        i0 = np.floor(t).astype(int)
        f = t - i0
        return i0 % cells, (i0 + 1) % cells, f * f * (3 - 2 * f)

    y0, y1, fy = axis(cells_y)
    x0, x1, fx = axis(cells_x)
    y0, y1, fy = y0[:, None], y1[:, None], fy[:, None]
    a = lattice[y0, x0] * (1 - fx) + lattice[y0, x1] * fx
    b = lattice[y1, x0] * (1 - fx) + lattice[y1, x1] * fx
    return a * (1 - fy) + b * fy


def fbm(rng, cells_y, cells_x, octaves):
    total, amp, norm = np.zeros((SIZE, SIZE)), 1.0, 0.0
    for o in range(octaves):
        total += amp * periodic_noise(rng, SIZE, cells_y * 2 ** o, cells_x * 2 ** o)
        norm += amp
        amp *= 0.5
    return total / norm


def tile(seed, tone):
    rng = np.random.default_rng(seed)
    y = np.arange(SIZE)[:, None] / SIZE
    # Rings run along x (grain horizontal): a whole number per tile, spaced
    # unevenly by a slow wobble across, and bent only gently along the grain.
    rings = 7 + (seed % 5)
    spacing = fbm(rng, 3, 1, 2)                       # uneven ring widths
    bend = fbm(rng, 4, 1, 3)                          # slow drift along the grain
    phase = (y * rings + spacing * 1.6 + bend * 0.9) % 1.0
    ring = np.abs(np.sin(np.pi * phase)) ** 0.45
    late = np.clip((phase - 0.82) / 0.18, 0, 1) ** 2   # the thin dark late wood
    streak = fbm(rng, 48, 2, 3)                       # long fine streaks along the grain
    figure = fbm(rng, 6, 3, 2)                        # broad light and dark patches
    pores = fbm(rng, 96, 24, 1)
    mix = np.clip(0.50 * ring + 0.22 * streak + 0.18 * figure + 0.10 * pores - 0.35 * late, 0, 1)
    light, dark = np.array(tone[0], float), np.array(tone[1], float)
    rgb = dark[None, None, :] + (light - dark)[None, None, :] * mix[..., None]
    return Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8), 'RGB')


def main():
    os.makedirs(OUT, exist_ok=True)
    for n, tone in enumerate(TONES, start=1):
        horizontal = tile(1000 + n, tone)
        horizontal.save(os.path.join(OUT, 'wood-h-%02d.png' % n), optimize=True)
        horizontal.transpose(Image.Transpose.ROTATE_90).save(
            os.path.join(OUT, 'wood-v-%02d.png' % n), optimize=True)
    print('wrote %d tiles to %s' % (2 * len(TONES), OUT))


if __name__ == '__main__':
    main()
