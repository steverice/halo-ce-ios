#!/usr/bin/env python3
"""Run native ILP32, memory-tracking and SDL audio handoff regressions."""
import re
import shlex
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/ios/probe'
BUILD.mkdir(parents=True, exist_ok=True)

def run(*args):
    subprocess.run([str(a) for a in args], cwd=ROOT, check=True)

clang = '/opt/homebrew/opt/llvm/bin/clang'
lld = '/opt/homebrew/opt/lld/bin/ld.lld'
run(clang, '--target=arm64_32-apple-watchos', '-mcpu=cortex-a53', '-ffixed-x15', '-ffixed-x27',
    '-mllvm', '-aarch64-enable-compress-jump-tables=false', '-fno-stack-protector', '-ffreestanding',
    '-O2', '-S', 'port/ios/tests/guest_probe.c', '-o', BUILD/'probe.darwin.s')
run('python3', 'tools/ios_asm_convert.py', BUILD/'probe.darwin.s', BUILD/'probe.s')
run(clang, '--target=aarch64-none-elf', '-c', BUILD/'probe.s', '-o', BUILD/'probe.o')
run(lld, '-m', 'aarch64linux', '-static', '-nostdlib', '-T', 'port/ios/guest.ld', BUILD/'probe.o', '-o', BUILD/'probe.elf')
run('python3', 'tools/ios_embed_guest.py', BUILD/'probe.elf', BUILD/'embed')
run('xcrun','clang','-O2',f'-I{BUILD}/embed','port/ios/tests/probe_runner.c',
    'port/ios/host/guest_call.S',BUILD/'embed/guest_image.S','-o',BUILD/'runner')
run(BUILD/'runner')
# gpu.h crosses the guest/host boundary (step 4): the host's layout of its structs, as
# _Static_asserts, must hold under the guest's arm64_32 flags too
run('xcrun', 'clang', '-O2', '-Iport/linux/src', 'port/ios/tests/gpu_layout_probe.c', '-o', BUILD / 'gpu-layout-probe')
with open(BUILD / 'gpu_layout_check.h', 'w') as layout_check:
    subprocess.run([str(BUILD / 'gpu-layout-probe')], cwd=ROOT, check=True, stdout=layout_check)
run(clang, '--target=arm64_32-apple-watchos', '-mcpu=cortex-a53', '-ffixed-x15', '-ffixed-x27',
    '-fno-stack-protector', '-ffreestanding', '-Iport/linux/src', '-fsyntax-only', '-x', 'c', BUILD / 'gpu_layout_check.h')
# memory_probe.c includes guest_host.h, which includes gpu.h (port/linux/src)
run('xcrun','clang','-O2','-Iport/ios/host','-Iport/runtime/include','-Iport/linux/src',
    '-Iport/runtime/guest/runtime','port/ios/tests/memory_probe.c','port/ios/host/host_memory.c','-o',BUILD/'memory-probe')
run(BUILD/'memory-probe')
sdl_flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl3'], text=True))
run('xcrun', 'clang', '-O2', '-DHALO_IOS=1', '-Iport/ios/host', '-Iport/ios/host',
    '-Iport/runtime/include', 'port/ios/tests/audio_probe.c', '-Wl,-dead_strip',
    *sdl_flags, '-o', BUILD/'audio-probe')
run(BUILD/'audio-probe')

run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/linux/src',
    'port/ios/tests/display_probe.c', '-o', BUILD/'display-probe')
run(BUILD/'display-probe')

# the native Mac runner's pinned display (host_display_pin.h)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/ios/host',
    'port/ios/tests/display_pin_probe.c', '-o', BUILD/'display-pin-probe')
run(BUILD/'display-pin-probe')
# config.toml: the written file has each table once and parses, and one with a table or key repeated
# keeps the first of each instead of being ignored; once with the iOS and visionOS settings (HALO_ILP32)
# and once with the desktop's (only the address sanitizer: tomlc17's page arithmetic trips the
# undefined-behavior one)
# ... and once as an Apple build (HALO_IOS), whose defaults keep map downloads and map seeding off
for name, defines in (('config-probe-ios', ['-DHALO_ILP32=1']), ('config-probe-desktop', []),
                      ('config-probe-apple', ['-DHALO_ILP32=1', '-DHALO_IOS=1'])):
    run('xcrun', 'clang', '-O2', '-fsanitize=address', *defines, '-Iport/linux/src', '-Iport/third_party/tomlc17',
        'port/ios/tests/config_probe.c', *sdl_flags, '-o', BUILD / name)
    run(BUILD / name)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/linux/src',
    'port/ios/tests/virtual_clock_probe.c', '-o', BUILD/'virtual-clock-probe')
