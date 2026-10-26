#!/usr/bin/env python3
"""tools/profile_check.py refuses what a device profile must not hold. Run by tools/ios_test.py (plain asserts:
python3 tools/test_profile_check.py)."""
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import profile_check  # noqa: E402

KEYS = profile_check.profile_keys()
GOOD = '''schema = 1
name = "Box"
kind = "model"
[match]
platform = "tvos"
models = ["Box1,1"]
[display]
render_height = 540
[evidence."display.render_height"]
basis = "measured"
why = "At 1080 the beach ran 12 frames a second."
source = "Box runs of b30, 2026-10-12"
'''


def problems(text, name="box.toml"):
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / name
        path.write_text(text)
        return profile_check.check_profile(path, KEYS, drop_in=name == "device-profile.toml")


def refused(text, word, name="box.toml"):
    found, _ = problems(text, name)
    assert any(word in problem for problem in found), (word, found)


assert problems(GOOD)[0] == []
SHADOW = GOOD + '''[debug]
dynamic_resolution_shadow = true
[evidence."debug.dynamic_resolution_shadow"]
basis = "assumed"
why = "Collect virtual planner decisions during a fixed-resolution validation run."
'''
assert problems(SHADOW)[0] == []

# settings that are the player's, and values outside what a setting accepts
refused(GOOD + "[network]\nonline = true\n", "network.online is not a setting a profile may set")
refused(GOOD + "[debug]\nexit_after = 1.0\n", "debug.exit_after is not a setting")
refused(GOOD.replace("render_height = 540", 'render_height = 540\nstereo = "head"'), "display.stereo is not a setting")
refused(GOOD.replace("render_height = 540", "render_height = 99999"), "must be from 0 to 4320")
refused(GOOD.replace("render_height = 540", 'render_height = "540"'), "must be a whole number")
refused(GOOD.replace("render_height = 540", "render_height = true"), "must be a whole number")
for line, word in (('frame_rate = "5"', "must be one of"), ('renderer = "vulkan"', "must be one of"),
                   ("shadow_map_size = 2048", "must be from 128 to 1024"), ("render_quality = 0.0", "must be from 0.1 to 1"),
                   ("render_scale = nan", "must be")):
    refused(GOOD.replace("[display]", "[display]\n" + line), word)
# every value says why; a measured one says where; evidence for nothing is a slip
refused(GOOD.replace("[display]", "[display]\nshadow_map_size = 256"), 'display.shadow_map_size needs an [evidence."display.shadow_map_size"]')
refused(GOOD.replace('source = "Box runs of b30, 2026-10-12"\n', ""), "a measured value needs source")
refused(GOOD.replace('basis = "measured"', 'basis = "certain"'), "basis must be one of")
refused(GOOD.replace("At 1080 the beach ran 12 frames a second.", "faster"), "why must say")
refused(GOOD + '[evidence."display.renderer"]\nbasis = "assumed"\nwhy = "A value this file does not set."\n', "which this profile does not set")
# the heading
refused(GOOD.replace("schema = 1", "schema = 2"), "schema must be 1")
refused(GOOD.replace('kind = "model"', 'kind = "everything"'), 'kind must be "class" or "model"')
refused(GOOD.replace('platform = "tvos"', 'platform = "android"'), "match.platform must be one of")
refused(GOOD.replace('models = ["Box1,1"]\n', ""), "needs match.models or match.chips")
refused(GOOD.replace('kind = "model"', 'kind = "class"'), "a class needs match.gpu_family")
refused(GOOD.replace('name = "Box"', 'name = "Box\nkind'), "does not parse")
refused(GOOD + "[quality.performance]\nrender_height = 480\n", "quality.performance is not a setting")
assert problems(GOOD.replace('models = ["Box1,1"]\n', ""), "device-profile.toml")[0] == []
# scene classes: the classes a build reads, and only the settings that can change while the game runs
SCENE = GOOD + '''[scene.menu.display]
render_scale = 0.75
[evidence."scene.menu.display.render_scale"]
basis = "assumed"
why = "The menu's GPU time leaves room for more pixels."
'''
assert problems(SCENE)[0] == []
refused(SCENE.replace("render_scale = 0.75", "render_scale = 0.75\nshadow_map_size = 128"), "cannot change by scene class")
refused(SCENE.replace("[scene.menu.display]", '[scene."cutscene@a10".display]'), 'scene class "cutscene@a10"')
refused(SCENE.replace("[scene.menu.display]", "[scene.gameplay.display]"), 'scene class "gameplay"')
refused(SCENE.replace('[evidence."scene.menu.display.render_scale"]', '[evidence."display.render_scale"]'),
        'scene.menu.display.render_scale needs an [evidence."scene.menu.display.render_scale"]')
