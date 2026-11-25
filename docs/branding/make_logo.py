"""Amplitude's logo (python docs/branding/make_logo.py): The Bozos' clown as a beat-matcher, drawn with BozoEngine's logo
tool (BozoEngine/tools/branding/make_logo.py: the clown, the PS2 BIOS blue, the boot towers, the wordmark) plus this
game's variant: a pair of studio headphones over the head (a neon band, big round ear cups with glowing rings) and,
behind the clown, a three-lane track running to the horizon with glowing note gems on its lanes and an equaliser along
the banner. Generic shapes only, no game art.

Writes docs/branding/amp_icon_1024.png, amp_icon_256.png, amp_banner.png and the window's icon runtime/app/amplitude.ico
(the usual Windows sizes). The font: Exo 2 (SIL OFL 1.1), BOZO_LOGO_FONT=<Exo2[wght].ttf>.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'BozoEngine', 'tools', 'branding'))
import make_logo as logo  # noqa: E402  (BozoEngine's tool)
from PIL import Image, ImageDraw, ImageFilter  # noqa: E402

SS = logo.SS
# Amplitude's colours: a neon club palette over the team's PS2 blue. The lanes' gems take the clown's own console-shape
# colours (cross blue, triangle green, circle red), the headphones are dark plastic with cyan and magenta rings.
CYAN = (64, 232, 255)
MAGENTA = (255, 72, 196)
PLASTIC = (28, 26, 48)
PLASTIC_LIT = (70, 66, 112)
TRACK = (14, 20, 64)
GEMS = (logo.CROSS, logo.TRIANGLE, logo.CIRCLE)


def track(canvas, cx, horizon, bottom, half_bottom, half_top, gems=True, alpha=1.0):
    """A three-lane highway in perspective: from a narrow end at the horizon to a wide end at the bottom, neon edges,
    lane dividers, cross bars that crowd toward the horizon, and a gem on each lane."""
    layer = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    a = lambda v: int(v * alpha)  # noqa: E731

    def at(u, t):
        """The point at lane coordinate u (-1 left edge .. 1 right edge) and depth t (0 horizon .. 1 bottom)."""
        y = horizon + (bottom - horizon) * t
        half = half_top + (half_bottom - half_top) * t
        return cx + u * half, y
    d.polygon([at(-1, 0), at(1, 0), at(1, 1), at(-1, 1)], fill=TRACK + (a(235),))
    # Cross bars, spaced like a floor seen in perspective (closer together near the horizon).
    for k in range(1, 12):
        t = (k / 12) ** 1.8
        d.line([at(-1, t), at(1, t)], fill=CYAN + (a(40 + 50 * t),), width=max(1, int(SS * (1 + 3 * t))))
    w = max(2, int((half_bottom - half_top) * 0.02))
    for u in (-1 / 3, 1 / 3):
        d.line([at(u, 0), at(u, 1)], fill=logo.ACCENT + (a(150),), width=w)
    edges = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    de = ImageDraw.Draw(edges)
    for u, col in ((-1, CYAN), (1, MAGENTA)):
        de.line([at(u, 0), at(u, 1)], fill=col + (a(255),), width=w * 2)
    layer.alpha_composite(edges.filter(ImageFilter.GaussianBlur(w * 2.5)))
    layer.alpha_composite(edges)
    canvas.alpha_composite(layer)
    if not gems:
        return
    # Gems: one per lane, at different depths, flat diamonds sized by their distance.
    for lane, (t, col) in enumerate(zip((0.42, 0.7, 0.25), GEMS)):
        u = -2 / 3 + lane * 2 / 3
        x, y = at(u, t)
        half = half_top + (half_bottom - half_top) * t
        gw, gh = half * 0.2, half * 0.1

        def gem(dd, x=x, y=y, gw=gw, gh=gh, col=col):
            dd.polygon([(x - gw, y), (x, y - gh), (x + gw, y), (x, y + gh)], fill=col + (a(255),))
        logo.glow_shape(canvas, gem, gw * 0.5, 1.2 * alpha)
        dd = ImageDraw.Draw(canvas)
        dd.polygon([(x - gw * 0.45, y), (x, y - gh * 0.45), (x + gw * 0.45, y), (x, y + gh * 0.45)],
                   fill=(255, 255, 255, a(170)))


def equaliser(canvas, x0, x1, base, height, bars, seed=5, alpha=1.0):
    """A row of equaliser bars rising from base, cyan to magenta, each made of stacked segments."""
    layer = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    step = (x1 - x0) / bars
    rnd = seed
    for i in range(bars):
        rnd = (rnd * 1103515245 + 12345) & 0x7fffffff
        level = 0.25 + 0.75 * abs(math.sin(i * 0.55 + seed)) * (0.6 + 0.4 * (rnd % 1000) / 1000)
        k = i / max(1, bars - 1)
        col = tuple(int(CYAN[j] + (MAGENTA[j] - CYAN[j]) * k) for j in range(3))
        segs = max(1, int(level * 10))
        sh = height / 10
        for s in range(segs):
            y1 = base - s * sh
            d.rectangle([x0 + i * step + step * 0.18, y1 - sh * 0.72, x0 + (i + 1) * step - step * 0.18, y1],
                        fill=col + (int((120 + 100 * s / 10) * alpha),))
    canvas.alpha_composite(layer.filter(ImageFilter.GaussianBlur(step * 0.35)))
    canvas.alpha_composite(layer)


def headphones_back(canvas, cx, cy, r):
    """Behind the head: the headband, an arc of dark plastic over the top of the head with a neon stripe along it.
    Drawn where BozoEngine's clown draws fire hair."""
    layer = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    box = [cx - r * 1.18, cy - r * 1.3, cx + r * 1.18, cy + r * 1.06]
    d.arc(box, 180, 360, fill=PLASTIC + (255,), width=int(r * 0.24))
    d.arc(box, 180, 360, fill=PLASTIC_LIT + (255,), width=int(r * 0.08))
    inner = [box[0] + r * 0.09, box[1] + r * 0.09, box[2] - r * 0.09, box[3] - r * 0.09]
    glow = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    ImageDraw.Draw(glow).arc(inner, 200, 340, fill=CYAN + (255,), width=max(2, int(r * 0.045)))
    layer.alpha_composite(glow.filter(ImageFilter.GaussianBlur(r * 0.05)))
    layer.alpha_composite(glow)
    canvas.alpha_composite(layer)