run(BUILD/'virtual-clock-probe')
# halo://join links the app is opened with (host_join_link.h)
run('xcrun', 'clang', '-O2', '-Wall', '-Werror', '-fsanitize=address,undefined', '-Iport/ios/host',
    'port/ios/tests/join_link_probe.c', '-o', BUILD/'join-link-probe')
run(BUILD/'join-link-probe')
# the frame pacer's rules (frame_pacer.h), with made-up refreshes and presented times
run('xcrun', 'clang', '-O2', '-Wall', '-Werror', '-fsanitize=address,undefined', '-Iport/ios/host',
    'port/ios/tests/frame_pacer_probe.c', '-o', BUILD/'frame-pacer-probe')
run(BUILD/'frame-pacer-probe')
# the Metal backend's redundant-state filter (metal_state_cache.h)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/ios/host',
    'port/ios/tests/metal_state_probe.c', '-o', BUILD/'metal-state-probe')
run(BUILD/'metal-state-probe')
# the Metal backend's pipelines compiled off the game thread (metal_compile_async.h): its table
# under the thread sanitizer, then everything under the address and undefined-behavior ones
run('xcrun', 'clang', '-O2', '-Wall', '-Werror', '-fsanitize=thread', '-Iport/ios/host',
    'port/ios/tests/metal_compile_probe.c', '-o', BUILD/'metal-compile-probe-thread')
run(BUILD/'metal-compile-probe-thread')
run('xcrun', 'clang', '-O2', '-Wall', '-Werror', '-fsanitize=address,undefined', '-Iport/ios/host',
    'port/ios/tests/metal_compile_probe.c', '-o', BUILD/'metal-compile-probe')
run(BUILD/'metal-compile-probe')

# debug.texture_override_directory's hash and file checks (port/linux/src/texture_override.h)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/linux/src',
    'port/ios/tests/texture_override_probe.c', '-o', BUILD/'texture-override-probe')
run(BUILD/'texture-override-probe')
# the texture upscale policy's device classifier (port/runtime/texture_policy.c)
run('python3', 'tools/embed_texture_policy.py', 'port/assets/texture-policy.json', BUILD/'texture_policy_table.c')
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/runtime', '-Iport/linux/src',
    'port/ios/tests/texture_policy_probe.c', 'port/runtime/texture_policy.c', BUILD/'texture_policy_table.c',
    '-lz', '-o', BUILD/'texture-policy-probe')
run(BUILD/'texture-policy-probe')
# the texture upscale cache's recipe key, invalidation, writes and storage rules (port/runtime/texture_cache.c)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/runtime', '-Iport/linux/src',
    'port/ios/tests/texture_cache_probe.c', 'port/runtime/texture_cache.c', '-o', BUILD/'texture-cache-probe')
run(BUILD/'texture-cache-probe')
# the per-level upscaled-texture gate (port/linux/src/texture_upscale_state.h)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/linux/src',
    'port/ios/tests/texture_upscale_state_probe.c', '-o', BUILD/'texture-upscale-state-probe')
run(BUILD/'texture-upscale-state-probe')
# the host's write of one setting into config.toml, for the Settings app's switch (port/ios/host/host_config.c)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/ios/host', 'port/ios/tests/host_config_write_probe.c',
    'port/ios/host/host_config.c', '-o', BUILD/'host-config-write-probe')
run(BUILD/'host-config-write-probe')
# cloud saves: the sync rules against a fake cloud shared by several simulated devices (port/runtime/cloud_saves.c)
run('xcrun', 'clang', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
    '-Iport/runtime', 'port/ios/tests/cloud_saves_probe.c', 'port/runtime/cloud_saves.c', '-lz', '-o', BUILD/'cloud-saves-probe')
run(BUILD/'cloud-saves-probe')
# the game's reread of one setting at a level load, the environment still winning (port/linux/src/port_config.c)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-DHALO_ILP32', '-Iport/linux/src',
    '-Iport/third_party/tomlc17', *shlex.split(subprocess.check_output(['pkg-config', '--cflags', 'sdl3'], text=True)),
    'port/ios/tests/config_reload_probe.c', 'port/third_party/tomlc17/tomlc17.c', '-o', BUILD/'config-reload-probe')
