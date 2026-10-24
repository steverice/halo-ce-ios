#!/usr/bin/env python3
"""Draw the VISR app icon as SVG and render it into the asset catalogs.

The icon is vector art: a dark backdrop with a faint grid, and a mark on top in
two layers (middle and front), so visionOS and tvOS can float the layers apart.
Several concepts are drawn; `--concept` picks the one that goes into the
catalogs. The default, `visor-helmet-gold`, is traced from the game's Master
Chief helmet (see `port/ios/ICON.md`); the others are original.

    python3 tools/visr_icon.py --catalogs                   # render the default concept into port/ios/Assets*.xcassets
    python3 tools/visr_icon.py --concept reticle --catalogs # render another concept
    python3 tools/visr_icon.py --previews DIR               # every concept's layers and mockups, and the comparison sheets

Needs `rsvg-convert` (librsvg) and `magick` (ImageMagick) on PATH.
"""
import argparse
import json
import math
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IOS = ROOT / "port/ios"
DEFAULT_CONCEPT = "visor-helmet-gold"

CYAN = "#3fd8ff"
ICE = "#e9fbff"
AMBER = "#ffb547"
GLOW = """<filter id="glow" x="-30%" y="-30%" width="160%" height="160%">
  <feGaussianBlur stdDeviation="{blur}" result="b"/>
  <feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge>
</filter>"""


def svg(width, height, body, blur=10):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
            f'viewBox="0 0 {width} {height}"><defs>{GLOW.format(blur=blur)}</defs>{body}</svg>')


def place(width, height, scale, x=None, y=None):
    """a transform that puts the 1024-unit mark, centered on its origin, at (x, y) at this scale"""
    x = width / 2 if x is None else x
    y = height / 2 if y is None else y
    return f'translate({x:.2f} {y:.2f}) scale({scale:.5f})'


def backdrop(width, height, scale):
    """the back layer: a radial gradient lit a little above center, and a faint grid"""
    step = 64 * scale
    lines = []
    x = width / 2 % step
    while x < width:
        lines.append(f'M{x:.2f} 0V{height}')
        x += step
    y = height / 2 % step
    while y < height:
        lines.append(f'M0 {y:.2f}H{width}')
        y += step
    radius = max(width, height) * 0.75
    return (f'<defs><radialGradient id="bg" cx="{width / 2}" cy="{height * 0.45}" r="{radius}" '
            f'gradientUnits="userSpaceOnUse"><stop offset="0" stop-color="#123a49"/>'
            f'<stop offset="0.45" stop-color="#08161e"/><stop offset="1" stop-color="#020507"/></radialGradient></defs>'
            f'<rect width="{width}" height="{height}" fill="url(#bg)"/>'
            f'<path d="{" ".join(lines)}" stroke="#2b6577" stroke-opacity="0.28" stroke-width="{max(1.0, 2 * scale):.2f}" fill="none"/>')


def arc(radius, start, end):
    """an SVG arc path, angles in degrees clockwise from 12 o'clock"""
    def point(angle):
        a = math.radians(angle - 90)
        return radius * math.cos(a), radius * math.sin(a)
    (x0, y0), (x1, y1) = point(start), point(end)
    large = 1 if (end - start) % 360 > 180 else 0
    return f'M{x0:.2f} {y0:.2f}A{radius} {radius} 0 {large} 1 {x1:.2f} {y1:.2f}'


# Letterforms for "VISR": chamfered strokes on a 300-unit cap height, drawn by hand so no font is needed.
CAP = 300
CHAMFER = 56
LETTERS = {
    "V": (170, [[(0, 0), (85, CAP), (170, 0)]]),
    "I": (0, [[(0, 0), (0, CAP)]]),
    "S": (150, [[(150, 0), (CHAMFER, 0), (0, CHAMFER), (0, CAP / 2 - CHAMFER / 2), (CHAMFER / 2, CAP / 2),
                 (150 - CHAMFER / 2, CAP / 2), (150, CAP / 2 + CHAMFER / 2), (150, CAP - CHAMFER),
                 (150 - CHAMFER, CAP), (0, CAP)]]),
    "R": (160, [[(0, CAP), (0, 0), (160 - CHAMFER, 0), (160, CHAMFER), (160, CAP / 2 - CHAMFER), (160 - CHAMFER, CAP / 2),
                 (0, CAP / 2)], ["butt", (64, CAP / 2), (160, CAP + 10)]]),
}


