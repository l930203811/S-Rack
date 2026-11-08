# -*- coding: utf-8 -*-
"""Generate the S-Rack application icon.

Design language (2026-11-08 redesign):
  * deep-blue rounded square + neon rim (kept from the original brand palette)
  * a large luminous "S" monogram with a white -> cyan vertical gradient
  * a thin rack ring + horizontal rails / corner screws (the "Rack" cue)
  * the LED segment spectrum (green -> yellow -> red) along the bottom,
    so the icon still reads as an audio plugin

Outputs (into Assets/):
  S-Rack-icon.png        1024x1024 master artwork (CMake ICON_BIG)
  S-Rack-icon-small.png  64x64 simplified artwork (CMake ICON_SMALL)
  S-Rack.ico             multi-size Windows icon (16..256)

Run:  python Assets/generate_icon.py
"""

import os
from PIL import Image, ImageDraw, ImageFilter, ImageChops, ImageFont

OUT = os.path.dirname(os.path.abspath(__file__))
S = 1024          # logical canvas size
SS = 2            # supersampling for smooth edges
W = S * SS


def p(v):
    return int(round(v * SS))


def rgba(hexstr, a=255):
    h = hexstr.lstrip('#')
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), a)


def new():
    return Image.new('RGBA', (W, W), (0, 0, 0, 0))


def lerp(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(4))


def vgrad(top, bottom, width=W, height=W):
    im = Image.new('RGBA', (width, height))
    d = ImageDraw.Draw(im)
    for y in range(height):
        t = (y / max(1, height - 1)) ** 1.25
        d.line([(0, y), (width, y)], fill=lerp(top, bottom, t))
    return im


def round_mask(inset=36, radius=215):
    m = Image.new('L', (W, W), 0)
    ImageDraw.Draw(m).rounded_rectangle(
        [p(inset), p(inset), W - p(inset), W - p(inset)],
        radius=p(radius), fill=255)
    return m


def bold_font(size_px):
    for path in (r'C:\Windows\Fonts\seguibl.ttf',
                 r'C:\Windows\Fonts\segoeuib.ttf',
                 r'C:\Windows\Fonts\arialbd.ttf'):
        if os.path.exists(path):
            return ImageFont.truetype(path, size_px)
    return ImageFont.load_default()


def make_background():
    mask = round_mask()
    glow = new()
    ImageDraw.Draw(glow).rounded_rectangle(
        [p(36), p(36), W - p(36), W - p(36)],
        radius=p(215), fill=rgba('#2E9CFF', 120))
    glow = glow.filter(ImageFilter.GaussianBlur(p(32)))
    base = Image.alpha_composite(new(), glow)

    grad = vgrad(rgba('#17327D'), rgba('#050A20'))
    grad.putalpha(mask)
    base = Image.alpha_composite(base, grad)

    hl = new()
    d = ImageDraw.Draw(hl)
    d.ellipse([p(-200), p(-420), p(720), p(500)], fill=rgba('#5C9CFF', 70))
    d.ellipse([p(300), p(700), p(1060), p(1200)], fill=rgba('#2A6BFF', 62))
    hl = hl.filter(ImageFilter.GaussianBlur(p(60)))
    hl.putalpha(ImageChops.multiply(hl.getchannel('A'), mask))
    return Image.alpha_composite(base, hl), mask


def glyph_mask(char, size_logical, cx, cy):
    """Render a bold glyph into an L mask with its ink box centred on (cx, cy)."""
    f = bold_font(p(size_logical))
    m = Image.new('L', (W, W), 0)
    d = ImageDraw.Draw(m)
    bbox = f.getbbox(char)
    gw, gh = bbox[2] - bbox[0], bbox[3] - bbox[1]
    x = p(cx) - gw / 2.0 - bbox[0]
    y = p(cy) - gh / 2.0 - bbox[1]
    d.text((int(round(x)), int(round(y))), char, font=f, fill=255)
    return m


