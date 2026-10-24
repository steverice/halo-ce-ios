"""Keep the published landing page synchronized with its Markdown source."""

import pytest

from tools.build_site import ROOT, render_site


SOURCE = """![Logo](icon.png)

# VISR

## Experience *Halo: Combat Evolved*

An introduction with **emphasis**.

> A quote with a [citation](https://example.com).

[GitHub](https://github.com/steverice/visr)

## Apple Vision Pro

- A feature

![Gameplay](a30.png)

## Apple Vision Pro

Another section.
"""


def test_committed_page_matches_markdown():
    site = ROOT / "docs/site"
    assert (site / "index.html").read_text(encoding="utf-8") == render_site(
        (site / "index.md").read_text(encoding="utf-8")
    ), "Run python3 tools/build_site.py and commit index.html"


def test_layout_preserves_markdown_and_unique_section_ids():
    page = render_site(SOURCE)
    assert '<img src="icon.png" alt="Logo" class="icon" width="180" height="180" />' in page
    assert '<h1 class="wordmark">VISR</h1>' in page
    assert '<h1>Experience <em>Halo: Combat Evolved</em></h1>' in page
    assert '<p class="lede">An introduction with <strong>emphasis</strong>.</p>' in page
    assert '<blockquote>\n<p>A quote with' in page
    assert '<p class="links"><a href="https://github.com/steverice/visr">GitHub</a></p>' in page
    for identifier in ("apple-vision-pro", "apple-vision-pro-2"):
        assert f'<section aria-labelledby="{identifier}">' in page
        assert f'<h2 id="{identifier}">Apple Vision Pro</h2>' in page
    assert '<li>A feature</li>' in page
    assert '<img src="a30.png" alt="Gameplay" />' in page
    assert '<h1>Experience Halo: Combat Evolved</h1>' in render_site(SOURCE.replace('*', ''))


def test_front_matter_updates_and_escapes_metadata():
    page = render_site('---\ntitle: "A & B"\ndescription: \'A "description"\'\nog_title: Preview\n---\n' + SOURCE)
    assert '<title>A &amp; B</title>' in page
    assert '<meta name="description" content="A &quot;description&quot;">' in page
    assert '<meta property="og:description" content="A &quot;description&quot;">' in page
    assert '<meta property="og:title" content="Preview">' in page


@pytest.mark.parametrize("prefix", ["---\ntitle: 42\n---\n", "---\nunknown: value\n---\n", "---\ntitle: Missing end\n"])
def test_invalid_front_matter_fails_build(prefix):
    with pytest.raises(ValueError):
        render_site(prefix + SOURCE)


def test_missing_brand_fails_build():
    with pytest.raises(ValueError, match="Start with"):
        render_site("## Missing brand\n")
