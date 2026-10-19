"""Bound formation and observer input through production helpers and the actual stick consumer."""
from pathlib import Path
import shutil
import subprocess
from unittest import SkipTest

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "port/linux/include/benchmark_mp_arena.h"
INPUT = (ROOT / "source/input/input_abstraction.c").read_text()


def test_signed_formation_bounds_and_actual_observer_look_signs(tmp_path):
    assert HEADER.is_file(), "The generic signed arena helper is not implemented"
    compiler = shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SkipTest("A C compiler is required for the arena probe")
    start = INPUT.index("state->yaw = -right_stick.x;")
    end = INPUT.index("break;", start)
    consumer = INPUT[start:end]
    source = r'''
#include <assert.h>
#include <math.h>
#include "benchmark_mp_arena.h"
static void actual_stick_consumer(short x,short y,int invert_look,float *yaw,float *pitch) {
    struct {float x,y;} right_stick = {x/32767.0f,y/32767.0f};
    struct {float yaw,pitch;} result,*state=&result;
    __CONSUMER__
    *yaw=result.yaw;*pitch=result.pitch;
}
int main(void) {
    struct benchmark_mp_arena_config cfg={6,1.5f,3.5f,0.4f};
    assert(benchmark_mp_arena_valid(&cfg));
    for(int count=2;count<=32;count++) {
        float points[32][2];
        for(int i=0;i<count;i++) {
            assert(benchmark_mp_arena_offset(&cfg,count,i,1,0,points[i]));
            assert(isfinite(points[i][0])&&isfinite(points[i][1]));
            for(int j=0;j<i;j++) assert(hypotf(points[i][0]-points[j][0],points[i][1]-points[j][1])>=1.49f);
        }
    }
    float point[2];
    assert(!benchmark_mp_arena_offset(&cfg,129,1,1,0,point));
    assert(!benchmark_mp_arena_offset(&cfg,32,32,1,0,point));
    assert(!benchmark_mp_arena_offset(&cfg,32,1,NAN,0,point));
    assert(!benchmark_mp_arena_offset(&cfg,32,1,0,0,point));
    cfg.spacing=900/1000.0f;cfg.movement_radius=300/1000.0f;
    assert(benchmark_mp_arena_valid(&cfg));
    cfg.movement_radius=301/1000.0f;assert(!benchmark_mp_arena_valid(&cfg));
    cfg.movement_radius=3;assert(!benchmark_mp_arena_valid(&cfg));
    cfg.movement_radius=NAN;assert(!benchmark_mp_arena_valid(&cfg));
    struct mp_benchmark_probe_pad pad;
    float yaw,pitch;
    for(int inverted=0;inverted<2;inverted++) {
        assert(benchmark_mp_arena_observer_pad(1.0f,0.5f,inverted,10,&pad));
        actual_stick_consumer(pad.right_x,pad.right_y,inverted,&yaw,&pitch);
        assert(yaw>0&&pitch>0&&yaw<=0.7f&&pitch<=0.7f);
        assert(pad.left_x&&pad.fire&&!pad.grenade&&!pad.jump);
        assert(benchmark_mp_arena_observer_pad(-1.0f,-0.5f,inverted,60,&pad));
        actual_stick_consumer(pad.right_x,pad.right_y,inverted,&yaw,&pitch);
        assert(yaw<0&&pitch<0&&!pad.fire);
    }
    assert(!benchmark_mp_arena_observer_pad(NAN,0,0,10,&pad));
    return 0;
}
'''.replace("__CONSUMER__", consumer)
    path, binary = tmp_path / "arena.c", tmp_path / "arena"
    path.write_text(source)
    subprocess.run([compiler,"-O2","-fsanitize=address,undefined","-iquote",str(HEADER.parent),
                    str(path),"-lm","-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
