#!/usr/bin/env python3
"""Regenerate the site and marked README blocks from shared public facts."""

from __future__ import annotations

import argparse
import ast
from html import escape
from pathlib import Path
import re
import struct
from string import Template
import tomllib

from markdown_it import MarkdownIt
import yaml

ROOT = Path(__file__).resolve().parents[1]
DEFAULTS = {
    "title": "VISR",
    "description": "A native port of Halo: Combat Evolved to Apple Vision Pro: full stereo VR with head tracking and foveated rendering. Bring your own Xbox disc.",
    "og_title": "VISR: Halo: Combat Evolved on Apple Vision Pro",
}


def deployment_versions(source: str, mac_source: str | None = None) -> dict[str, str]:
    """Read literal build targets without importing the build script's dependencies."""
    tree = ast.parse(source)
    versions = {}
    for node in ast.walk(tree):
        if (isinstance(node, ast.Assign)
                and any(isinstance(target, ast.Tuple)
                        and [getattr(item, "id", None) for item in target.elts] == ["system", "deployment"]
                        for target in node.targets)
                and isinstance(node.value, ast.Subscript)):
            targets = ast.literal_eval(node.value.value)
            versions.update({key: value[1] for key, value in targets.items()})
    # ios_build.py's --mac target is Catalyst; the native AppKit app has its own target.
    if mac_source is None:
        mac_source = (ROOT / "tools/macos_build.py").read_text(encoding="utf-8")
    for node in ast.walk(ast.parse(mac_source)):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            match = re.fullmatch(r"-DCMAKE_OSX_DEPLOYMENT_TARGET=(\d+(?:\.\d+)*)", node.value)
            if match:
                versions["macos"] = match[1]
    if set(versions) != {"ios", "tvos", "visionos", "macos"}:
        raise ValueError("Cannot read deployment targets from tools/ios_build.py")
    return versions


def load_facts() -> dict:
    facts = tomllib.loads((ROOT / "docs/features.toml").read_text(encoding="utf-8"))
    versions = deployment_versions((ROOT / "tools/ios_build.py").read_text(encoding="utf-8"))
    for platform in facts["platforms"].values():
        platform["minimum_os"] = versions[platform["deployment"]]
    return facts


def fact_block(kind: str, name: str, facts: dict) -> str:
    if kind == "features" and name in facts["platforms"]:
        versions = deployment_versions((ROOT / "tools/ios_build.py").read_text(encoding="utf-8"))
        values = {f"{system}_minimum": version.removesuffix(".0") for system, version in versions.items()}
        return "\n".join("- " + bullet.format(**values) for bullet in facts["platforms"][name]["bullets"])
    if kind == "shared" and name == "every-device":
        return "\n".join(f"- **{item['title']}:** {item['text']}" for item in facts["every-device"]["items"])
    if kind == "shared":
        key = name.replace("-", "_")
        if key in facts["shared"]:
            return facts["shared"][key]
    raise ValueError(f"Unknown facts block: {kind}: {name}")


NAV_ICONS = {"Vision Pro": "vision-pro", "iPhone": "iphone", "iPad": "ipad", "Apple TV": "apple-tv",
             "Mac": "mac", "GitHub": "github"}


SECTION_ICONS = {"apple-vision-pro": ["vision-pro"], "iphone-and-ipad": ["iphone", "ipad"],
                 "apple-tv": ["apple-tv"], "mac": ["mac"]}


def glyph(name: str) -> str:
    """a platform glyph from docs/site/icons/nav/, inlined so it takes the text color"""
    icon = (ROOT / "docs/site/icons/nav" / f"{name}.svg").read_text(encoding="utf-8").strip()
    return f'<span class="nav-icon">{icon}</span>'


def nav_with_icons(html: str) -> str:
    """prefix each platform link with its glyph"""
    def add_icon(match: re.Match) -> str:
        name = NAV_ICONS.get(match[2])
        return match[0] if name is None else f'{match[1]}{glyph(name)}{match[2]}</a>'
    return re.sub(r'(<a href="[^"]*">)([^<]+)</a>', add_icon, html)


def heading_with_icons(html: str, identifier: str) -> str:
    """prefix a platform section's heading with its glyphs"""
    icons = "".join(glyph(name) for name in SECTION_ICONS.get(identifier, []))
    return html.replace(f'<h2 id="{identifier}">', f'<h2 id="{identifier}">{icons}', 1) if icons else html


INLINE = MarkdownIt("commonmark", {"html": False})
EVERY_DEVICE_MARKER = "VISR-EVERY-DEVICE-COLUMNS"