def wordmark(stroke=46, gap=84, color=ICE):
    """VISR centered on the origin, in mark units"""
    width = sum(LETTERS[c][0] for c in "VISR") + gap * 3
    x = -width / 2
    paths = {"square": [], "butt": []}
    for c in "VISR":
        advance, strokes = LETTERS[c]
        for points in strokes:
            # a stroke that starts inside another one has butt caps, so its cap does not poke out
            cap = points[0] if isinstance(points[0], str) else "square"
            points = [p for p in points if not isinstance(p, str)]
            paths[cap].append("M" + "L".join(f"{x + px:.1f} {py - CAP / 2:.1f}" for px, py in points))
        x += advance + gap
    return "".join(f'<path d="{" ".join(d)}" fill="none" stroke="{color}" stroke-width="{stroke}" '
                   f'stroke-linejoin="miter" stroke-miterlimit="2" stroke-linecap="{cap}"/>' for cap, d in paths.items())


# Orbitron Regular (wght 400) outlines from docs/site/fonts/Orbitron-VariableFont_wght.woff2.
# SIL OFL 1.1: see docs/site/fonts/OFL-Orbitron.txt. Coordinates use a 1000-unit em;
# tracking matches the site wordmark's 0.06em (60 units between glyphs).
# Extract with fontTools.ttLib.TTFont, varLib.instancer.instantiateVariableFont
# at {"wght": 400}, and pens.svgPathPen.SVGPathPen on the V, I, S, R glyphs.
ORBITRON_LETTERS = {
    "V": (1003, "M452 0 35 720H130L499 79L867 720H962L546 0Z"),
    "I": (214, "M66 0V720H148V0Z"),
    "S": (822, "M171 0Q138 0 110.5 16.0Q83 32 67.0 59.5Q51 87 51 120V148H132V120Q132 104 143.5 92.5Q155 81 171 81H651Q667 81 678.5 92.5Q690 104 690 120V281Q690 297 678.5 308.0Q667 319 651 319H171Q138 319 110.5 335.5Q83 352 67.0 379.0Q51 406 51 439V600Q51 633 67.0 660.5Q83 688 110.5 704.0Q138 720 171 720H651Q684 720 711.5 704.0Q739 688 755.0 660.5Q771 633 771 600V572H690V600Q690 616 678.5 627.5Q667 639 651 639H171Q155 639 143.5 627.5Q132 616 132 600V439Q132 423 143.5 412.0Q155 401 171 401H651Q684 401 711.5 384.5Q739 368 755.0 341.0Q771 314 771 281V120Q771 87 755.0 59.5Q739 32 711.5 16.0Q684 0 651 0Z"),
    "R": (825, "M664 0 429 280H535L771 1V0ZM56 0V719H656Q689 719 716.5 702.5Q744 686 760.0 659.0Q776 632 776 599V388Q776 355 760.0 328.0Q744 301 716.5 284.5Q689 268 656 268H137V0ZM176 350H656Q672 350 683.5 361.0Q695 372 695 388V599Q695 615 683.5 626.5Q672 638 656 638H176Q160 638 148.5 626.5Q137 615 137 599V388Q137 372 148.5 361.0Q160 350 176 350Z"),
}


def topshelf_wordmark(color=ICE):
    """Orbitron VISR, centered by its visible bounds, fitted to the old wordmark width."""
    # Ink bounds after advancing VISR with 60 units of tracking: x=35..2995, y=0..720.
    # Preserve the old 778-unit ink width without stretching the font's letterforms.
    scale = 778 / (2995 - 35)
    x = 0
    paths = []
    for c in "VISR":
        advance, outline = ORBITRON_LETTERS[c]
        paths.append(f'<path transform="translate({x} 0)" d="{outline}"/>')
        x += advance + 60
    return (f'<g fill="{color}" transform="scale({scale} {-scale}) translate(-1515 -360)">'
            + "".join(paths) + "</g>")