run(BUILD/'config-reload-probe')
# bp and bpf on the GPU (port/ios/host/texture_refine.m); skips itself where there is no Metal device
run('xcrun', 'clang', '-O2', '-fobjc-arc', '-Iport/ios/host', 'port/ios/tests/texture_refine_probe.m',
    'port/ios/host/texture_refine.m', '-framework', 'Foundation', '-framework', 'Metal', '-o', BUILD/'texture-refine-probe')
run(BUILD/'texture-refine-probe')
# the render's own local random sequence: frames drawn between ticks leave the game's local
# numbers alone (the probe includes port/linux/game/render_random.c)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/linux/src',
    'port/ios/tests/render_random_probe.c', '-o', BUILD/'render-random-probe')
run(BUILD/'render-random-probe')
# and the call sites the probe can't see: each in-game and pregame frame brackets its work, and
# the stereo frame's passes take their seeds from render_random.c
def function_body(path, name):
    text = (ROOT / path).read_text()
    match = re.search(r'\n(?:static )?void ' + name + r'\(\s*\w[^)]*\)\n\{\n(.*?)\n\}\n', text, re.S)
    if not match:
        raise SystemExit(f'{path}: no function {name}')
    return match.group(1)
for path, name, calls in (
        ('source/main/main.c', 'main_game_render', ('halo_render_random_begin();', 'halo_render_random_end();')),
        ('source/main/main.c', 'main_pregame_render', ('halo_render_random_begin();', 'halo_render_random_end();')),
        ('source/render/render.c', 'render_player_frame_stereo',
         ('halo_render_random_stereo_pass(eye);', 'halo_render_random_stereo_end();'))):
    body = function_body(path, name)
    positions = [body.find(call) for call in calls]
    if -1 in positions or positions != sorted(positions):
        raise SystemExit(f'{path}: {name} must call {" then ".join(calls)}')
# head-tracked stereo's look: the world holds still in the room while the head pans, and
# the right stick's turn (the probe includes port/linux/game/stereo.c)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-DHALO_IOS=1', '-Iport/linux/src', '-Iport/ios/host',
    'port/ios/tests/stereo_head_probe.c', 'port/ios/host/host_stereo_head.c',
    '-o', BUILD/'stereo-head-probe')
run(BUILD/'stereo-head-probe')
# and the call site the probe can't see: stereo latches its frame once a drawn frame, at the render,
# not in render_interpolation_frame_begin, which game_time_update calls around game_frame as well
latch_calls = [path for path in sorted((ROOT / 'source').rglob('*.c')) + sorted((ROOT / 'port/linux').rglob('*.c'))
               for line in path.read_text(errors='replace').splitlines()
               if re.match(r'\s*halo_stereo_frame_begin\(\);', line)]
if latch_calls != [ROOT / 'source/main/main.c'] or not re.search(
        r'halo_stereo_frame_begin\(\);\n\s*render_interpolation_frame_begin\(\);\n\s*main_game_render\(',
        (ROOT / 'source/main/main.c').read_text()):
    raise SystemExit('halo_stereo_frame_begin must be called once, in main.c just before the render\'s '
                     f'render_interpolation_frame_begin; found {[str(path.relative_to(ROOT)) for path in latch_calls]}')
# HEAD mode's HUD and UI layout: each group's outer corner at its CE corner's angles in the
# periphery, the UI inside foveation's sharp region, the reticle centered or along a seat's aim,
# the level frame's yaw
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/ios/host', '-Iport/linux/src',
    'port/ios/tests/stereo_hud_probe.c', 'port/ios/host/host_stereo_hud.c', '-o', BUILD/'stereo-hud-probe')
run(BUILD/'stereo-hud-probe')
# the HUD split by the function that draws each element: nested spans, the catch-all, the corner
# fallback and each frame's reset (the probe includes port/linux/game/hud_group.c)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/linux/src',
    'port/ios/tests/hud_group_probe.c', '-o', BUILD/'hud-group-probe')