def every_device_columns(facts: dict) -> str:
    """the every-device items as centered columns, each icon inlined so it takes the theme's color"""
    columns = []
    for item in facts["every-device"]["items"]:
        icon = (ROOT / "docs/site/icons" / f"{item['icon']}.svg").read_text(encoding="utf-8").strip()
        columns.append(f'      <li><span class="pillar-icon">{icon}</span>'
                       f'<h3>{escape(item["title"])}</h3><p>{INLINE.renderInline(item["text"]).strip()}</p></li>')
    return "\n".join(['    <ul class="pillars">', *columns, '    </ul>'])


def expand_includes(source: str, facts: dict) -> str:
    return re.sub(r"\{\{\s*([\w-]+):\s*([\w-]+)\s*\}\}",
                  lambda match: fact_block(*match.groups(), facts), source)


def render_readme(source: str, facts: dict) -> str:
    if source.count("<!-- generated:") != source.count("<!-- end generated -->"):
        raise ValueError("Unbalanced generated README markers")
    pattern = r"(<!-- generated: ([\w-]+): ([\w-]+) -->)\n.*?\n?(<!-- end generated -->)"
    return re.sub(pattern, lambda match: (match[1] + "\n" + fact_block(match[2], match[3], facts)
                                          + "\n" + match[4]), source, flags=re.S)


def write_outputs(outputs: dict[Path, str], *, check: bool) -> int:
    stale = [path for path, expected in outputs.items()
             if not path.is_file() or path.read_text(encoding="utf-8") != expected]
    if check:
        for path in stale:
            print(f"Stale generated file: {path.name}; run python3 tools/build_site.py")
        return int(bool(stale))
    for path in stale:
        path.write_text(outputs[path], encoding="utf-8")
    return 0


def front_matter(source: str) -> tuple[dict[str, str], str]:
    metadata = dict(DEFAULTS)
    lines = source.splitlines(keepends=True)
    if lines and lines[0].strip() == "---":
        end = next((i for i in range(1, len(lines)) if lines[i].strip() == "---"), None)
        if end is None:
            raise ValueError("Front matter needs a closing --- line")
        values = yaml.safe_load("".join(lines[1:end])) or {}
        if not isinstance(values, dict) or values.keys() - DEFAULTS.keys():
            raise ValueError("Front matter supports title, description and og_title")
        if any(not isinstance(value, str) for value in values.values()):
            raise ValueError("Front matter values must be strings")
        metadata.update(values)
        source = "".join(lines[end + 1:])
    return metadata, source


def image_size(path: Path) -> tuple[int, int]:
    """Read web image dimensions without adding an image decoder to the build."""
    with path.open("rb") as image:
        header = image.read(24)
        if header.startswith(b"\x89PNG\r\n\x1a\n"):
            return struct.unpack(">II", header[16:24])
        if header[:2] == b"\xff\xd8":
            image.seek(2)
            while image.read(1) == b"\xff":
                marker = image.read(1)
                while marker == b"\xff":
                    marker = image.read(1)
                length = int.from_bytes(image.read(2), "big")
                if marker in (b"\xc0", b"\xc1", b"\xc2"):
                    _, height, width = struct.unpack(">BHH", image.read(5))
                    return width, height
                if length < 2:
                    break
                image.seek(length - 2, 1)
    raise ValueError(f"Unsupported site image: {path.name}")