def reticle_middle():
    ring = "".join(f'<path d="{arc(360, q + 14, q + 76)}"/>' for q in (0, 90, 180, 270))
    ticks = []
    for i in range(36):
        angle = i * 10
        if angle % 90 == 0:
            continue
        a = math.radians(angle - 90)
        inner, outer = (282, 318) if angle % 30 == 0 else (296, 314)
        ticks.append(f'M{inner * math.cos(a):.1f} {inner * math.sin(a):.1f}L{outer * math.cos(a):.1f} {outer * math.sin(a):.1f}')
    return (f'<g filter="url(#glow)"><g fill="none" stroke="{CYAN}" stroke-width="38" stroke-linecap="butt">{ring}</g>'
            f'<path d="{" ".join(ticks)}" stroke="{CYAN}" stroke-opacity="0.7" stroke-width="8"/></g>')


def reticle_front():
    hairs = "M0 -420V-205 M0 205V420 M-420 0H-205 M205 0H420"
    chevron = "M-92 70L0 -40L92 70"
    return (f'<g filter="url(#glow)"><path d="{hairs}" stroke="{ICE}" stroke-width="30"/>'
            f'<path d="{chevron}" fill="none" stroke="{ICE}" stroke-width="30" stroke-linejoin="miter"/>'
            f'<circle cx="0" cy="118" r="22" fill="{AMBER}"/></g>')


VISOR = "M-330 -150H330L420 -64V58L300 150H-300L-420 58V-64Z"


def visor_middle():
    return (f'<defs><linearGradient id="lens" x1="0" y1="-150" x2="0" y2="150" gradientUnits="userSpaceOnUse">'
            f'<stop offset="0" stop-color="#7fe9ff"/><stop offset="0.55" stop-color="#1aa6c9"/>'
            f'<stop offset="1" stop-color="#063f52"/></linearGradient></defs>'
            f'<g filter="url(#glow)"><path d="{VISOR}" fill="url(#lens)" stroke="{ICE}" stroke-width="14" stroke-linejoin="miter"/></g>')


def visor_front():
    shine = "M-250 -150H-120L-260 150H-390Z"
    bars = "".join(f'<rect x="{-360 + i * 34}" y="{86 - h}" width="20" height="{h}"/>' for i, h in enumerate((28, 46, 64, 40)))
    return (f'<path d="{shine}" fill="#ffffff" fill-opacity="0.22" clip-path="url(#lensclip)"/>'
            f'<defs><clipPath id="lensclip"><path d="{VISOR}"/></clipPath></defs>'
            f'<g filter="url(#glow)" stroke="{ICE}" fill="none">'
            f'<circle r="74" stroke-width="16"/><path d="M-26 0H-150 M26 0H150 M0 -26V-112 M0 26V112" stroke-width="12"/>'
            f'<g fill="{ICE}" stroke="none" fill-opacity="0.85">{bars}</g>'
            f'<circle cx="320" cy="-62" r="18" fill="{AMBER}" stroke="none"/></g>')


def wordmark_middle():
    corner = 120
    d = []
    for sx in (-1, 1):
        for sy in (-1, 1):
            x, y = sx * 400, sy * 330
            d.append(f'M{x:.0f} {y - sy * corner:.0f}L{x:.0f} {y:.0f}L{x - sx * corner:.0f} {y:.0f}')
    return (f'<g filter="url(#glow)"><path d="{" ".join(d)}" fill="none" stroke="{CYAN}" stroke-width="28"/>'
            f'<path d="M-300 230H300" stroke="{CYAN}" stroke-opacity="0.55" stroke-width="10"/>'
            f'<circle cx="0" cy="230" r="16" fill="{AMBER}"/></g>')


def wordmark_front():
    return f'<g filter="url(#glow)">{wordmark()}</g>'