run(BUILD/'hud-group-probe')
# and the call sites the probe can't see: every crosshair draw goes through the reticle's
# redirect, and each group's drawing function opens its span
weapon_source = (ROOT / 'source/interface/hud_weapon.c').read_text()
if len(re.findall(r'halo_stereo_reticle_overlay\(TRUE\);[^;]*\n\s*crosshairs_draw\(', weapon_source)) != 2 or \
        len(re.findall(r'\n\t+crosshairs_draw\(', weapon_source)) != 2:
    raise SystemExit('hud_weapon.c: both crosshairs_draw calls must follow halo_stereo_reticle_overlay(TRUE)')
for path, spans in (('source/interface/hud_weapon.c', ('HALO_HUD_GROUP_WEAPON',)),
                    ('source/interface/hud_unit.c', ('HALO_HUD_GROUP_UNIT', 'HALO_HUD_GROUP_TRACKER')),
                    ('source/interface/hud_messaging.c', ('HALO_HUD_GROUP_PROMPT', 'HALO_HUD_GROUP_MESSAGES')),
                    ('source/interface/hud.c', ('HALO_HUD_GROUP_NONE',))):
    text = (ROOT / path).read_text()
    if any(f'halo_hud_group_begin({span});' not in text for span in spans) or \
            text.count('halo_hud_group_begin(') != text.count('halo_hud_group_end()'):
        raise SystemExit(f'{path}: each of {", ".join(spans)} must open a span that ends')
if 'halo_hud_group_corner(corner);' not in (ROOT / 'source/interface/hud_draw.c').read_text():
    raise SystemExit("hud_draw.c: hud_calculate_point must hand the element's corner to halo_hud_group_corner")
if 'halo_hud_group_forget_corner();' not in function_body('source/interface/interface.c', 'interface_draw_hud'):
    raise SystemExit('interface.c: interface_draw_hud must end with halo_hud_group_forget_corner()')
# head-tracked stereo's first-person body: the render-only node matrices with the head and the
# third-person arms collapsed, and when the body draws (the probe includes
# port/linux/game/first_person_body.c)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/linux/src',
    'port/ios/tests/first_person_body_probe.c', '-o', BUILD/'first-person-body-probe')
run(BUILD/'first-person-body-probe')
# SCREEN mode's 3D TV and the film: the one mapping, its ease, the reasons and the framing, and
# the first-person weapon's own eye (the probe includes port/linux/game/stereo.c)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-DHALO_IOS=1', '-Iport/linux/src', '-Iport/ios/host',
    'port/ios/tests/stereo_screen_probe.c', 'port/ios/host/host_stereo_head.c',
    '-o', BUILD/'stereo-screen-probe')
run(BUILD/'stereo-screen-probe')
# the stereo culling frustum's pixel scale: model detail, particles and sprites at mono's, times
# display.lod_scale, and the planes untouched (the probe includes port/linux/game/stereo_lod.c
# and port/linux/game/stereo.c)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/linux/src',
    'port/ios/tests/stereo_lod_probe.c', '-o', BUILD/'stereo-lod-probe')
run(BUILD/'stereo-lod-probe')
# and render.c still hands the culling frustum to it, after building it
render_source = (ROOT / 'source/render/render.c').read_text()
cull_build = render_source.find('render_camera_build_frustum(&cull_camera')
if cull_build < 0 or render_source.find('halo_stereo_lod_projection(&cull_camera, &cull_frustum)', cull_build) < 0:
    raise SystemExit('render.c: render_player_frame_stereo must call halo_stereo_lod_projection(&cull_camera, '
                     '&cull_frustum) after render_camera_build_frustum(&cull_camera, ...)')
# the sky at infinity in each eye: render_sky centers it on the eye's position, which the eye loop sets after the
# mirror's window, around the eye's own render_window, and clears after
eye_set = render_source.find('halo_stereo_set_eye_position(eye < 2 ? &eye_camera.position : NULL);')
eye_window = render_source.find('render_window(', eye_set)
if eye_set < 0 or render_source.rfind('structure_visibility_find_mirror', 0, eye_set) < 0 or \
        render_source.find('halo_stereo_set_eye_position(NULL);', eye_window) < 0:
    raise SystemExit("render.c: render_player_frame_stereo must set the eye's position after the mirror's window, "
                     "around the eye's render_window, and clear it after")
if 'halo_stereo_eye_position()' not in (ROOT / 'source/render/render_sky.c').read_text():
    raise SystemExit("render_sky.c: render_sky must center the sky on the eye's position in an eye pass")
