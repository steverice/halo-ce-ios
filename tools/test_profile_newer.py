"""Newer-device rules preserve product lines and Mac tier floors."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import profile_check
import data_pack


def profile(platform='ios', field='models', names=None):
    return {'schema': 1, 'name': 'Newer', 'kind': 'model',
            'match': {'platform': platform, field: names or ['iPhone18,1'], 'and_newer': True}}


def test_model_threshold_uses_newest_major_and_one_product_line():
    assert profile_check.newer_rule(profile()['match']) == ('iPhone', 18, 0)
    assert profile_check.newer_rule(profile(names=['iPhone17,2', 'iPhone18,1'])['match']) == ('iPhone', 18, 0)
    for match in [profile(names=['iPhone18,1', 'iPad19,1'])['match'],
                  profile(names=['Unknown18,1'])['match'],
                  profile(names=['iPhone999999999999999999999,1'])['match'],
                  profile('tvos')['match'],
                  profile('mac', 'models', ['Mac18,1'])['match']]:
        with pytest.raises(ValueError):
            profile_check.newer_rule(match)


def test_mac_known_tiers_and_mixed_tiers_refused():
    assert profile_check.newer_rule(profile('mac', 'chips', ['Apple M4 Pro'])['match']) == ('Apple M', 4, 1)
    assert profile_check.newer_rule(profile('mac', 'chips', ['Apple M3 Max', 'Apple M4 Max'])['match']) == ('Apple M', 4, 2)
    for names in [['Apple M4 Extreme'], ['Apple M4 Pro', 'Apple M4 Max'], ['Apple M999999999999999999999 Pro']]:
        with pytest.raises(ValueError):
            profile_check.newer_rule(profile('mac', 'chips', names)['match'])


def test_checker_rejects_invalid_or_ambiguous_rules(tmp_path):
    # Use TOML text to exercise the same checker called by signed-pack preparation.
    good = 'schema=1\nname="Newer"\nkind="model"\n[match]\nplatform="ios"\nmodels=["iPhone18,1"]\nand_newer=true\n'
    path = tmp_path / 'newer.toml'
    path.write_text(good)
    assert not profile_check.check_profile(path, profile_check.profile_keys())[0]
    for text in [good.replace('and_newer=true', 'and_newer="true"'),
                 good.replace('models=["iPhone18,1"]', 'models=[]'),
                 good.replace('iPhone18,1', 'Mystery18,1'),
                 good.replace('platform="ios"', 'platform="tvos"'),
                 good.replace('kind="model"', 'kind="class"')]:
        path.write_text(text)
        errors, _ = profile_check.check_profile(path, profile_check.profile_keys())
        assert any('and_newer' in error for error in errors), (text, errors)
    first, second = profile(), profile(names=['iPhone18,2'])
    errors = profile_check.check_set({'one.toml': first, 'two.toml': second})
    assert any('ambiguous and_newer' in error for error in errors)
    second = profile(names=['iPhone19,1'])
    assert not any('and_newer' in error for error in profile_check.check_set({'one.toml': first, 'two.toml': second}))
    mac1, mac2 = profile('mac', 'chips', ['Apple M4 Pro']), profile('mac', 'chips', ['Apple M4 Max'])
    assert not any('and_newer' in error for error in profile_check.check_set({'one.toml': mac1, 'two.toml': mac2}))


@pytest.mark.parametrize("enabled", ["true", "false"])
def test_profiles_1_refuses_newer_rules(tmp_path, enabled):
    import shutil
    shutil.copytree(data_pack.ROOT / 'profiles', tmp_path / 'profiles')
    path = tmp_path / 'profiles/iphone-17-pro.toml'
    text = path.read_text().replace('and_newer = true', f'and_newer = {enabled}')
    path.write_text(text)
    with pytest.raises(ValueError, match='profiles-2'):
        data_pack.snapshot(tmp_path / 'profiles', 'profiles', compatibility='profiles-1')
    assert data_pack.snapshot(tmp_path / 'profiles', 'profiles', compatibility='profiles-2')