def render_site(source: str, template: str | None = None, *, asset_root: Path | None = None) -> str:
    facts = load_facts()
    metadata, source = front_matter(source)
    source = re.sub(r"\{\{\s*shared:\s*every-device\s*\}\}", EVERY_DEVICE_MARKER, source)
    source = expand_includes(source, facts)
    asset_root = (asset_root or ROOT / "docs/site").resolve()
    md = MarkdownIt("commonmark", {"html": False})
    tokens = md.parse(source)
    # Work on top-level blocks so nested paragraphs retain their Markdown layout.
    blocks = []
    start = 0
    depth = 0
    for i, token in enumerate(tokens):
        depth += token.nesting
        if depth == 0:
            blocks.append(tokens[start:i + 1])
            start = i + 1

    def inline(block):
        return block[1].children or []

    def local_image_size(image, required=False):
        path = (asset_root / image.attrGet("src")).resolve()
        if not path.is_relative_to(asset_root) or not path.is_file():
            if required:
                raise ValueError("A device image must be a local site image")
            return
        width, height = image_size(path)
        image.attrSet("width", str(width))
        image.attrSet("height", str(height))

    device_blocks = {}
    for index, block in enumerate(blocks[3:], 3):
        if block[0].type != "paragraph_open":
            continue
        children = inline(block)
        if not children or children[0].type != "image":
            continue
        annotation = "".join(child.content for child in children[1:]).strip()
        image = children[0]
        if annotation.startswith("{device="):
            match = re.fullmatch(r"\{device=(iphone|ipad|tv|mac|vision)\}", annotation)
            if not match:
                raise ValueError("Use device=iphone, ipad, tv or mac on a standalone image")
            device_blocks[index] = match.group(1)
            local_image_size(image, required=True)
            image.attrSet("loading", "lazy")
            image.attrSet("decoding", "async")
        elif len(children) == 1:
            local_image_size(image)

    if (len(blocks) < 3 or blocks[0][0].type != "paragraph_open"
            or len(inline(blocks[0])) != 1 or inline(blocks[0])[0].type != "image"
            or blocks[1][0].type != "heading_open" or blocks[1][0].tag != "h1"
            or blocks[1][1].content != "VISR"
            or blocks[2][0].type != "heading_open" or blocks[2][0].tag != "h2"):
        raise ValueError("Start with a logo image, # VISR and a ## tagline")

    image = inline(blocks[0])[0]
    image.attrSet("class", "icon")
    image.attrSet("width", "180")
    image.attrSet("height", "180")
    logo = md.renderer.render([image], md.options, {})
    output = ['  <header class="head">', '    <div class="brand">',
              f'      {logo}', '      <h1 class="wordmark">VISR</h1>', '    </div>']
    blocks[2][0].tag = blocks[2][-1].tag = "h1"
    output.append(md.renderer.render(blocks[2], md.options, {}).rstrip())
    section = False
    copy = []
    media = []

    def finish_section():
        if media:
            output.extend(['    <div class="platform-layout">', '      <div class="platform-copy">',
                           *copy, '      </div>', '      <div class="platform-media">',
                           *media, '      </div>', '    </div>'])
        else:
            output.extend(copy)
        output.append("  </section>")
        copy.clear()
        media.clear()

    identifiers = set()
    for index, block in enumerate(blocks[3:], 3):
        first = block[0]
        if first.type == "heading_open" and first.tag == "h2":
            if section:
                finish_section()
            else:
                output.append("  </header>")
            text = md.renderer.renderInlineAsText(inline(block), md.options, {})
            base = re.sub(r"[^\w-]+", "-", text.lower()).strip("-") or "section"
            identifier = base
            suffix = 2
            while identifier in identifiers:
                identifier = f"{base}-{suffix}"
                suffix += 1
            identifiers.add(identifier)
            first.attrSet("id", identifier)
            output.append(f'  <section aria-labelledby="{escape(identifier, quote=True)}">')
            section = True
            copy.append(heading_with_icons(md.renderer.render(block, md.options, {}).rstrip(), identifier))
            continue
        elif not section and first.type == "paragraph_open":
            children = inline(block)
            links = [child for child in children if child.type == "link_open"]
            if len(links) > 1 and all(child.attrGet("href").startswith(("#", "https://github.com/")) for child in links):
                for child in children:
                    if child.type == "text" and child.content.strip() == "•":
                        child.content = " "
                output.extend(['    <nav class="platform-nav" aria-label="Platforms">',
                               nav_with_icons(md.renderer.render(children, md.options, {}).rstrip()), '    </nav>'])
                continue
            lone_link = (children and children[0].type == "link_open"
                         and children[-1].type == "link_close"
                         and sum(child.type == "link_open" for child in children) == 1)
            first.attrSet("class", "links" if lone_link else "lede")
        if index in device_blocks:
            if not section:
                raise ValueError("Place a device image inside a section")
            image = md.renderer.render([inline(block)[0]], md.options, {})
            media.append(f'        <figure class="device device-{device_blocks[index]}">{image}</figure>')
        else:
            (copy if section else output).append(md.renderer.render(block, md.options, {}).rstrip())
    if section:
        finish_section()
    else:
        output.append("  </header>")
    if template is None:
        template = (ROOT / "tools/site_template.html").read_text(encoding="utf-8")
    return Template(template).substitute(
        **{key: escape(value, quote=True) for key, value in metadata.items()},
        content="\n".join(output).replace(f"<p>{EVERY_DEVICE_MARKER}</p>", every_device_columns(facts)),
        legal=md.render(facts["shared"]["legal"]).rstrip(),
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="report stale generated files without writing")
    args = parser.parse_args()
    source = (ROOT / "docs/site/index.md").read_text(encoding="utf-8")
    readme = ROOT / "README.md"
    return write_outputs({
        ROOT / "docs/site/index.html": render_site(source),
        readme: render_readme(readme.read_text(encoding="utf-8"), load_facts()),
    }, check=args.check)


if __name__ == "__main__":
    raise SystemExit(main())