def make_rack_furniture():
    """Thin ring + side rails + corner screws: the 'Rack' hardware cue."""
    art = new()
    d = ImageDraw.Draw(art)

    cx, cy = p(512), p(452)
    r = p(300)
    # soft halo behind the monogram
    halo = new()
    ImageDraw.Draw(halo).ellipse([cx - r, cy - r, cx + r, cy + r],
                                fill=rgba('#2E9CFF', 44))
    halo = halo.filter(ImageFilter.GaussianBlur(p(46)))
    art = Image.alpha_composite(art, halo)

    d = ImageDraw.Draw(art)
    # concentric rack ring
    for rr, col, wd in [(300, rgba('#8AB8FF', 92), 3),
                        (286, rgba('#2E9CFF', 46), 2)]:
        d.ellipse([cx - p(rr), cy - p(rr), cx + p(rr), cy + p(rr)],
                  outline=col, width=p(wd))

    # short horizontal rails on both sides of the ring
    for y in (300, 372, 444, 516, 588):
        for x0, x1 in ((92, 232), (792, 932)):
            d.rounded_rectangle([p(x0), p(y), p(x1), p(y + 5)],
                                radius=p(2), fill=rgba('#7FB6FF', 58))
        for x in (150, 874):
            d.ellipse([p(x - 5), p(y - 3), p(x + 5), p(y + 8)],
                      fill=rgba('#BFE0FF', 95))

    # corner screws
    for sx, sy in ((96, 96), (928, 96), (96, 928), (928, 928)):
        d.ellipse([p(sx - 9), p(sy - 9), p(sx + 9), p(sy + 9)],
                  fill=rgba('#0A1730', 190), outline=rgba('#8AB8FF', 105), width=p(2))
        d.line([p(sx - 4), p(sy), p(sx + 4), p(sy)],
               fill=rgba('#8AB8FF', 120), width=p(2))
    return art


def make_s_monogram():
    mask = glyph_mask('S', 600, 512, 448)

    # outer glow
    glow = Image.new('RGBA', (W, W), rgba('#2E9CFF', 0))
    glow.putalpha(mask.filter(ImageFilter.GaussianBlur(p(30))).point(
        lambda v: int(v * 0.62)))
    glow = Image.alpha_composite(new(), glow)

    # dark outline for separation from the ring
    outline = mask.filter(ImageFilter.MaxFilter(p(18) + 1))   # MaxFilter 只接受奇数尺寸
    rim = ImageChops.subtract(outline, mask)
    rim_img = Image.new('RGBA', (W, W), rgba('#062046', 205))
    rim_img.putalpha(rim)

    # vertical white -> cyan fill
    fill = vgrad(rgba('#FFFFFF'), rgba('#7FD2FF'))
    fill.putalpha(mask)

    # upper gloss inside the letter
    gloss = new()
    ImageDraw.Draw(gloss).ellipse([p(250), p(170), p(780), p(420)],
                                 fill=rgba('#FFFFFF', 60))
    gloss = gloss.filter(ImageFilter.GaussianBlur(p(34)))
    gloss.putalpha(ImageChops.multiply(gloss.getchannel('A'), mask))

    out = glow
    out = Image.alpha_composite(out, rim_img)
    out = Image.alpha_composite(out, fill)
    out = Image.alpha_composite(out, gloss)
    return out


def spectrum_color(t):
    if t < 0.52:
        return lerp(rgba('#33D433'), rgba('#EED500'), t / 0.52)
    return lerp(rgba('#EED500'), rgba('#E4390E'), (t - 0.52) / 0.48)


def make_spectrum():
    bars = [0.26, 0.42, 0.62, 0.82, 0.98, 0.74, 0.56, 0.38, 0.24]
    spec = new()
    d = ImageDraw.Draw(spec)
    base_y, max_h = 902, 150
    bar_w, gap = 44, 20
    seg_h, seg_gap = 14, 7
    total = len(bars) * bar_w + (len(bars) - 1) * gap
    x0 = (S - total) / 2.0

    for j, h in enumerate(bars):
        x = x0 + j * (bar_w + gap)
        target = h * max_h
        i = 0
        while (i + 1) * seg_h + i * seg_gap <= target:
            y0 = base_y - i * (seg_h + seg_gap)
            y1 = y0 - seg_h
            t = (base_y - (y0 + y1) / 2.0) / 150.0
            d.rounded_rectangle([p(x), p(y1), p(x + bar_w), p(y0 - 3)],
                                radius=p(4), fill=spectrum_color(t))
            i += 1
        top = base_y - i * (seg_h + seg_gap)
        d.rounded_rectangle([p(x), p(top - 17), p(x + bar_w), p(top - 9)],
                            radius=p(3), fill=rgba('#FFF8D8', 255))
    # rack panel divider
    d.line([(p(120), p(918)), (p(904), p(918))],
           fill=rgba('#66B0FF', 150), width=p(2))
    return spec


