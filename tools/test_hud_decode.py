"""Decode real PNG filters and exercise the map-scoped preload worker."""
from pathlib import Path
import struct
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[1]


def png(width, height, filter_kind):
    rows = [bytes((x * 37 + y * 53 + 11) % 256 for x in range(width * 4)) for y in range(height)]
    raw = bytearray()
    def paeth(a, b, c):
        p = a + b - c
        return min((a, b, c), key=lambda v: abs(p - v))
    for y, row in enumerate(rows):
        raw.append(filter_kind)
        for x, value in enumerate(row):
            a = row[x - 4] if x >= 4 else 0
            b = rows[y - 1][x] if y else 0
            c = rows[y - 1][x - 4] if y and x >= 4 else 0
            predictor = (0, a, b, (a + b) // 2, paeth(a, b, c))[filter_kind]
            raw.append((value - predictor) % 256)
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    encoded = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
    expected = b''.join(bytes((r[x+2], r[x+1], r[x], r[x+3])) for r in rows for x in range(0, len(r), 4))
    return encoded, expected


def program():
    source = (ROOT / 'port/linux/src/hud_hires.c').read_text()
    for header in ('platform.h', 'port_config.h', 'xgpu.h'):
        source = source.replace(f'#include "{header}"', '')
    source = source.replace('#include "zlib_prefixed.h"', '#include <zlib.h>')
    prefix = r'''
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "hud_hires.h"
#include "gpu.h"
static int enabled = 1;
static unsigned uploads;
static int available = 512;
static unsigned revision;
static pthread_t render_thread;
static int config_boolean(const char *key) { (void)key; return enabled; }
static unsigned long config_changes(void) { return revision; }
static void platform_log(const char *format, ...) { (void)format; }
long hud_hires_asset_at(unsigned long a, long w, long h) { (void)a;(void)w;(void)h;return 0; }
static void host_thread_background(void) {}
static int host_memory_available_megabytes(void) { return available; }
gpu_texture gpu_texture_create(const struct gpu_texture_description *d) { (void)d; assert(pthread_equal(render_thread,pthread_self())); return ++uploads; }
void gpu_texture_upload(gpu_texture t,uint32_t f,uint32_t l,const void *p,uint32_t n) { (void)t;(void)f;(void)l;(void)p;(void)n; }
void gpu_texture_generate_mipmaps(gpu_texture t,uint32_t f) { (void)t;(void)f; }
'''
    arrays, checks = [], []
    for width in (1, 2, 7):
        for kind in range(5):
            data, expected = png(width, 5, kind)
            name = f'case_{width}_{kind}'
            arrays.append(f'static const unsigned char {name}[] = {{{",".join(map(str,data))}}};')
            arrays.append(f'static const unsigned char {name}_expected[] = {{{",".join(map(str,expected))}}};')
            checks.append(f'''p=png_decode({name},sizeof({name}),&w,&h); assert(p && w=={width} && h==5); assert(!memcmp(p,{name}_expected,sizeof({name}_expected))); free(p);''')
    return prefix + source + '\n' + '\n'.join(arrays), '\n'.join(checks)


def compile_run(tmp_path, source):
    path = tmp_path / 'hud.c'
    path.write_text(source)
    binary = tmp_path / 'hud'
    subprocess.run(['cc', '-std=c11', '-DHALO_IOS=1', '-Wall', '-Wextra', '-Wno-unused-function', '-Wno-unused-const-variable',
                    '-fsanitize=address,undefined', '-pthread', '--include-directory=' + str(ROOT / 'port/linux/src'),
                    str(path), '-lz', '--output=' + str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_png_filters_decode_to_bgra_with_overlapping_compaction(tmp_path):
    source, checks = program()
    compile_run(tmp_path, source + r'''
const struct hud_hires_embedded hud_hires_embedded[] = {{0}};
const unsigned int hud_hires_embedded_count = 1;
int main(void) { unsigned char *p; unsigned long w,h;
''' + checks + '\nreturn 0; }')


def test_preload_ownership_fallback_budget_and_map_lifetime(tmp_path):
    source, _ = program()
    data, _ = png(7, 5, 4)
    source += '\nstatic const unsigned char image[] = {' + ','.join(map(str, data)) + '};\n'
    source += r'''
const struct hud_hires_embedded hud_hires_embedded[] = {
 {"ui\\hud\\bitmaps\\combined\\hud_reticles",0,7,5,0,0,0,0,(const unsigned int *)image,sizeof(image)},
 {"ui\\hud\\bitmaps\\combined\\hud_unit_meters",0,7,5,0,0,0,0,(const unsigned int *)image,sizeof(image)},
 {"ui\\hud\\bitmaps\\combined\\hud_unit_backgrounds",2,7,5,0,0,0,0,(const unsigned int *)image,sizeof(image)},
 {"ui\\hud\\bitmaps\\scope",0,7,5,0,0,0,0,(const unsigned int *)image,sizeof(image)},
 {"ui\\hud\\bitmaps\\combined\\hud_unit_backgrounds",6,7,5,0,0,0,0,(const unsigned int *)image,sizeof(image)},
};
const unsigned int hud_hires_embedded_count = 5;
static void wait_ready(long asset) {
 pthread_mutex_lock(&preload_lock);
 while(preload[asset].state == PRELOAD_PENDING || preload[asset].state == PRELOAD_DECODING)
  pthread_cond_wait(&preload_condition,&preload_lock);
 pthread_mutex_unlock(&preload_lock);
}
int main(void) {
 long ids[]={0,1,2,3,4}; unsigned long levels,bytes;
 render_thread=pthread_self();
 hud_hires_preload(ids,5); wait_ready(4);
 assert(preload[0].pixels && preload[1].pixels && preload[2].pixels && preload[4].pixels);
 assert(!preload[3].pixels); /* Scope sheets are not speculative allocations. */
 bytes=preload_bytes;
 assert(hud_hires_override_texture(0,&levels) && levels==3 && uploads==1);
 assert(!preload[0].pixels && preload_bytes==bytes-5*(7*4+1));
 assert(hud_hires_override_texture(0,&levels)==1 && uploads==1);
 assert(hud_hires_override_texture(3,&levels) && uploads==2);
 hud_hires_preload_reset(); assert(!preload_bytes && !preload_started && !preload[1].pixels);
 /* Both low headroom and the committed setting suppress speculative work. */
 memset(textures,0,sizeof(textures)); available=127;
 hud_hires_preload(ids,5); assert(!preload_started);
 assert(hud_hires_override_texture(0,&levels));
 available=512; enabled=0;
 hud_hires_preload(ids,5); assert(!preload_started);
 enabled=1;
 /* Repeated unload during decode must leave no buffers or stale publication. */
 for(int i=0;i<40;i++) { memset(textures,0,sizeof(textures)); hud_hires_preload(ids,5); hud_hires_preload_reset(); assert(!preload_bytes); }
 /* A speculative allocation failure must not mark an image permanently failed. */
 memset(textures,0,sizeof(textures));
 preload[1].state=PRELOAD_PENDING; preload_bytes=PRELOAD_LIMIT; preload_stop=0;
 preload_worker(NULL); assert(!preload[1].pixels && !textures[1].failed);
 assert(hud_hires_override_texture(1,&levels));
 hud_hires_preload_reset();
 /* A CRC mismatch still leaves native map pixels selected. */
 revision++; assert(hud_hires_override_find((unsigned long)image,7,5,sizeof(image))==-1);
 return 0;
}
'''
    compile_run(tmp_path, source)


def test_active_decode_demand_failure_and_real_allocation_cap(tmp_path):
    source, _ = program()
    prefix, production = source.split('/*\nHUD_HIRES.C', 1)
    hooks = r'''
#include <zlib.h>
#include <stddef.h>
static pthread_mutex_t probe_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t probe_condition=PTHREAD_COND_INITIALIZER;
static int pause_decode, entered, released, fail_worker, fail_create, fail_allocation;
static unsigned worker_calls, render_calls;
static size_t live_bytes, peak_bytes;
union allocation { max_align_t align; size_t size; };
static void *tracked_malloc(size_t size) {
 union allocation *p;
 pthread_mutex_lock(&probe_lock);
 if(fail_allocation && !pthread_equal(render_thread,pthread_self())) { pthread_mutex_unlock(&probe_lock); return NULL; }
 p=malloc(sizeof(*p)+size);
 if(p) { p->size=size; live_bytes+=size; if(live_bytes>peak_bytes) peak_bytes=live_bytes; }
 pthread_mutex_unlock(&probe_lock);
 return p ? p+1 : NULL;
}
static void tracked_free(void *value) {
 if(value) { union allocation *p=(union allocation *)value-1; pthread_mutex_lock(&probe_lock); live_bytes-=p->size; pthread_mutex_unlock(&probe_lock); free(p); }
}
static int probe_uncompress(Bytef *out,uLongf *size,const Bytef *in,uLong count) {
 int failed;
 pthread_mutex_lock(&probe_lock);
 if(!pthread_equal(render_thread,pthread_self())) {
  worker_calls++; entered=1; pthread_cond_broadcast(&probe_condition);
  while(pause_decode && !released) pthread_cond_wait(&probe_condition,&probe_lock);
  failed=fail_worker;
 } else { render_calls++; failed=0; }
 pthread_mutex_unlock(&probe_lock);
 return failed ? Z_MEM_ERROR : uncompress(out,size,in,count);
}
static int probe_create(pthread_t *t,const pthread_attr_t *a,void *(*f)(void *),void *v) {
 return fail_create ? 11 : pthread_create(t,a,f,v);
}
static void *release_worker(void *unused) {
 struct timespec delay={0,5000000}; (void)unused; nanosleep(&delay,NULL);
 pthread_mutex_lock(&probe_lock); released=1; pthread_cond_broadcast(&probe_condition); pthread_mutex_unlock(&probe_lock); return NULL;
}
'''
    production = production.replace('malloc(', 'tracked_malloc(').replace('free(', 'tracked_free(')
    production = production.replace('result = uncompress(', 'result = probe_uncompress(').replace('if (pthread_create(', 'if (probe_create(')
    # Two full 2048 sheets cannot coexist inside the speculative budget.
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 2048, 2048, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(bytes(2048 * (2048 * 4 + 1)))) + chunk(b'IEND', b'')
    source = prefix + hooks + '/*\nHUD_HIRES.C' + production
    source += '\nstatic const unsigned char image[] = {' + ','.join(map(str, data)) + '};\n'
    source += r'''
const struct hud_hires_embedded hud_hires_embedded[] = {
 {"ui\\hud\\bitmaps\\combined\\hud_reticles",0,2048,2048,0,0,0,0,(const unsigned int *)image,sizeof(image)},
 {"ui\\hud\\bitmaps\\combined\\hud_unit_meters",0,2048,2048,0,0,0,0,(const unsigned int *)image,sizeof(image)},
};
const unsigned int hud_hires_embedded_count = 2;
static void wait_done(void) { pthread_join(preload_thread,NULL); preload_started=0; }
/* LOAD_FINISH_HEAD */
int main(void) {
 long ids[]={0,1}; unsigned long levels; pthread_t release;
 render_thread=pthread_self(); pause_decode=1;
 hud_hires_preload(ids,2);
 pthread_mutex_lock(&probe_lock); while(!entered) pthread_cond_wait(&probe_condition,&probe_lock); pthread_mutex_unlock(&probe_lock);
 /* A queued demand decodes immediately rather than waiting behind asset 0. */
 assert(hud_hires_override_texture(1,&levels) && render_calls==1);
 pthread_create(&release,NULL,release_worker,NULL);
 /* Active demand waits for that decode, without decoding a second time. */
 assert(hud_hires_override_texture(0,&levels) && worker_calls==1 && render_calls==1);
 pthread_join(release,NULL); hud_hires_preload_reset(); assert(live_bytes==0);
 memset(textures,0,sizeof(textures)); pause_decode=0; worker_calls=render_calls=0; peak_bytes=0;
 hud_hires_preload(ids,2); wait_done();
 assert(preload[0].pixels && !preload[1].pixels);
 assert(preload_bytes<=PRELOAD_LIMIT && peak_bytes+PRELOAD_STACK+PRELOAD_INFLATE<=PRELOAD_LIMIT);
 hud_hires_preload_reset(); assert(live_bytes==0);
 for(int failure=0;failure<3;failure++) {
  memset(textures,0,sizeof(textures));
  fail_worker=failure==0; fail_create=failure==1; fail_allocation=failure==2;
  hud_hires_preload(ids,2); if(preload_started) wait_done();
  assert(!preload[0].pixels && !textures[0].failed);
  assert(hud_hires_override_texture(0,&levels));
  hud_hires_preload_reset(); assert(live_bytes==0);
 }
 /* Loading completion must await only finite CPU work, retaining its result. */
 memset(textures,0,sizeof(textures)); fail_worker=fail_create=fail_allocation=0;
 pause_decode=1; released=entered=0;
 hud_hires_preload(ids,1);
 pthread_mutex_lock(&probe_lock); while(!entered) pthread_cond_wait(&probe_condition,&probe_lock); pthread_mutex_unlock(&probe_lock);
 pthread_create(&release,NULL,release_worker,NULL);
 game_initialize_for_new_map();
 assert(preload[0].state==PRELOAD_READY && preload[0].pixels && !preload_started);
 { size_t held=preload_bytes; unsigned char *pixels=preload[0].pixels;
   game_initialize_for_new_map(); assert(preload_bytes==held && preload[0].pixels==pixels); }
 pthread_join(release,NULL); hud_hires_preload_reset(); assert(live_bytes==0);
 return 0;
}
'''
    game = (ROOT / 'source/game/game.c').read_text()
    start = game.index('\tnetwork_objects_placed();', game.index('void game_initialize_for_new_map('))
    tail = game[start:game.index('\n}', start)]
    head = 'void host_astc_game_started(void) {}\nstatic void network_objects_placed(void) {}\nstatic void ui_widgets_safe_to_load(int value) { (void)value; }\n#define TRUE 1\nvoid game_initialize_for_new_map(void) {\n' + tail + '\n}\n'
    source = source.replace('/* LOAD_FINISH_HEAD */', head)
    compile_run(tmp_path, source)


def test_mac_catalyst_reports_unknown_memory_limit(tmp_path):
    source = (ROOT / 'port/ios/host/host_memory_pressure.m').read_text()
    start = source.index('int host_memory_available_megabytes(void)')
    function = source[start:source.index('\n}\n', start) + 3]
    for native, catalyst, available, expected in ((0, 1, 0, -1), (1, 0, 0, -1), (0, 0, 0, 0), (0, 0, 127, 127), (0, 0, 512, 512)):
        path = tmp_path / 'headroom.c'
        path.write_text(f"""#include <assert.h>
#include <stddef.h>
#define HALO_NATIVE_MAC {native}
#undef TARGET_OS_MACCATALYST
#define TARGET_OS_MACCATALYST {catalyst}
static size_t os_proc_available_memory(void) {{ return (size_t){available} << 20; }}
""" + function + f'\nint main(void) {{ assert(host_memory_available_megabytes()=={expected}); }}\n')
        binary = tmp_path / 'headroom'
        subprocess.run(['cc', str(path), '--output=' + str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