refused(SCENE.replace("render_scale = 0.75", "render_scale = 0.1"), "must be from 0.25 to 1")

# External contexts admit only live output settings and keep distinct evidence.
EXTERNAL = GOOD + '''[external.display]
output_height = 1080
render_height = 720
[external.scene.cutscene.display]
render_scale = 0.75
[evidence."external.display.output_height"]
basis = "assumed"
why = "A conservative output cap before measuring the device."
[evidence."external.display.render_height"]
basis = "assumed"
why = "A conservative render height before measuring the device."
[evidence."external.scene.cutscene.display.render_scale"]
basis = "assumed"
why = "Cutscenes leave room for this external render scale."
'''
assert problems(EXTERNAL)[0] == []
refused(EXTERNAL.replace("output_height = 1080", 'output_height = 1080\nrenderer = "metal"'), "cannot change by output")
# a tvOS profile's own [display] may cap the built-in output; no other platform's, and no scene class's
BUILTIN_CAP = GOOD.replace("render_height = 540", "render_height = 540\noutput_height = 720") + '''[evidence."display.output_height"]
basis = "assumed"
why = "A cap at the render height leaves no upscale pass in the frame."
'''
assert problems(BUILTIN_CAP)[0] == []
refused(BUILTIN_CAP.replace('platform = "tvos"', 'platform = "ios"'), "only in an external section")
refused(BUILTIN_CAP.replace('platform = "tvos"', 'platform = "visionos"'), "only in an external section")
refused(BUILTIN_CAP + "[scene.menu.display]\noutput_height = 720\n", "only in an external section")
refused(EXTERNAL.replace("[external.scene.cutscene.display]", "[external.scene.loading.display]"), 'scene class "loading"')
refused(EXTERNAL.replace("render_scale = 0.75", 'anti_aliasing = "fxaa"'), "cannot change by scene class")

EXTERNAL_MEASURED = EXTERNAL.replace('basis = "assumed"', 'basis = "measured"\nsource = "Wired output report, a10, 2026-10-15"\noutput = "external"')
assert problems(EXTERNAL_MEASURED)[0] == []
refused(EXTERNAL_MEASURED.replace('output = "external"', 'output = "builtin"'), 'output = "external"')
refused(EXTERNAL_MEASURED.replace('output = "external"\n', ''), 'output = "external"')

# across the folder: one profile a model, and each platform's classes covering every GPU family once
def model(names):
    return {"kind": "model", "match": {"platform": "tvos", "models": names}}


def family_class(platform, low, high):
    return {"kind": "class", "match": {"platform": platform, "gpu_family": [low, high]}}


whole = {f"{platform}.toml": family_class(platform, 1, 0) for platform in profile_check.PLATFORMS}
assert profile_check.check_set(whole) == []
assert any("Box1,1 is already named by a.toml" in problem
           for problem in profile_check.check_set({**whole, "a.toml": model(["Box1,1"]), "b.toml": model(["Box1,1"])}))
assert any("platform tvos, GPU family Apple3: no class profile covers it" in problem
           for problem in profile_check.check_set({**whole, "tvos.toml": family_class("tvos", 1, 2)}))
assert any("GPU family Apple2: covered by" in problem
           for problem in profile_check.check_set({**whole, "tvos.toml": family_class("tvos", 1, 2),
                                                   "tvos-new.toml": family_class("tvos", 2, 0)}))
assert any("no class profile for platform mac" in problem
           for problem in profile_check.check_set({name: table for name, table in whole.items() if name != "mac.toml"}))
print("PASS: profile_check.py refuses player settings, out-of-range values, missing evidence, bad headings and scenes")
