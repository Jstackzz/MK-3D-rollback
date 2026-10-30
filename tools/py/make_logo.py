"""Makes the launcher's logo files in launcher/data/logo from the MK3D logo art (a picture with a transparent
background): mk3d-logo.png, 320 px tall, for the header and the dialogs; icon-16.png ... icon-256.png, the whole logo
across a square, for the window icon; and mk3d.ico, the same sizes and a few more, for the executable's file icon.

    python tools/py/make_logo.py <logo art> [out dir]
"""
import sys
from pathlib import Path

from PIL import Image

LOGO_HEIGHT = 320                                     # a 76 px header logo at up to 400 % display scaling
ICON_PNGS = (16, 24, 32, 48, 64, 128, 256)            # what style::appIcon() adds
ICO_SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve().parents[2] / "launcher" / "data" / "logo"
    out.mkdir(parents=True, exist_ok=True)
    art = Image.open(sys.argv[1]).convert("RGBA")
    alpha = art.getchannel("A")
    top = max(i for i, n in enumerate(alpha.histogram()) if n)
    if top < 255:  # an encoder that stops short of opaque: solid parts solid, soft edges in proportion
        alpha = alpha.point(lambda v: min(255, round(v * 255 / top)))
        art.putalpha(alpha)
    art = art.crop(alpha.point(lambda v: 255 if v > 8 else 0).getbbox())

    logo = art.resize((round(art.width * LOGO_HEIGHT / art.height), LOGO_HEIGHT), Image.LANCZOS)
    logo.save(out / "mk3d-logo.png", optimize=True)

    def square(size: int) -> Image.Image:
        height = max(1, round(art.height * size / art.width))
        icon = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        icon.alpha_composite(art.resize((size, height), Image.LANCZOS), (0, (size - height) // 2))
        return icon

    for size in ICON_PNGS:
        square(size).save(out / f"icon-{size}.png", optimize=True)
    icons = [square(size) for size in ICO_SIZES]
    icons[-1].save(out / "mk3d.ico", sizes=[(s, s) for s in ICO_SIZES], append_images=icons[:-1])
    print(f"wrote {out}: mk3d-logo.png {logo.width}x{logo.height}, icon-*.png, mk3d.ico")
    return 0


if __name__ == "__main__":
    sys.exit(main())