def make_big():
    base, mask = make_background()
    art = make_rack_furniture()
    art = Image.alpha_composite(art, make_s_monogram())
    art = Image.alpha_composite(art, make_spectrum())
    art.putalpha(ImageChops.multiply(art.getchannel('A'), mask))
    out = Image.alpha_composite(base, art)
    ImageDraw.Draw(out).rounded_rectangle(
        [p(36), p(36), W - p(36), W - p(36)],
        radius=p(215), outline=rgba('#8AB8FF', 95), width=p(3))
    return out.resize((S, S), Image.LANCZOS)


def make_small():
    """64px glyph: bold S + rim + three LED bars, legible down to 16px."""
    n, ss, w = 64, 8, 64 * 8
    q = lambda v: int(round(v * ss))
    cv = lambda c, a=255: rgba(c, a)
    img = Image.new('RGBA', (w, w), (0, 0, 0, 0))
    m = Image.new('L', (w, w), 0)
    ImageDraw.Draw(m).rounded_rectangle([q(2), q(2), w - q(2), w - q(2)],
                                        radius=q(14), fill=255)
    grad = vgrad(cv('#203A8E'), cv('#050A20'), w, w)
    grad.putalpha(m)
    img = Image.alpha_composite(img, grad)

    f = bold_font(q(52))
    gm = Image.new('L', (w, w), 0)
    gd = ImageDraw.Draw(gm)
    bbox = f.getbbox('S')
    gw, gh = bbox[2] - bbox[0], bbox[3] - bbox[1]
    gd.text((int(round(q(32) - gw / 2.0 - bbox[0])),
             int(round(q(27) - gh / 2.0 - bbox[1]))), 'S', font=f, fill=255)

    glow = Image.new('RGBA', (w, w), cv('#2E9CFF', 0))
    glow.putalpha(gm.filter(ImageFilter.GaussianBlur(q(5))).point(
        lambda v: int(v * 0.55)))
    letter = vgrad(cv('#FFFFFF'), cv('#7FD2FF'), w, w)
    letter.putalpha(gm)
    art = Image.alpha_composite(glow, letter)

    d = ImageDraw.Draw(art)
    for j, (h, c) in enumerate(zip((5, 9, 6), ('#33D433', '#EED500', '#E4390E'))):
        x = 20 + j * 9
        d.rounded_rectangle([q(x), q(58 - h), q(x + 6), q(58)],
                            radius=q(1), fill=cv(c))
    art.putalpha(ImageChops.multiply(art.getchannel('A'), m))
    img = Image.alpha_composite(img, art)
    ImageDraw.Draw(img).rounded_rectangle([q(2), q(2), w - q(2), w - q(2)],
                                          radius=q(14), outline=cv('#8AB8FF', 95),
                                          width=q(1))
    return img.resize((n, n), Image.LANCZOS)


def main():
    big = make_big()
    big.save(os.path.join(OUT, 'S-Rack-icon.png'))
    small = make_small()
    small.save(os.path.join(OUT, 'S-Rack-icon-small.png'))
    big.save(os.path.join(OUT, 'S-Rack.ico'), format='ICO',
             sizes=[(16, 16), (24, 24), (32, 32), (48, 48),
                    (64, 64), (128, 128), (256, 256)])
    for f in ('S-Rack-icon.png', 'S-Rack-icon-small.png', 'S-Rack.ico'):
        print(' ', f, os.path.getsize(os.path.join(OUT, f)), 'bytes')


if __name__ == '__main__':
    main()