# The Halo: CE visor and helmet, traced from a frame of the game's c40 cutscene rendered from the user's own disc
# (`c40-02820`, the Chief facing the camera): the gold visor, the helmet and the lit rims of the brow pads were
# segmented, the head's slight roll leveled, each mask mirrored about the visor's center line (the model is
# symmetric), and these paths fitted. Units are the 1024-unit mark: the helmet is 900 wide, the visor 710.
VISOR_PATH = "M-75 -76C-94 -76 -114 -72 -133 -70C-153 -68 -172 -65 -192 -63C-211 -61 -231 -59 -250 -56C-269 -53 -292 -55 -308 -46C-323 -37 -335 -19 -342 -2C-350 15 -352 36 -354 55C-356 75 -357 95 -356 114C-354 133 -353 154 -346 172C-339 189 -325 205 -313 220C-302 236 -291 255 -275 265C-259 274 -238 276 -219 278C-200 281 -180 279 -160 279C-141 279 -121 279 -101 279C-82 279 -62 279 -43 279C-23 279 -3 279 16 279C36 279 55 279 75 279C94 279 114 279 134 279C153 279 173 279 192 279C212 278 234 282 251 275C269 269 284 255 297 242C311 228 323 211 332 194C342 178 350 159 354 141C358 122 356 101 355 82C355 62 354 42 349 24C345 5 341 -19 329 -31C317 -44 295 -48 276 -53C258 -57 237 -58 218 -60C198 -62 179 -65 160 -67C140 -69 121 -73 101 -74C82 -75 62 -75 43 -75C23 -75 3 -73 -16 -73C-36 -73 -55 -77 -75 -76Z"
HELMET_PATH = "M-340 -266L-415 -146L-418 -135L-411 -113L-442 -67L-406 31L-406 155L-372 205L-438 240L-450 343L450 343L438 240L372 205L406 155L406 31L442 -67L411 -113L418 -135L415 -146L338 -267L269 -272L247 -303L82 -344L15 -338L-82 -344L-247 -303L-269 -272L-329 -271Z"
BROW_RIMS = "M129 -271L128 -258L123 -250L124 -209L119 -163L123 -144L134 -139L138 -119L150 -118L155 -122L152 -145L162 -209L180 -223L195 -225L257 -216L305 -203L320 -190L331 -172L349 -153L386 -123L390 -110L397 -105L397 -96L420 -90L422 -100L411 -114L418 -141L338 -267L329 -271L143 -279Z M-129 -271L-143 -279L-329 -271L-338 -267L-418 -141L-411 -114L-422 -100L-420 -90L-397 -96L-397 -105L-390 -110L-386 -123L-349 -153L-331 -172L-320 -190L-305 -203L-257 -216L-195 -225L-180 -223L-162 -209L-152 -145L-155 -122L-150 -118L-138 -119L-134 -139L-123 -144L-119 -163L-124 -209L-123 -250L-128 -258Z"
HELMET_CLIP_Y = 344


# the gold glass: a radial gradient's three stops (color, opacity), and the rim's bright and base colors
GLASS = (("#fff0b0", 0.95), ("#e0a53a", 0.92), ("#5a3a0c", 0.95))
GLASS_RIM = ("#fff6d8", "#f2c35a")


def visor_defs(view, visor):
    """the visor's clip and the glass and rim gradients"""
    (c0, o0), (c1, o1), (c2, o2) = GLASS
    rim = GLASS_RIM
    return (f'<defs><clipPath id="visorclip-{view}"><path d="{visor}"/></clipPath>'
            f'<radialGradient id="glass-{view}" cx="0.42" cy="0.78" r="0.75" fx="0.38" fy="0.85">'
            f'<stop offset="0" stop-color="{c0}" stop-opacity="{o0}"/><stop offset="0.5" stop-color="{c1}" stop-opacity="{o1}"/>'
            f'<stop offset="1" stop-color="{c2}" stop-opacity="{o2}"/></radialGradient>'
            f'<linearGradient id="edge-{view}" x1="0" y1="0" x2="1" y2="1">'
            f'<stop offset="0" stop-color="{rim[0]}"/><stop offset="0.45" stop-color="{rim[1]}"/>'
            f'<stop offset="1" stop-color="{rim[1]}" stop-opacity="0.35"/></linearGradient></defs>')


