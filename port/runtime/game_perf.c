#include "game_perf.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#define DETAIL_RING 2048
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "frame counters must be lock-free");
typedef struct atomic_metric_summary {
    _Atomic uint64_t available, sum_us, max_us;
    _Atomic uint64_t histogram[GAME_PERF_HISTOGRAM_BINS];
} atomic_metric_summary;
_Static_assert(sizeof(game_perf_frame)*GAME_PERF_WINDOW_FRAMES <= 128*1024, "window budget");
struct game_perf_recorder {
    game_perf_frame_enricher enrich;
    void *enrich_context;
    game_perf_frame frames[GAME_PERF_FRAME_CAPACITY];
    game_perf_event events[GAME_PERF_EVENT_CAPACITY];
    _Atomic uint64_t frame_write, frame_read, event_write, event_read;
    _Atomic uint64_t checkpoint_version, last_frame_id;
    _Atomic uint64_t frames_seen, frames_dropped, events_dropped, detail_dropped;
    _Atomic uint64_t first_gap_us, last_gap_us, start_us, end_us;
    _Atomic uint32_t active, end_reason;
    _Atomic uint64_t summary_frames[2], over_budget[2], budget_available[2];
    atomic_metric_summary metrics[2][GAME_PERF_METRIC_COUNT];
    uint64_t reported_drops, reported_events;
    uint32_t events_accepted;
    game_perf_frame ring[DETAIL_RING];
    size_t ring_head, ring_count, window_count;
    game_perf_window windows[GAME_PERF_WINDOW_COUNT];
};
_Static_assert(sizeof(struct game_perf_recorder) < 4*1024*1024,"recorder budget");
game_perf_recorder *game_perf_create(void) { return calloc(1,sizeof(game_perf_recorder)); }
void game_perf_destroy(game_perf_recorder *r) { free(r); }
void game_perf_set_enricher(game_perf_recorder *r,game_perf_frame_enricher callback,void *context) {
    if(!r)return;
    r->enrich=callback;r->enrich_context=context;
}
bool game_perf_append_event(game_perf_recorder *r,const game_perf_event *e) {
    if(!r || !e || !atomic_load_explicit(&r->active,memory_order_relaxed)) return false;
    uint64_t w=atomic_load_explicit(&r->event_write,memory_order_relaxed);
    uint64_t rd=atomic_load_explicit(&r->event_read,memory_order_acquire);
    if(r->events_accepted==GAME_PERF_EVENT_CAPACITY || w-rd==GAME_PERF_EVENT_CAPACITY) { atomic_fetch_add_explicit(&r->events_dropped,1,memory_order_relaxed); return false; }
    r->events[w%GAME_PERF_EVENT_CAPACITY]=*e; r->events_accepted++;
    atomic_store_explicit(&r->event_write,w+1,memory_order_release); return true;
}
bool game_perf_start(game_perf_recorder *r,uint64_t start) {
    if(!r || atomic_load(&r->active) || atomic_load(&r->frame_read)!=atomic_load(&r->frame_write) || atomic_load(&r->event_read)!=atomic_load(&r->event_write)) return false;
    /* The caller has stopped the worker; preserve allocated storage. */
    game_perf_frame_enricher enrich=r->enrich;void *context=r->enrich_context;
    memset(r,0,sizeof(*r));r->enrich=enrich;r->enrich_context=context; atomic_store(&r->start_us,start); atomic_store(&r->active,1);
    game_perf_event e={0,0,0,GAME_PERF_EVENT_START,0}; game_perf_append_event(r,&e); return true;
}
void game_perf_end(game_perf_recorder *r,uint64_t end,game_perf_end_reason reason) {
    if(!r || !atomic_load(&r->active)) return;
    uint64_t start=atomic_load(&r->start_us);
    game_perf_event e={end>=start?end-start:0,reason,0,GAME_PERF_EVENT_END,0};
    game_perf_append_event(r,&e); atomic_store(&r->end_us,end); atomic_store(&r->end_reason,reason); atomic_store_explicit(&r->active,0,memory_order_release);
}
static unsigned histogram_bin(uint64_t value) {
    unsigned bin=0;
    while(value>1) { value>>=1; bin++; }
    return bin;
}
static uint64_t saturating_add(uint64_t a,uint64_t b) { return a>UINT64_MAX-b?UINT64_MAX:a+b; }
static void frame_values(const game_perf_frame *f,uint64_t values[GAME_PERF_METRIC_COUNT]) {
    values[0]=f->interval_us; values[1]=f->cpu_us; values[2]=f->simulation_us;
    values[3]=f->network_us; values[4]=f->render_us; values[5]=f->gpu_us; values[6]=f->budget_us;
}
void game_perf_summary_append(game_perf_summary *s,const game_perf_frame *f) {
    if(!s || !f) return;
    unsigned group=(f->flags&GAME_PERF_LOADING)?1:0; uint64_t values[GAME_PERF_METRIC_COUNT]; frame_values(f,values);
    s->frames[group]++;
    if(f->budget_us!=GAME_PERF_UNAVAILABLE && f->interval_us!=GAME_PERF_UNAVAILABLE) {
        s->budget_available[group]++; if(f->interval_us>f->budget_us) s->over_budget[group]++;
    }
    for(unsigned i=0;i<GAME_PERF_METRIC_COUNT;i++) {
        uint64_t value=values[i]; if(value==GAME_PERF_UNAVAILABLE) continue;
        game_perf_metric_summary *m=&s->metrics[group][i]; m->available++;
        m->sum_us=saturating_add(m->sum_us,value); if(value>m->max_us) m->max_us=value;
        m->histogram[histogram_bin(value)]++;
    }
}
static void summarize(game_perf_recorder *r,const game_perf_frame *f) {
    unsigned group=(f->flags&GAME_PERF_LOADING)?1:0; uint64_t values[GAME_PERF_METRIC_COUNT]; frame_values(f,values);
    atomic_fetch_add_explicit(&r->summary_frames[group],1,memory_order_relaxed);
    if(f->budget_us!=GAME_PERF_UNAVAILABLE && f->interval_us!=GAME_PERF_UNAVAILABLE) {
        atomic_fetch_add_explicit(&r->budget_available[group],1,memory_order_relaxed);
        if(f->interval_us>f->budget_us) atomic_fetch_add_explicit(&r->over_budget[group],1,memory_order_relaxed);
    }
    for(unsigned i=0;i<GAME_PERF_METRIC_COUNT;i++) {
        uint64_t value=values[i]; if(value==GAME_PERF_UNAVAILABLE) continue;
        atomic_metric_summary *m=&r->metrics[group][i];
        atomic_fetch_add_explicit(&m->available,1,memory_order_relaxed);
        /* Only the producer writes these counters; readers use atomic loads. */
        atomic_store_explicit(&m->sum_us,saturating_add(atomic_load_explicit(&m->sum_us,memory_order_relaxed),value),memory_order_relaxed);
        if(value>atomic_load_explicit(&m->max_us,memory_order_relaxed)) atomic_store_explicit(&m->max_us,value,memory_order_relaxed);
        atomic_fetch_add_explicit(&m->histogram[histogram_bin(value)],1,memory_order_relaxed);
    }
}
void game_perf_get_summary(const game_perf_recorder *r,game_perf_summary *s) {
    if(!r || !s) return;
    for(unsigned group=0;group<2;group++) {
        s->frames[group]=atomic_load_explicit(&r->summary_frames[group],memory_order_relaxed);
        s->over_budget[group]=atomic_load_explicit(&r->over_budget[group],memory_order_relaxed);
        s->budget_available[group]=atomic_load_explicit(&r->budget_available[group],memory_order_relaxed);
        for(unsigned i=0;i<GAME_PERF_METRIC_COUNT;i++) {
            const atomic_metric_summary *m=&r->metrics[group][i]; game_perf_metric_summary *out=&s->metrics[group][i];
            out->available=atomic_load_explicit(&m->available,memory_order_relaxed);
            out->sum_us=atomic_load_explicit(&m->sum_us,memory_order_relaxed);
            out->max_us=atomic_load_explicit(&m->max_us,memory_order_relaxed);
            for(unsigned j=0;j<GAME_PERF_HISTOGRAM_BINS;j++) out->histogram[j]=atomic_load_explicit(&m->histogram[j],memory_order_relaxed);
        }
    }
}
bool game_perf_checkpoint_snapshot(const game_perf_recorder *r,game_perf_summary *summary,game_perf_status *status,uint64_t *last_frame_id) {
    if(!r || !summary || !status || !last_frame_id)return false;
    for(unsigned attempt=0;attempt<3;attempt++) {
        uint64_t before=atomic_load_explicit(&r->checkpoint_version,memory_order_acquire);
        if(before&1)continue;
        game_perf_get_summary(r,summary);game_perf_get_status(r,status);
        uint64_t last=atomic_load_explicit(&r->last_frame_id,memory_order_relaxed);
        /* The producer never waits for this snapshot. Retry if any frame crossed
         * these reads, rather than persisting a summary with a wrong watermark. */
        atomic_thread_fence(memory_order_acq_rel);
        uint64_t after=atomic_load_explicit(&r->checkpoint_version,memory_order_acquire);
        if(before==after && !(after&1)){*last_frame_id=last;return true;}
    }
    return false;
}
bool game_perf_append_frame(game_perf_recorder *r,const game_perf_frame *f) {
    if(!r || !f || !atomic_load_explicit(&r->active,memory_order_relaxed)) return false;
    atomic_fetch_add_explicit(&r->checkpoint_version,1,memory_order_acq_rel);
    /* Pairs with the worker fence through relaxed summary/status reads, so a
     * snapshot that observes new fields cannot retain the old even version. */
    atomic_thread_fence(memory_order_release);
    summarize(r,f);
    atomic_fetch_add_explicit(&r->frames_seen,1,memory_order_relaxed);
    uint64_t w=atomic_load_explicit(&r->frame_write,memory_order_relaxed);
    uint64_t rd=atomic_load_explicit(&r->frame_read,memory_order_acquire);
    if(w-rd==GAME_PERF_FRAME_CAPACITY) {
        if(atomic_fetch_add_explicit(&r->frames_dropped,1,memory_order_relaxed)==0) atomic_store_explicit(&r->first_gap_us,f->elapsed_us,memory_order_relaxed);
        atomic_store_explicit(&r->last_gap_us,f->elapsed_us,memory_order_release);
        atomic_store_explicit(&r->last_frame_id,f->frame_id,memory_order_relaxed);
        atomic_fetch_add_explicit(&r->checkpoint_version,1,memory_order_release);return false;
    }
    r->frames[w%GAME_PERF_FRAME_CAPACITY]=*f;
    atomic_store_explicit(&r->frame_write,w+1,memory_order_release);
    atomic_store_explicit(&r->last_frame_id,f->frame_id,memory_order_relaxed);
    atomic_fetch_add_explicit(&r->checkpoint_version,1,memory_order_release);return true;
}
static void window_add(game_perf_recorder *r,game_perf_window *w,const game_perf_frame *f) {
    if(w->frame_count && w->frames[w->frame_count-1].frame_id==f->frame_id) return;
    if(w->frame_count==GAME_PERF_WINDOW_FRAMES) { w->truncated=1; atomic_fetch_add(&r->detail_dropped,1); return; }
    w->frames[w->frame_count++]=*f;
}
static void detail_frame(game_perf_recorder *r,const game_perf_frame *f) {
    for(size_t i=0;i<r->window_count;i++) if(f->elapsed_us>=r->windows[i].begin_us && f->elapsed_us<=r->windows[i].end_us) window_add(r,&r->windows[i],f);
    uint64_t threshold=f->budget_us==GAME_PERF_UNAVAILABLE || f->budget_us>UINT64_MAX/2 ? 50000 : f->budget_us*2;
    if(threshold<50000) threshold=50000;
    if(f->interval_us!=GAME_PERF_UNAVAILABLE && f->interval_us>threshold) {
        uint64_t begin=f->elapsed_us>3000000?f->elapsed_us-3000000:0;
        uint64_t end=f->elapsed_us>UINT64_MAX-2000000?UINT64_MAX:f->elapsed_us+2000000;
        size_t selected=GAME_PERF_WINDOW_COUNT;
        for(size_t i=0;i<r->window_count;i++) if(begin<=r->windows[i].end_us && end>=r->windows[i].begin_us) { selected=i; break; }
        if(selected==GAME_PERF_WINDOW_COUNT) {
            if(r->window_count<GAME_PERF_WINDOW_COUNT) selected=r->window_count++;
            else { selected=0; for(size_t i=1;i<r->window_count;i++) if(r->windows[i].peak_us<r->windows[selected].peak_us) selected=i;
                if(f->interval_us<=r->windows[selected].peak_us) { atomic_fetch_add(&r->detail_dropped,1); selected=GAME_PERF_WINDOW_COUNT; }
            }
            if(selected<GAME_PERF_WINDOW_COUNT) memset(&r->windows[selected],0,sizeof(r->windows[selected]));
        }
        if(selected<GAME_PERF_WINDOW_COUNT) {
            game_perf_window *w=&r->windows[selected];
            if(!w->frame_count || begin<w->begin_us) w->begin_us=begin;
            if(end>w->end_us) w->end_us=end;
            if(f->interval_us>w->peak_us) w->peak_us=f->interval_us;
            if(!w->frame_count) for(size_t j=0;j<r->ring_count;j++) {
                const game_perf_frame *old=&r->ring[(r->ring_head+DETAIL_RING-r->ring_count+j)%DETAIL_RING];
                if(old->elapsed_us>=begin) window_add(r,w,old);
            }
            window_add(r,w,f);
        }
    }
    while(r->ring_count) { size_t oldest=(r->ring_head+DETAIL_RING-r->ring_count)%DETAIL_RING;
        if(f->elapsed_us-r->ring[oldest].elapsed_us<=8000000) break; r->ring_count--; }
    r->ring[r->ring_head]=*f; r->ring_head=(r->ring_head+1)%DETAIL_RING;
    if(r->ring_count<DETAIL_RING) r->ring_count++; else atomic_fetch_add(&r->detail_dropped,1);
}
size_t game_perf_drain(game_perf_recorder *r,game_perf_frame_callback fc,game_perf_event_callback ec,void *ctx) {
    if(!r) return 0; size_t count=0;
    uint64_t rd=atomic_load_explicit(&r->frame_read,memory_order_relaxed),w=atomic_load_explicit(&r->frame_write,memory_order_acquire);
    while(rd<w) {
        game_perf_frame frame=r->frames[rd%GAME_PERF_FRAME_CAPACITY];
        if(r->enrich)r->enrich(r->enrich_context,&frame);
        detail_frame(r,&frame);if(fc)fc(ctx,&frame);rd++;count++;
    }
    atomic_store_explicit(&r->frame_read,rd,memory_order_release);
    rd=atomic_load_explicit(&r->event_read,memory_order_relaxed); w=atomic_load_explicit(&r->event_write,memory_order_acquire);
    while(rd<w) { if(ec) ec(ctx,&r->events[rd%GAME_PERF_EVENT_CAPACITY]); rd++; }
    atomic_store_explicit(&r->event_read,rd,memory_order_release);
    uint64_t drops=atomic_load_explicit(&r->frames_dropped,memory_order_acquire),events=atomic_load(&r->events_dropped);
    if(drops!=r->reported_drops || events!=r->reported_events) { game_perf_event e={atomic_load(&r->last_gap_us),drops-r->reported_drops,events-r->reported_events,GAME_PERF_EVENT_GAP,GAME_PERF_GAP}; if(ec) ec(ctx,&e); r->reported_drops=drops; r->reported_events=events; }
    return count;
}
void game_perf_get_status(const game_perf_recorder *r,game_perf_status *s) {
    if(!r || !s) return;
#define READ(x) s->x=atomic_load(&r->x)
    READ(frames_seen); READ(frames_dropped); READ(events_dropped); READ(detail_dropped); READ(first_gap_us); READ(last_gap_us); READ(start_us); READ(end_us); READ(active); READ(end_reason);
#undef READ
}
size_t game_perf_get_windows(const game_perf_recorder *r,const game_perf_window **windows) { if(!r || !windows) return 0; *windows=r->windows; return r->window_count; }
static void put32(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint16_t duration(uint64_t us,uint16_t *flags) { if(us==GAME_PERF_UNAVAILABLE) return UINT16_MAX; if(us/100>65534) { *flags|=GAME_PERF_OVERFLOW; return 65534; } return (uint16_t)(us/100); }
bool game_perf_encode_columns(const game_perf_frame *frames,size_t n,uint8_t *out,size_t capacity) {
    if(n>SIZE_MAX/24 || capacity<n*24 || (!frames && n) || (!out && n)) return false;
    for(size_t i=0;i<n;i++) { const game_perf_frame *f=&frames[i]; uint16_t flags=f->flags;
        if(f->gpu_us==GAME_PERF_UNAVAILABLE) flags|=GAME_PERF_GPU_UNAVAILABLE;
        if(f->budget_us!=GAME_PERF_UNAVAILABLE && f->interval_us!=GAME_PERF_UNAVAILABLE && f->interval_us>f->budget_us) flags|=GAME_PERF_LATE;
        uint64_t interval=f->interval_us,elapsed=f->elapsed_us/1000;
        if(interval>UINT32_MAX || elapsed>UINT32_MAX) flags|=GAME_PERF_OVERFLOW;
        put32(out+i*4,interval>UINT32_MAX?UINT32_MAX:(uint32_t)interval);
        put32(out+n*4+i*4,elapsed>UINT32_MAX?UINT32_MAX:(uint32_t)elapsed);
        uint16_t values[8]={duration(f->cpu_us,&flags),duration(f->simulation_us,&flags),duration(f->network_us,&flags),duration(f->render_us,&flags),duration(f->gpu_us,&flags),duration(f->budget_us,&flags),0,0};
        if(f->tick_delta>65534) flags|=GAME_PERF_OVERFLOW;
        values[6]=f->tick_delta>65534?65534:(uint16_t)f->tick_delta; values[7]=flags;
        for(size_t j=0;j<8;j++) { size_t offset=n*8+j*n*2+i*2; out[offset]=(uint8_t)values[j]; out[offset+1]=(uint8_t)(values[j]>>8); }
    } return true;
}
uint32_t game_perf_crc32(const void *data,size_t size) { const uint8_t *p=data; uint32_t crc=~0u; for(size_t i=0;i<size;i++) { crc^=p[i]; for(unsigned bit=0;bit<8;bit++) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u))); } return ~crc; }
size_t game_perf_journal_chunk(void *dst,size_t capacity,uint32_t seq,const void *data,size_t size) { if(!dst || (!data && size) || size>UINT32_MAX || size>SIZE_MAX-16 || capacity<size+16) return 0; uint8_t *p=dst; memcpy(p,"GPJ1",4); put32(p+4,(uint32_t)size); put32(p+8,seq); put32(p+12,game_perf_crc32(data,size)); if(size) memcpy(p+16,data,size); return size+16; }
size_t game_perf_journal_recover(const void *data,size_t size,uint32_t *sequence) { if(!data) return 0; const uint8_t *p=data; size_t offset=0; uint32_t previous=0; bool first=true; while(size-offset>=16) { const uint8_t *h=p+offset; uint32_t n=get32(h+4),seq=get32(h+8); if(memcmp(h,"GPJ1",4) || n>size-offset-16 || (!first && seq!=previous+1) || game_perf_crc32(h+16,n)!=get32(h+12)) break; offset+=16+n; previous=seq; first=false; if(sequence) *sequence=seq; } return offset; }