def headphones_front(canvas, cx, cy, r):
    """Over the sides of the head: the two ear cups, dark rounded pads with a glowing ring (cyan left, magenta right)
    and a small speaker grille, and the yokes joining them to the band."""
    d = ImageDraw.Draw(canvas)
    for side, col in ((-1, CYAN), (1, MAGENTA)):
        x, y = cx + side * r * 1.02, cy - r * 0.08
        cw, ch = r * 0.34, r * 0.46
        d.rounded_rectangle([x - r * 0.07, y - ch - r * 0.42, x + r * 0.07, y - ch + r * 0.05], radius=r * 0.04,
                            fill=PLASTIC_LIT + (255,))

        def ring(dd, x=x, y=y, cw=cw, ch=ch, col=col):
            dd.ellipse([x - cw, y - ch, x + cw, y + ch], fill=col + (255,))
        logo.glow_shape(canvas, ring, r * 0.12, 1.3)
        d = ImageDraw.Draw(canvas)
        k = 0.8
        d.ellipse([x - cw * k, y - ch * k, x + cw * k, y + ch * k], fill=PLASTIC + (255,))
        d.ellipse([x - cw * 0.5, y - ch * 0.5, x + cw * 0.5, y + ch * 0.5], fill=PLASTIC_LIT + (255,))
        for gy in (-0.22, 0.0, 0.22):
            d.line([(x - cw * 0.28, y + ch * gy), (x + cw * 0.28, y + ch * gy)], fill=PLASTIC + (255,),
                   width=max(2, int(r * 0.03)))
        d.ellipse([x - cw * 0.62, y - ch * 0.66, x - cw * 0.3, y - ch * 0.38], fill=(255, 255, 255, 70))


_engine_clown = logo.clown


def dj_clown(canvas, cx, cy, r, fire=False, **_):
    """BozoEngine's clown with fire=True (no hat, no hair puffs; its fire_hair hook draws the headband), then the cups."""
    _engine_clown(canvas, cx, cy, r, fire=True)
    headphones_front(canvas, cx, cy, r)


logo.fire_hair = headphones_back
logo.clown = dj_clown


def icon(px):
    W = px * SS
    bg = logo.background(W, W).convert('RGBA')
    bg.alpha_composite(logo.towers(W * 0.3, W, W, W * 1.02, 9, dim=0.6))
    track(bg, W * 0.5, W * 0.6, W * 1.0, W * 0.78, W * 0.16)
    dj_clown(bg, W * 0.5, W * 0.43, W * 0.24)
    bg = logo.scanlines(bg, SS * 3)
    mask = Image.new('L', (W, W), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, W - 1, W - 1], radius=W * 0.18, fill=255)
    bg.putalpha(mask)
    return bg.resize((px, px), Image.LANCZOS)


def banner():
    w, h = 2000, 600
    img = logo.wordmark(w, h, False, title='AMPLITUDE', subtitle='NATIVE PC PORT  ·  THE BOZOS', fire=True,
                        title_size=0.33)
    # An equaliser along the bottom right and a faint track behind the clown, laid over the finished wordmark.
    big = img.resize((w * SS // 2, h * SS // 2), Image.LANCZOS).convert('RGBA')
    W, H = big.size
    over = Image.new('RGBA', big.size, (0, 0, 0, 0))
    equaliser(over, W * 0.33, W * 0.98, H * 0.99, H * 0.2, 40, alpha=0.55)
    big.alpha_composite(over)
    return big.resize((w, h), Image.LANCZOS)


if __name__ == '__main__':
    big = icon(1024)
    big.save(os.path.join(HERE, 'amp_icon_1024.png'))
    icon(256).save(os.path.join(HERE, 'amp_icon_256.png'))
    app = os.path.join(ROOT, 'runtime', 'app')
    os.makedirs(app, exist_ok=True)
    big.save(os.path.join(app, 'amplitude.ico'), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128),
                                                        (256, 256)])
    banner().save(os.path.join(HERE, 'amp_banner.png'))
    print('amplitude done ->', HERE)