def specular(view, visor, strength=1.0):
    """curved highlights, as on a convex glass: a wide arc under the top edge and a short bright one on the
    upper left, clipped to the glass"""
    return (f'<defs><linearGradient id="spec-{view}" x1="0" y1="0" x2="1" y2="0">'
            f'<stop offset="0" stop-color="#ffffff" stop-opacity="{0.15 * strength:.2f}"/>'
            f'<stop offset="0.3" stop-color="#ffffff" stop-opacity="{0.8 * strength:.2f}"/>'
            f'<stop offset="0.7" stop-color="#ffffff" stop-opacity="{0.35 * strength:.2f}"/>'
            f'<stop offset="1" stop-color="#ffffff" stop-opacity="0"/></linearGradient></defs>'
            f'<g clip-path="url(#visorclip-{view})" fill="none" stroke="url(#spec-{view})" stroke-linecap="round">'
            f'<ellipse cx="0" cy="330" rx="560" ry="372" stroke-width="22"/>'
            f'<path d="M-300 70Q-250 -10 -150 -22" stroke="#ffffff" stroke-opacity="{0.85 * strength:.2f}" stroke-width="20"/></g>')


def visor_glass(view, visor):
    return visor_defs(view, visor) + f'<path d="{visor}" fill="url(#glass-{view})"/>'


def visor_edge(view, visor, width=26):
    """the visor's rim: bright where it catches the light at the upper left, fading to the lower right"""
    return (f'<g filter="url(#glow)"><path d="{visor}" fill="none" stroke="url(#edge-{view})" stroke-width="{width}" '
            f'stroke-linejoin="round"/></g>')


def helmet_shell(view, helmet, clip_y):
    """the helmet as a dark shape barely lighter than the backdrop, with a cyan rim light along its upper left edge"""
    return (f'<defs><linearGradient id="shell-{view}" x1="0" y1="0" x2="0" y2="1">'
            f'<stop offset="0" stop-color="#16343f"/><stop offset="1" stop-color="#081820"/></linearGradient>'
            f'<linearGradient id="fade-{view}" x1="0" y1="-500" x2="0" y2="{clip_y}" gradientUnits="userSpaceOnUse">'
            f'<stop offset="0.75" stop-color="#fff"/><stop offset="1" stop-color="#000"/></linearGradient>'
            f'<mask id="shellmask-{view}" maskUnits="userSpaceOnUse" x="-512" y="-512" width="1024" height="1024">'
            f'<rect x="-512" y="-512" width="1024" height="1024" fill="url(#fade-{view})"/></mask>'
            f'<linearGradient id="rim-{view}" x1="0" y1="0" x2="1" y2="1">'
            f'<stop offset="0" stop-color="{CYAN}" stop-opacity="0.9"/><stop offset="0.6" stop-color="{CYAN}" stop-opacity="0.15"/>'
            f'<stop offset="1" stop-color="{CYAN}" stop-opacity="0"/></linearGradient></defs>'
            f'<g mask="url(#shellmask-{view})"><path d="{helmet}" fill="url(#shell-{view})"/>'
            f'<g filter="url(#glow)"><path d="{helmet}" fill="none" stroke="url(#rim-{view})" stroke-width="12" stroke-linejoin="round"/></g></g>')


def brow_rims(opacity=0.85):
    """the lit lower rims of the two brow pads above the visor"""
    return f'<g filter="url(#glow)"><path d="{BROW_RIMS}" fill="{CYAN}" fill-opacity="{opacity}"/></g>'

# visor-helmet-gold: the gold visor set in the helmet: a dark shell with a rim light, and the brow pads' lit edges
HELMET_VIEW = 'transform="scale(0.94)"'


def visor_helmet_gold_middle():
    return f'<g {HELMET_VIEW}>{helmet_shell("g", HELMET_PATH, HELMET_CLIP_Y)}{brow_rims()}{visor_glass("g", VISOR_PATH)}</g>'


def visor_helmet_gold_front():
    return (f'<g {HELMET_VIEW}>{visor_defs("g2", VISOR_PATH)}{specular("g2", VISOR_PATH)}'
            f'{visor_edge("g2", VISOR_PATH, 12)}</g>')


