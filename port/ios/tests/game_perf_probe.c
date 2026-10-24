#include "game_perf.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
static size_t received, gaps;
static void frame(void *ctx,const game_perf_frame *f) { (void)ctx; assert(f->interval_us); received++; }
static void event(void *ctx,const game_perf_event *e) { (void)ctx; if(e->type==GAME_PERF_EVENT_GAP) gaps++; }
static void enrich(void *ctx,game_perf_frame *f) { f->gpu_us=*(uint64_t *)ctx; }
static atomic_bool racing_done;
static atomic_uint racing_phase;
static void *racing_producer(void *value) {
    game_perf_recorder *r=value;game_perf_frame f={0};f.interval_us=3;f.budget_us=2;
    for(uint64_t i=0;i<20000;i++) {
        f.frame_id=i;f.elapsed_us=i*3;game_perf_append_frame(r,&f);
        if(i==9999){atomic_store_explicit(&racing_phase,1,memory_order_release);while(atomic_load_explicit(&racing_phase,memory_order_acquire)==1)sched_yield();}
    }
    atomic_store_explicit(&racing_done,true,memory_order_release);return NULL;
}
int main(void) {
    game_perf_recorder *r=game_perf_create(); assert(r); assert(game_perf_start(r,100));
    game_perf_frame f={0}; f.interval_us=16667; f.budget_us=16666; f.gpu_us=GAME_PERF_UNAVAILABLE;
    for(size_t i=0;i<GAME_PERF_FRAME_CAPACITY+3;i++){ f.elapsed_us=i*16667; f.frame_id=i; game_perf_append_frame(r,&f); }
    game_perf_summary summary; game_perf_get_summary(r,&summary);
    assert(summary.frames[0]==GAME_PERF_FRAME_CAPACITY+3);
    assert(summary.over_budget[0]==GAME_PERF_FRAME_CAPACITY+3);
    assert(summary.metrics[0][5].available==0);
    assert(summary.metrics[0][0].sum_us==(GAME_PERF_FRAME_CAPACITY+3)*16667);
    game_perf_status s; uint64_t watermark=0;
    assert(game_perf_checkpoint_snapshot(r,&summary,&s,&watermark));
    assert(watermark==GAME_PERF_FRAME_CAPACITY+2 && summary.frames[0]==s.frames_seen);
    assert(summary.metrics[0][0].available==s.frames_seen && s.frames_dropped==3);
    game_perf_drain(r,frame,event,NULL); assert(received==GAME_PERF_FRAME_CAPACITY); assert(gaps==1);
    f.flags=GAME_PERF_LOADING; f.gpu_us=1234; f.interval_us=16666;
    assert(game_perf_append_frame(r,&f)); game_perf_drain(r,NULL,NULL,NULL);
    game_perf_get_summary(r,&summary); assert(summary.frames[1]==1);
    assert(summary.over_budget[1]==0 && summary.budget_available[1]==1);
    assert(summary.metrics[1][5].available==1 && summary.metrics[1][5].max_us==1234);
    assert(summary.metrics[1][5].histogram[10]==1);
    f.flags=0; f.gpu_us=GAME_PERF_UNAVAILABLE;
    uint8_t bytes[24]; assert(game_perf_encode_columns(&f,1,bytes,sizeof(bytes))); assert(bytes[16]==255 && bytes[17]==255);
    f.cpu_us=9000000; assert(game_perf_encode_columns(&f,1,bytes,sizeof(bytes))); assert(bytes[22]&GAME_PERF_OVERFLOW);
    game_perf_end(r,500000,GAME_PERF_END_SUSPEND); game_perf_get_status(r,&s); assert(!s.active && s.end_reason==GAME_PERF_END_SUSPEND);
    assert(!game_perf_append_frame(r,&f)); game_perf_drain(r,NULL,event,NULL); uint64_t joined_gpu=555; game_perf_set_enricher(r,enrich,&joined_gpu); assert(game_perf_start(r,600000));
    f.elapsed_us=100; f.interval_us=100000; game_perf_append_frame(r,&f); game_perf_drain(r,NULL,NULL,NULL);
    const game_perf_window *w; assert(game_perf_get_windows(r,&w)==1); assert(w[0].peak_us==100000); assert(w[0].frames[0].gpu_us==555);
    uint8_t chunk[64]; size_t n=game_perf_journal_chunk(chunk,sizeof(chunk),7,"hello",5); uint32_t seq=0;
    assert(n==21); assert(game_perf_journal_recover(chunk,n,&seq)==n && seq==7); assert(game_perf_journal_recover(chunk,n-1,NULL)==0);
    chunk[20]^=1; assert(game_perf_journal_recover(chunk,n,NULL)==0);
    game_perf_end(r,700000,GAME_PERF_END_NORMAL); game_perf_drain(r,NULL,NULL,NULL);
    assert(game_perf_start(r,800000));
    /* Nine separated spikes retain the eight largest windows. */
    for(size_t i=0;i<9;i++) { f.frame_id=i; f.elapsed_us=i*10000000; f.interval_us=60000+i*1000; assert(game_perf_append_frame(r,&f)); game_perf_drain(r,NULL,NULL,NULL); }
    assert(game_perf_get_windows(r,&w)==8);
    uint64_t smallest=UINT64_MAX; for(size_t i=0;i<8;i++) if(w[i].peak_us<smallest) smallest=w[i].peak_us;
    assert(smallest==61000);
    game_perf_event e={0,0,0,GAME_PERF_EVENT_CONTEXT,0};
    for(size_t i=0;i<GAME_PERF_EVENT_CAPACITY+2;i++) game_perf_append_event(r,&e);
    game_perf_get_status(r,&s); assert(s.events_dropped==3);
    game_perf_drain(r,NULL,NULL,NULL); assert(!game_perf_append_event(r,&e));
    game_perf_get_status(r,&s); assert(s.events_dropped==4);
    assert(!game_perf_encode_columns(&f,SIZE_MAX,bytes,sizeof(bytes)));
    assert(!game_perf_encode_columns(&f,1,bytes,23));
    uint8_t chunks[64]; size_t a=game_perf_journal_chunk(chunks,sizeof(chunks),1,"a",1);
    size_t b=game_perf_journal_chunk(chunks+a,sizeof(chunks)-a,2,"b",1);
    assert(game_perf_journal_recover(chunks,a+b,&seq)==a+b && seq==2);
    chunks[a+b-1]^=1; assert(game_perf_journal_recover(chunks,a+b,&seq)==a && seq==1);
    game_perf_summary replay={0}; f.interval_us=100; f.budget_us=99;
    game_perf_summary_append(&replay,&f); assert(replay.frames[0]==1 && replay.over_budget[0]==1);
    assert(replay.metrics[0][0].sum_us==100 && replay.metrics[0][0].histogram[6]==1);
    replay.metrics[0][0].sum_us=UINT64_MAX; game_perf_summary_append(&replay,&f);
    assert(replay.metrics[0][0].sum_us==UINT64_MAX);
    game_perf_destroy(r);
    r=game_perf_create();assert(r && game_perf_start(r,0));pthread_t producer;
    assert(!pthread_create(&producer,NULL,racing_producer,r));
    while(atomic_load_explicit(&racing_phase,memory_order_acquire)!=1)sched_yield();
    assert(game_perf_checkpoint_snapshot(r,&summary,&s,&watermark));
    assert(watermark==9999 && summary.frames[0]==10000 && s.frames_seen==10000);
    atomic_store_explicit(&racing_phase,2,memory_order_release);
    while(!atomic_load_explicit(&racing_done,memory_order_acquire)) {
        if(game_perf_checkpoint_snapshot(r,&summary,&s,&watermark)) {
            assert(summary.frames[0]==s.frames_seen);
            assert(summary.metrics[0][0].available==s.frames_seen);
            assert(summary.metrics[0][0].sum_us==s.frames_seen*3);
            assert(summary.over_budget[0]==s.frames_seen);
            if(s.frames_seen)assert(watermark+1==s.frames_seen);
        }
    }
    assert(!pthread_join(producer,NULL));assert(game_perf_checkpoint_snapshot(r,&summary,&s,&watermark));
    assert(watermark==19999 && summary.frames[0]==20000 && s.frames_seen==20000);
    assert(s.frames_dropped==20000-GAME_PERF_FRAME_CAPACITY);game_perf_destroy(r); return 0;
}
