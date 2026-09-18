"""Draw assets/revenant.ico.

The mark is a spectrum peak with a dimmer copy of itself standing behind it,
offset in time. That is the product in one shape: a signal, and the same signal
returning from a capture it was never being watched in.

Drawn rather than painted. The shapes are three polygons, so a generator gets
exact colours, true alpha and hard edges, none of which a diffusion model would
give reliably. It also means every size in the container is rendered at its own
resolution from the same geometry instead of being downscaled from one 256px
image, which is the difference between a readable notification-area icon and a
smudge.

Run: python scripts/make_icon.py
"""

from pathlib import Path

from PIL import Image, ImageDraw

# Specter Point red, the accent every Locke Werks repository uses.
CRIMSON = (214, 38, 42, 255)
# The ghost. Dark enough to read as absent rather than as a second signal.
MAROON = (112, 28, 32, 255)

# Rendered at this multiple of the target size and then reduced, which is what
# produces clean edges at 16 and 24 pixels. Drawing directly at 16 gives
# staircased diagonals; drawing at 128 and reducing does not.
SUPERSAMPLE = 16

SIZES = (16, 24, 32, 48, 64, 128, 256)


def draw_mark(size: int, target: int) -> Image.Image:
    """Render the mark at one edge length, with a transparent background.

    `size` is the supersampled canvas; `target` is the pixel size this frame
    will end up at. The geometry reads the target because an icon is not one
    drawing scaled seven ways. At 256 pixels a hairline baseline is elegant; at
    16 it falls below one pixel and dissolves into grey, and the ghost merges
    with the peak it is supposed to stand behind. Small frames therefore get a
    thicker baseline and a wider separation. This is the same reason the
    container holds seven real images instead of one.
    """
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(canvas)

    small = target <= 32

    def x(fraction: float) -> float:
        return fraction * size

    def y(fraction: float) -> float:
        return fraction * size

    # The baseline the peaks stand on. Squared ends, no rounding: a rounded cap
    # reads as a pill at 16 pixels and softens the whole mark.
    baseline_top = 0.735 if small else 0.760
    baseline_bottom = 0.865 if small else 0.838
    baseline_inset = 0.055 if small else 0.085
    draw.rectangle(
        [x(baseline_inset), y(baseline_top), x(1.0 - baseline_inset), y(baseline_bottom)],
        fill=CRIMSON,
    )

    # The ghost, drawn first so the live peak overlaps it. Shorter and set back
    # to the left, so the pair reads as one mark with a shadow rather than as
    # two peaks competing. Pushed further left on small frames so the gap
    # survives as whole pixels.
    ghost_shift = 0.075 if small else 0.0
    draw.polygon(
        [
            (x(0.205 - ghost_shift), y(baseline_top)),
            (x(0.395 - ghost_shift), y(0.255)),
            (x(0.585 - ghost_shift), y(baseline_top)),
        ],
        fill=MAROON,
    )

    # The live peak. Narrow enough to read as a carrier rather than a mountain,
    # and tall enough to fill the frame's vertical extent.
    draw.polygon(
        [
            (x(0.345), y(baseline_top)),
            (x(0.560), y(0.105)),
            (x(0.775), y(baseline_top)),
        ],
        fill=CRIMSON,
    )

    return canvas


def render(size: int) -> Image.Image:
    large = draw_mark(size * SUPERSAMPLE, size)
    return large.resize((size, size), Image.LANCZOS)


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    assets = root / "assets"
    assets.mkdir(exist_ok=True)

    frames = [render(size) for size in SIZES]

    # Pillow writes a true multi-image container when handed the full set and
    # told which sizes to keep. The largest frame carries the others.
    target = assets / "revenant.ico"
    frames[-1].save(
        target,
        format="ICO",
        sizes=[(s, s) for s in SIZES],
        append_images=frames[:-1],
    )

    # A 512px PNG for anywhere that cannot take an .ico: the web page, a
    # markdown README on a host that will not render one, a Linux .desktop.
    render(512).save(assets / "revenant.png", format="PNG")

    print(f"wrote {target} with sizes {', '.join(str(s) for s in SIZES)}")
    print(f"wrote {assets / 'revenant.png'} at 512x512")


if __name__ == "__main__":
    main()
