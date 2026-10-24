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
    assert '<img src="a30.png" alt="Gameplay" width=' in page
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


def test_platform_navigation_is_a_labeled_group_of_links():
    source = SOURCE.replace('[GitHub](https://github.com/steverice/visr)',
                            '[Vision Pro](#apple-vision-pro) • [iPad](#iphone-and-ipad) • [GitHub](https://github.com/steverice/visr)')
    page = render_site(source)
    assert '<nav class="platform-nav" aria-label="Platforms">' in page
    assert '<a href="#iphone-and-ipad">iPad</a>' in page
    assert ' • ' not in page


def test_device_images_keep_alt_text_dimensions_and_section_copy(tmp_path):
    # A PNG header is enough to determine intrinsic size without decoding pixels.
    import struct
    (tmp_path / 'phone.png').write_bytes(b'\x89PNG\r\n\x1a\n' + struct.pack('>I4sII', 13, b'IHDR', 1600, 734))
    source = SOURCE + '\n![Halo & touch controls](phone.png){device=iphone}\n'
    page = render_site(source, asset_root=tmp_path)
    assert '<div class="platform-layout">' in page
    assert '<div class="platform-copy">' in page
    assert '<div class="platform-media">' in page
    assert '<figure class="device device-iphone">' in page
    assert 'alt="Halo &amp; touch controls"' in page
    assert 'width="1600" height="734"' in page
    assert 'loading="lazy" decoding="async"' in page
    assert '{device=' not in page


@pytest.mark.parametrize('device', ['watch', 'iphone onclick=alert(1)'])
def test_invalid_device_annotation_fails_build(device):
    with pytest.raises(ValueError, match='device'):
        render_site(SOURCE + f'\n![Gameplay](icon.png){{device={device}}}\n')


def test_missing_device_image_fails_build():
    with pytest.raises(ValueError, match='image'):
        render_site(SOURCE + '\n![Gameplay](missing.jpg){device=mac}\n')


def test_device_image_cannot_read_outside_site(tmp_path):
    with pytest.raises(ValueError, match='image'):
        render_site(SOURCE + '\n![Gameplay](../private.png){device=tv}\n', asset_root=tmp_path)


@pytest.mark.parametrize('device', ['iphone', 'ipad', 'tv', 'mac'])
def test_jpeg_device_frames_support_each_platform(tmp_path, device):
    import struct
    # SOI, an application segment, then a progressive JPEG frame header.
    (tmp_path / 'game.jpg').write_bytes(b'\xff\xd8\xff\xe0\x00\x04ab\xff\xc2\x00\x07' + struct.pack('>BHH', 8, 900, 1600))
    page = render_site(SOURCE + f'\n![Gameplay](game.jpg){{device={device}}}\n', asset_root=tmp_path)
    assert f'<figure class="device device-{device}">' in page
    assert 'width="1600" height="900"' in page
    assert page.count('alt="Gameplay"') == 2


def test_platform_copy_stays_in_its_own_section(tmp_path):
    import struct
    (tmp_path / 'game.png').write_bytes(b'\x89PNG\r\n\x1a\n' + struct.pack('>I4sII', 13, b'IHDR', 1600, 900))
    source = SOURCE + '\n![TV gameplay](game.png){device=tv}\n\n## Mac\n\n- Native AppKit app\n\n![Mac gameplay](game.png){device=mac}\n'
    page = render_site(source, asset_root=tmp_path)
    tv, mac = page.split('<section aria-labelledby="mac">')
    assert 'TV gameplay' in tv
    assert 'Mac gameplay' not in tv
    assert 'Native AppKit app' in mac
    assert 'TV gameplay' not in mac


def test_device_image_before_a_section_fails_instead_of_moving():
    source = SOURCE.replace('## Apple Vision Pro', '![Gameplay](a30.png){device=iphone}\n\n## Apple Vision Pro', 1)
    with pytest.raises(ValueError, match='section'):
        render_site(source)
