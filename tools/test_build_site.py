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


def test_vision_pro_image_sits_beside_its_bullets():
    page = render_site((ROOT / 'docs/site/index.md').read_text(encoding='utf-8'))
    section = page.split('aria-labelledby="apple-vision-pro"', 1)[1].split('</section>', 1)[0]
    assert 'class="platform-layout"' in section
    assert 'class="device device-vision"' in section


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


def test_shared_features_expand_from_facts():
    from tools.build_site import expand_includes, load_facts
    facts = load_facts()
    source = '{{ features: iphone-and-ipad }}'
    expanded = expand_includes(source, facts)
    assert '- Full controller support' in expanded
    assert 'iOS 16+, iPhone 8 or newer' in expanded
    assert '{{' not in expanded
    facts['platforms']['iphone-and-ipad']['bullets'][1] = 'Changed controller support'
    assert 'Changed controller support' in expand_includes(source, facts)


def test_every_device_block_is_the_cloud_summary_and_tailoring_line():
    from tools.build_site import expand_includes, load_facts
    text = expand_includes('{{ shared: every-device }}', load_facts())
    assert text == ('- Your saves and player profiles sync across your devices through iCloud.\n'
                    '- VISR is tailored to each Apple device, so it just works.')


def test_deployment_versions_follow_build_script_without_importing_it():
    from tools.build_site import deployment_versions
    assert deployment_versions("system,deployment={'ios':('iOS','18.5'),'tvos':('tvOS','19.0'),'visionos':('visionOS','3.0')}[platform_name]", "x = '-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0'") == {
        'ios': '18.5', 'tvos': '19.0', 'visionos': '3.0', 'macos': '15.0'}


def test_unknown_include_fails_build():
    from tools.build_site import expand_includes, load_facts
    with pytest.raises(ValueError, match='Unknown'):
        expand_includes('{{ features: typo }}', load_facts())


def test_readme_generation_preserves_unmarked_prose():
    from tools.build_site import render_readme, load_facts
    source = 'Personal introduction.\n\n<!-- generated: features: apple-tv -->\nOld text\n<!-- end generated -->\n\nPersonal closing.\n'
    result = render_readme(source, load_facts())
    assert result.startswith('Personal introduction.\n\n')
    assert result.endswith('\n\nPersonal closing.\n')
    assert '- Full controller support\n' in result
    assert 'Old text' not in result
    assert render_readme(result, load_facts()) == result


def test_check_detects_both_stale_outputs_without_writing(tmp_path):
    from tools.build_site import write_outputs
    readme = tmp_path / 'README.md'
    page = tmp_path / 'index.html'
    readme.write_text('stale README')
    page.write_text('stale site')
    assert write_outputs({readme: 'new README', page: 'new site'}, check=True) == 1
    assert readme.read_text() == 'stale README'
    assert page.read_text() == 'stale site'
    assert write_outputs({readme: 'new README', page: 'new site'}, check=False) == 0
    assert write_outputs({readme: 'new README', page: 'new site'}, check=True) == 0


def test_committed_readme_matches_facts():
    from tools.build_site import render_readme, load_facts
    source = (ROOT / 'README.md').read_text(encoding='utf-8')
    assert source == render_readme(source, load_facts()), 'Run python3 tools/build_site.py'


def test_footer_uses_combined_game_data_notice_and_apple_attribution():
    page = render_site(SOURCE)
    assert 'Apple, iPhone, iPad, Apple TV, Mac, Vision Pro and Metal are trademarks of Apple Inc.' in page
    assert 'You need a disc image of the original Xbox game' in page
    assert 'ships no game data' not in page
    assert 'Apache-2.0</a>' in page


def test_readme_rejects_unbalanced_generated_markers():
    from tools.build_site import render_readme, load_facts
    with pytest.raises(ValueError, match='markers'):
        render_readme('<!-- generated: features: apple-tv -->\nEdited text', load_facts())


def test_feature_requirement_keeps_minor_deployment_version(tmp_path, monkeypatch):
    import tools.build_site as builder
    facts = builder.load_facts()
    (tmp_path / 'tools').mkdir()
    build = (builder.ROOT / 'tools/ios_build.py').read_text(encoding='utf-8')
    build = build.replace("'ios':('iOS','16.0')", "'ios':('iOS','18.5')")
    (tmp_path / 'tools/ios_build.py').write_text(build, encoding='utf-8')
    (tmp_path / 'tools/macos_build.py').write_text((builder.ROOT / 'tools/macos_build.py').read_text(encoding='utf-8'), encoding='utf-8')
    monkeypatch.setattr(builder, 'ROOT', tmp_path)
    assert 'iOS 18.5+' in builder.expand_includes('{{ features: iphone-and-ipad }}', facts)


def test_native_mac_minimum_comes_from_appkit_build_not_catalyst():
    from tools.build_site import load_facts
    assert load_facts()['platforms']['mac']['minimum_os'] == '14.0'