CONCEPTS = {
    "reticle": ("A broken reticle ring with tick marks, crosshairs and a chevron; amber center pip.",
                reticle_middle, reticle_front),
    "visor": ("An abstract octagonal visor lens in cyan glass with a HUD reticle and meter bars.",
              visor_middle, visor_front),
    "wordmark": ("The letters VISR in chamfered strokes inside HUD corner brackets.",
                 wordmark_middle, wordmark_front),
    "visor-helmet-gold": ("The visor traced from the game in gold glass, set in the dark helmet shell with a cyan rim light.",
                          visor_helmet_gold_middle, visor_helmet_gold_front),
}


def layer_svgs(concept, width, height, mark_scale, x=None, y=None, with_wordmark=False):
    """(back, middle, front) SVG documents for one canvas"""
    _, middle, front = CONCEPTS[concept]
    t = place(width, height, mark_scale, x, y)
    blur = 10
    back = svg(width, height, backdrop(width, height, mark_scale), blur)
    mid = svg(width, height, f'<g transform="{t}">{middle()}</g>', blur)
    extra = ""
    if with_wordmark:
        # top shelf: the word beside the mark
        wx = width * 0.66
        extra = f'<g transform="{place(width, height, mark_scale * 1.4, wx, height / 2)}"><g filter="url(#glow)">{topshelf_wordmark()}</g></g>'
    fr = svg(width, height, f'<g transform="{t}">{front()}</g>{extra}', blur)
    return back, mid, fr


def flat_svg(concept, width, height, mark_scale, **kwargs):
    back, mid, fr = layer_svgs(concept, width, height, mark_scale, **kwargs)

    def inner(doc):
        return doc[doc.index("</defs>") + len("</defs>"):-len("</svg>")]
    return svg(width, height, inner(back) + inner(mid) + inner(fr))


def render(document, out, width, height, opaque=False):
    with tempfile.NamedTemporaryFile("w", suffix=".svg", delete=False) as f:
        f.write(document)
    subprocess.run(["rsvg-convert", "--width", str(width), "--height", str(height), "--output", str(out), f.name], check=True)
    Path(f.name).unlink()
    if opaque:
        subprocess.run(["magick", str(out), "-background", "black", "-alpha", "remove", "-alpha", "off",
                        # no date chunks, so a rerun writes the same bytes
                        "-define", "png:exclude-chunks=date,time", f"PNG24:{out}"], check=True)


def write_contents(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + "\n")


INFO = {"author": "xcode", "version": 1}
IOS_SIZES = (20, 29, 40, 58, 60, 76, 80, 87, 120, 152, 167, 180, 1024)


def ios_catalog(concept):
    folder = IOS / "Assets.xcassets/AppIcon.appiconset"
    for size in IOS_SIZES:
        render(flat_svg(concept, 1024, 1024, 1.0), folder / f"AppIcon-{size}.png", size, size, opaque=True)


def stack_layers(folder, concept, names, sizes, ext, idiom, mark_fraction):
    """write a layered stack: names like ("Front", "Middle", "Back"), sizes as [(scale, width, height)]"""
    write_contents(folder / "Contents.json", {"info": INFO, "layers": [{"filename": f"{n}.{ext}"} for n in names]})
    for name in names:
        imageset = folder / f"{name}.{ext}/Content.imageset"
        if imageset.parent.exists():
            shutil.rmtree(imageset.parent)
        write_contents(imageset.parent / "Contents.json", {"info": INFO})
        imageset.mkdir()
        images = []
        for scale, width, height in sizes:
            # visionOS layers name their file after the layer; tvOS layers by scale
            filename = f"{name}.png" if idiom == "vision" else f"Content@{scale}x.png"
            back, mid, fr = layer_svgs(concept, width, height, height * mark_fraction / 1024)
            doc = {"Back": back, "Middle": mid, "Front": fr}[name]
            render(doc, imageset / filename, width, height, opaque=name == "Back")
            images.append({"filename": filename, "idiom": idiom, "scale": f"{scale}x"})
        write_contents(imageset / "Contents.json", {"images": images, "info": INFO})