# far scenery (a10's ring) moves with the eye, so it too sits at infinity (the probe includes
# port/linux/game/stereo_far.c), and render_objects.c hands it every object's node matrices
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/linux/src',
    'port/ios/tests/stereo_far_probe.c', '-o', BUILD/'stereo-far-probe')
run(BUILD/'stereo-far-probe')
if 'halo_stereo_far_matrices(' not in (ROOT / 'source/render/render_objects.c').read_text():
    raise SystemExit('render_objects.c: objects must pass their node matrices through halo_stereo_far_matrices')
# HEAD mode's help text: a10's look-only prompts hidden, tutorial_moving_1's look line dropped, and
# every message whole with stereo off (the probe includes port/linux/game/stereo_help_text.c)
run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/linux/src',
    'port/ios/tests/stereo_help_text_probe.c', '-o', BUILD/'stereo-help-text-probe')
run(BUILD/'stereo-help-text-probe')
# and the call site the probe can't see: the help text's draw asks which part to show
messaging_source = (ROOT / 'source/interface/hud_messaging.c').read_text()
if 'halo_stereo_help_text_part(hud_messaging_globals->help_message->name)' not in messaging_source or \
        'halo_stereo_help_text_line_end(' not in messaging_source:
    raise SystemExit('hud_messaging.c: the help text must pass through halo_stereo_help_text_part and _line_end')
if '_port_help_text_all,\n\t_port_help_text_none,\n\t_port_help_text_first_line\n' not in messaging_source or \
        'HALO_HELP_TEXT_ALL,\n\tHALO_HELP_TEXT_NONE,\n\tHALO_HELP_TEXT_FIRST_LINE\n' not in \
        (ROOT / 'port/linux/src/halo_stereo.h').read_text():
    raise SystemExit("hud_messaging.c: its _port_help_text_* values must follow halo_stereo.h's HALO_HELP_TEXT_*")
# the head-tracked presenter's shaders, compiled from its source string at run time: compile
# them here, as the visionOS build's preprocessor leaves them (host_stereo_vignette.h's mask is
# macro text, which a math macro could otherwise rewrite unseen)
run('xcrun', '--sdk', 'xros', 'clang', '-target', 'arm64-apple-xros26.0', '-E', '-P', '-x', 'objective-c',
    '-Iport/ios/host', '-Iport/linux/src', '-Iport/runtime/include', '-Iport/runtime/guest/runtime',
    'port/ios/host/host_stereo.m', '-o', BUILD / 'host_stereo.i')
preprocessed = (BUILD / 'host_stereo.i').read_text()
literals = re.match(r'shader_source =\s*((?:@?"(?:[^"\\]|\\.)*"\s*)+);',
                    preprocessed[preprocessed.index('shader_source ='):])
(BUILD / 'host_stereo.metal').write_text(''.join(
    re.findall(r'"((?:[^"\\]|\\.)*)"', literals.group(1))).encode().decode('unicode_escape'))
run('xcrun', '--sdk', 'xros', 'metal', '-c', BUILD / 'host_stereo.metal', '-o', BUILD / 'host_stereo.air')
# HEAD mode turns in snaps unless the player asks otherwise
if '{ "input.turn", _config_string, "\\"snap\\""' not in (ROOT / 'port/linux/src/port_config.c').read_text():
    raise SystemExit('port_config.c: input.turn must default to "snap"')
# HEAD mode renders through foveation unless the player turns it off: the host's fallback
# (host_theater.m, before the game has written config.toml) and mac_run.py's defaults agree
if '{ "display.foveation", _config_boolean, "true"' not in (ROOT / 'port/linux/src/port_config.c').read_text() or \
        'host_config_string("display.foveation", "true"' not in (ROOT / 'port/ios/host/host_theater.m').read_text():
    raise SystemExit('port_config.c and host_theater.m: display.foveation must default to true')
mac_run_text = (ROOT / 'tools/mac_run.py').read_text()
if '"display.foveation": "true",' not in mac_run_text or '"debug.foveation_eye_passes": "true",' not in mac_run_text:
    raise SystemExit("mac_run.py: DEFAULTS must follow port_config.c's foveation defaults")

# Parse untrusted XISO metadata and exercise extraction/cancellation under sanitizers.
run('python3', 'tools/ios_xiso_test.py')
run('python3', 'tools/test_network_light_state.py')
