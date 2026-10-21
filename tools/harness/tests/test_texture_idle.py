"""The texture cache drops a texture not drawn for half a minute of time, at any frame rate."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run

CASES = ['fast', 'steady', 'late', 'profile', 'debug', 'long', 'trim', 'no_trim']
SOURCE = 'port/linux/src/xbox_textures.c'


def generated(fault=None, config_fault=None):
    source = read(SOURCE)
    config = '\n'.join(re.findall(r'^#define TEXTURE_\w+.*$', source, re.M) + re.findall(
        r'^static (?:unsigned long|Uint64|int) texture_(?:frame|drop_serial|sweep_\w+|next_sweep|trim_\w+)\b.*;$', source, re.M)
        + re.findall(r'^long \(\*xgpu_available_memory_hook\)\(void\);$', source, re.M))
    code = (function(source, 'texture_idle_sweeps') + function(source, 'texture_gpu_bytes') + function(source, 'texture_drop_before')
            + function(source, 'xgpu_texture_cache_request_trim') + function(source, 'texture_trim_if_requested') + function(source, 'xgpu_texture_cache_begin_frame'))
    if config_fault:
        config = mutated(config, *config_fault)
    if fault:
        code = mutated(code, *fault)
    return (('config.inc', config + '\n'), ('under_test.inc', code))


@pytest.mark.parametrize('case', CASES)
def test_case(case):
    status, output = run(build('texture_idle', generated()), case)
    assert status == 0, output


@pytest.mark.parametrize('fault,case', [
    # a sweep every frame
    (('texture_next_sweep = now + TEXTURE_SWEEP_SECONDS * 1000;', 'texture_next_sweep = now;'), 'fast'),
    # the profile's idle time ignored
    (('seconds = config_real("display.texture_idle_seconds");', 'seconds = 30.0;'), 'profile'),
    # the debug value ignored
    (('if (seconds < 0.0)', 'if (1)'), 'debug'),
    # the sweep's own frame taken as the one half a minute before
    (('\tidle_since = texture_sweep_count < sweeps ? 0 :\n'
      '\t\ttexture_sweep_frames[(texture_sweep_count - sweeps) % TEXTURE_SWEEP_COUNT];\n'
      '\ttexture_sweep_frames[texture_sweep_count % TEXTURE_SWEEP_COUNT] = texture_frame;\n',
      '\ttexture_sweep_frames[texture_sweep_count % TEXTURE_SWEEP_COUNT] = texture_frame;\n'
      '\tidle_since = texture_frame + 0 * sweeps;\n'), 'fast'),
    # the sweeps not counted: the one before taken each time
    (('\ttexture_sweep_count++;\n', '\n'), 'late'),
    # a warning that drops every texture
    (('texture_drop_before(texture_trim_frames[0], &bytes);', 'texture_drop_before(texture_frame + 1, &bytes);'), 'trim'),
    # a warning that drops nothing
    (('texture_drop_before(texture_trim_frames[0], &bytes);', 'texture_drop_before(0, &bytes);'), 'trim'),
    # a request that is never consumed
    (('__sync_lock_test_and_set(&texture_trim_requested, 0)', 'texture_trim_requested'), 'trim'),
    # a trim without a warning
    (('if (!__sync_lock_test_and_set(&texture_trim_requested, 0))', 'if (!texture_frame)'), 'trim'),
])
def test_negative_control(fault, case):
    status, output = run(build('texture_idle', generated(fault)), case)
    assert status == CHECK_FAILED, output


def test_negative_control_sweeps_remembered():
    # too few sweeps remembered for the longest idle time
    fault = ('#define TEXTURE_SWEEP_COUNT 60', '#define TEXTURE_SWEEP_COUNT 30')
    status, output = run(build('texture_idle', generated(config_fault=fault)), 'long')
    assert status == CHECK_FAILED, output