def tv_catalog(concept):
    brand = IOS / "Assets-tvOS.xcassets/AppIcon.brandassets"
    names = ("Front", "Middle", "Back")
    stack_layers(brand / "App Icon.imagestack", concept, names, [(1, 400, 240), (2, 800, 480)], "imagestacklayer", "tv", 0.86)
    stack_layers(brand / "App Icon - App Store.imagestack", concept, names, [(1, 1280, 768)], "imagestacklayer", "tv", 0.86)
    for name, width in (("Top Shelf Image", 1920), ("Top Shelf Image Wide", 2320)):
        for scale in (1, 2):
            w, h = width * scale, 720 * scale
            render(flat_svg(concept, w, h, h * 0.78 / 1024, x=w * 0.3, with_wordmark=True),
                   brand / f"{name}.imageset/{name}@{scale}x.png", w, h, opaque=True)


def vision_catalog(concept):
    stack = IOS / "Assets-visionOS.xcassets/AppIcon.solidimagestack"
    stack_layers(stack, concept, ("Front", "Middle", "Back"), [(2, 1024, 1024)], "solidimagestacklayer", "vision", 1.0)


LABEL_FONT = "/System/Library/Fonts/Menlo.ttc"  # ImageMagick has no default font here
SHEETS = {
    # the first three concepts, each as iOS, visionOS, and 120 and 60 px Home Screen sizes
    "concepts.png": (("reticle", "visor", "wordmark"), (("ios", 360), ("visionos", 360), ("ios", 120), ("ios", 60)), False),
}


def previews(out):
    out.mkdir(parents=True, exist_ok=True)
    for concept, (description, _, _) in CONCEPTS.items():
        back, mid, fr = layer_svgs(concept, 1024, 1024, 1.0)
        for name, doc in (("back", back), ("middle", mid), ("front", fr)):
            (out / f"{concept}-{name}.svg").write_text(doc)
            render(doc, out / f"{concept}-{name}.png", 1024, 1024)
        flat = out / f"{concept}-flat.png"
        render(flat_svg(concept, 1024, 1024, 1.0), flat, 1024, 1024, opaque=True)
        # mockups: the iOS rounded square and the visionOS circle, next to a 60-point size
        subprocess.run(["magick", str(flat), "(", "+clone", "-alpha", "extract", "-fill", "black", "-colorize", "100",
                        "-fill", "white", "-draw", "roundrectangle 0,0 1023,1023 230,230", ")", "-alpha", "off",
                        "-compose", "CopyOpacity", "-composite", str(out / f"{concept}-ios.png")], check=True)
        subprocess.run(["magick", str(flat), "(", "+clone", "-fill", "black", "-colorize", "100",
                        "-fill", "white", "-draw", "circle 512,512 512,0", ")", "-alpha", "off",
                        "-compose", "CopyOpacity", "-composite", str(out / f"{concept}-visionos.png")], check=True)
        (out / f"{concept}.txt").write_text(description + "\n")
    for sheet, (concepts, cells, labeled) in SHEETS.items():
        rows = []
        for concept in concepts:
            row = out / f"{concept}-row.png"
            label = ["(", "-font", LABEL_FONT, "-fill", "#c8d4da", "-pointsize", "26", f"label:{concept}",
                     "-gravity", "west", "-extent", "300x", ")"] if labeled else []
            images = [arg for kind, size in cells
                      for arg in ("(", str(out / f"{concept}-{kind}.png"),
                                  "-resize", f"{size}x{size}", ")")]
            subprocess.run(["magick", "-background", "#202428", *label, *images,
                            "-bordercolor", "#202428", "-border", "12", "-gravity", "center", "+append", str(row)], check=True)
            rows.append(str(row))
        subprocess.run(["magick", "-background", "#202428", *rows, "-gravity", "west", "-append",
                        "-bordercolor", "#202428", "-border", "24", str(out / sheet)], check=True)
        for row in rows:
            Path(row).unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--concept", choices=sorted(CONCEPTS), default=DEFAULT_CONCEPT)
    parser.add_argument("--catalogs", action="store_true", help="render the concept into the iOS, tvOS and visionOS catalogs")
    parser.add_argument("--previews", type=Path, help="write every concept's layers and mockups into this folder")
    args = parser.parse_args()
    if not (args.catalogs or args.previews):
        parser.error("nothing to do: pass --catalogs or --previews")
    if args.previews:
        previews(args.previews)
    if args.catalogs:
        ios_catalog(args.concept)
        tv_catalog(args.concept)
        vision_catalog(args.concept)


if __name__ == "__main__":
    main()
