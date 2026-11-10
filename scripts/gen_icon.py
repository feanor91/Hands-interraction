"""Genere installer/hands.ico (icone n 6 : pastille sombre, cube filaire, point d'etat orange/bleu). Usage : python scripts/gen_icon.py"""
from PIL import Image, ImageDraw
import os

def render(px):
    k = 8
    S = px * k
    u = S / 32.0
    im = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([1.5 * u, 1.5 * u, 30.5 * u, 30.5 * u], radius=7 * u, fill=(23, 32, 46, 255))
    cube = (108, 192, 255)
    hexa = [(16, 6.5), (24.5, 11), (24.5, 21), (16, 25.5), (7.5, 21), (7.5, 11)]
    lay = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    ld = ImageDraw.Draw(lay)
    w = int(1.8 * u)
    for a, b in [((7.5, 11), (16, 15.5)), ((16, 15.5), (24.5, 11)), ((16, 15.5), (16, 25.5))]:
        ld.line([(a[0] * u, a[1] * u), (b[0] * u, b[1] * u)], fill=cube + (153,), width=w)
    im = Image.alpha_composite(im, lay)
    d = ImageDraw.Draw(im)
    pts = [(x * u, y * u) for x, y in hexa]
    d.line(pts + [pts[0]], fill=cube + (255,), width=w, joint='curve')
    for x, y in pts:
        d.ellipse([x - w / 2, y - w / 2, x + w / 2, y + w / 2], fill=cube + (255,))
    d.ellipse([(16 - 3.2) * u, (15.5 - 3.2) * u, (16 + 3.2) * u, (15.5 + 3.2) * u], fill=(46, 160, 67, 255))
    return im.resize((px, px), Image.LANCZOS)

if __name__ == '__main__':
    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
    big = render(256)
    big.save('installer/hands.ico', sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (48, 48), (64, 64), (256, 256)])
    big.save('build/icon_preview.png')
    print('ok')
