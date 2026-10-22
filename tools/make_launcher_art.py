"""The launcher's art, from the game's files: fighter portraits (the character select's), the Smash Ball
(tools/make_icon.py) and the game's title font (system/font/font_hira.brfnt) as a glyph sheet with its widths.
   python tools/make_launcher_art.py [disc files dir, default Disc/DATA/files]
Writes launcher/art/: fighter_<name>.png, font.png and font.txt ("<code> <x> <glyph width> <left> <advance>" per
line after a "cell <w> <h> baseline <b>" line)."""
import os
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import brawl_assets  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "launcher", "art")

# MenSelchrFaceB<number>.brres: the character select's portraits.
FIGHTERS = {"mario": "000", "sonic": "460", "kirby": "050", "pikachu": "070", "fox": "060", "samus": "030",
            "pit": "220", "metaknight": "210", "ike": "330", "snake": "450"}


def main():
    files = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "Disc", "DATA", "files")
    os.makedirs(OUT, exist_ok=True)
    for name, number in FIGHTERS.items():
        path = os.path.join(files, "menu", "common", "char_bust_tex", f"MenSelchrFaceB{number}.brres")
        tex_name, at, data = brawl_assets.textures(open(path, "rb").read())[0]
        brawl_assets.tex0_image(data, at).save(os.path.join(OUT, f"fighter_{name}.png"))

    font = brawl_assets.Font(open(os.path.join(files, "system", "font", "font_hira.brfnt"), "rb").read())
    codes = [c for c in range(32, 127) if c in font.cmap] + [0x2019, 0x00E9]
    sheet = Image.new("RGBA", (font.cell_w * len(codes), font.cell_h), (0, 0, 0, 0))
    lines = [f"cell {font.cell_w} {font.cell_h} baseline {font.baseline}"]
    for i, code in enumerate(codes):
        img, (left, glyph_w, advance) = font.glyph(chr(code))
        alpha = img.getchannel("A")
        white = Image.new("RGBA", img.size, (255, 255, 255, 255))
        white.putalpha(alpha)
        sheet.alpha_composite(white, (i * font.cell_w, 0))
        lines.append(f"{code} {i * font.cell_w} {glyph_w} {left} {advance}")
    sheet.save(os.path.join(OUT, "font.png"))
    open(os.path.join(OUT, "font.txt"), "w", newline="\n").write("\n".join(lines) + "\n")

    ball = os.path.join(ROOT, "launcher", "smash_ball.png")
    if os.path.exists(ball):
        Image.open(ball).resize((256, 256), Image.LANCZOS).save(os.path.join(OUT, "smash_ball.png"))
    print("launcher art written to", OUT)


if __name__ == "__main__":
    main()
