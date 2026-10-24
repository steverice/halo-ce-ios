#!/usr/bin/env python3
"""Render the Markdown landing page with the site's existing layout."""

from __future__ import annotations

from html import escape
from pathlib import Path
import re
from string import Template

from markdown_it import MarkdownIt
import yaml

ROOT = Path(__file__).resolve().parents[1]
DEFAULTS = {
    "title": "VISR",
    "description": "A native port of Halo: Combat Evolved to Apple Vision Pro: full stereo VR with head tracking and foveated rendering. Bring your own Xbox disc.",
    "og_title": "VISR: Halo: Combat Evolved on Apple Vision Pro",
}


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


def render_site(source: str, template: str | None = None) -> str:
    metadata, source = front_matter(source)
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
    identifiers = set()
    for block in blocks[3:]:
        first = block[0]
        if first.type == "heading_open" and first.tag == "h2":
            output.append("  </section>" if section else "  </header>")
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
        elif not section and first.type == "paragraph_open":
            children = inline(block)
            lone_link = (children and children[0].type == "link_open"
                         and children[-1].type == "link_close"
                         and sum(child.type == "link_open" for child in children) == 1)
            first.attrSet("class", "links" if lone_link else "lede")
        output.append(md.renderer.render(block, md.options, {}).rstrip())
    output.append("  </section>" if section else "  </header>")
    if template is None:
        template = (ROOT / "tools/site_template.html").read_text(encoding="utf-8")
    return Template(template).substitute(
        **{key: escape(value, quote=True) for key, value in metadata.items()},
        content="\n".join(output),
    )


def main() -> None:
    source = (ROOT / "docs/site/index.md").read_text(encoding="utf-8")
    (ROOT / "docs/site/index.html").write_text(render_site(source), encoding="utf-8")


if __name__ == "__main__":
    main()
