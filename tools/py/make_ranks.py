"""Makes the rank badges from the rank art (art/ranks/NN-<rank>.webp, pictures with a transparent background, the lowest
rank first): launcher/data/ranks/<rank>.png for the launcher (lobbies, the Ranked page, the leaderboard) and
recomp/common/data/ranks/<rank>.png for the games' fights (beside the timer, standing on each player's health bar; the
package ships them once, in its ranks folder). <rank> is the ranked title as the server names it, in lower case with
dashes ("Chosen One": chosen-one).

Each badge is cut to what shows of it and fitted into a box of one shape for every rank, then made smaller until it
shows as much as the airiest badge does: a solid plaque (Learner) as large as it fits looks far bigger than one
mostly wings and flames (Demi God), so every badge ends up covering the same area of screen. The launcher's are centred
in their box, so the ranks line up in its lists. The games' keep the box's height with the badge at its foot and no
room at its sides, so a game can stand each one on its bar right against the timer.

    python tools/py/make_ranks.py [art dir]
"""
import math
import re
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
BOX = 2.4  # width : height of the box every badge fits in (the badges run 1.6 : 1 to 2.9 : 1)
LAUNCHER = ROOT / "launcher" / "data" / "ranks", 160      # 80 px on the Ranked page at 200 % display scaling
GAMES = ROOT / "recomp" / "common" / "data" / "ranks", 240  # some 45 of the frame's 480 lines, sharp at 4K


def badge(path: Path) -> Image.Image:
    art = Image.open(path).convert("RGBA")
    alpha = art.getchannel("A")
    top = max(i for i, n in enumerate(alpha.histogram()) if n)
    if top < 255:  # an encoder that stops short of opaque: solid parts solid, soft edges in proportion
        alpha = alpha.point(lambda v: min(255, round(v * 255 / top)))
        art.putalpha(alpha)
    return art.crop(alpha.point(lambda v: 255 if v > 8 else 0).getbbox())


def resized(art: Image.Image, scale: float) -> Image.Image:
    w, h = max(1, round(art.width * scale)), max(1, round(art.height * scale))
    # resampled with the colour times the alpha, so transparent pixels' colour does not bleed into the edges
    return art.convert("RGBa").resize((w, h), Image.LANCZOS).convert("RGBA")


def fit(art: Image.Image, height: int) -> float:
    """the scale that makes the badge as large as fits in the box height x BOX * height"""
    return min(height * BOX / art.width, height / art.height)


def coverage(art: Image.Image) -> float:
    """how much of it shows: its pixels weighted by their alpha"""
    return sum(i * n for i, n in enumerate(art.getchannel("A").histogram())) / 255


def main() -> int:
    art_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "art" / "ranks"
    files = sorted(art_dir.glob("[0-9][0-9]-*.webp"))
    if not files:
        print(__doc__)
        return 2
    arts = [(re.sub(r"^\d+-", "", f.stem), badge(f)) for f in files]
    # each badge as large as fits in the box, then shrunk to show what the airiest one shows (area goes with the
    # square of the size)
    shown = {name: coverage(resized(art, fit(art, GAMES[1]))) for name, art in arts}
    least = min(shown.values())
    for out, _ in (LAUNCHER, GAMES):
        out.mkdir(parents=True, exist_ok=True)
    for name, art in arts:
        even = math.sqrt(least / shown[name])
        for (out, height), boxed in ((LAUNCHER, True), (GAMES, False)):
            small = resized(art, fit(art, height) * even)
            w = round(height * BOX) if boxed else small.width
            canvas = Image.new("RGBA", (w, height), (0, 0, 0, 0))
            y = (height - small.height) // 2 if boxed else height - small.height
            canvas.paste(small, ((w - small.width) // 2, y))
            canvas.save(out / f"{name}.png", optimize=True)
        print(f"{name}: {art.width}x{art.height}, shown at {even:.2f} of its fit")
    return 0


if __name__ == "__main__":
    sys.exit(main())
