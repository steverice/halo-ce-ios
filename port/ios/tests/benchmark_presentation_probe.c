/* Synthetic presentation evidence: no drawable, display or callback clock. */
#include "benchmark_presentation.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "presentation line %d: %s\n", __LINE__, #c); } } while (0)
#define NEAR(a,b) (fabs((a)-(b)) < 1e-7)
#define ACCEPT(p,t,v) CHECK(benchmark_presentation_complete(p,t,v) == BENCHMARK_PRESENTATION_ACCEPTED)
static struct benchmark_presentation_token reserve(struct benchmark_presentation *p) {
    return benchmark_presentation_reserve(p, 0.5 + p->count / 355.0);
}

static void reordered(void) {
    struct benchmark_presentation p = {0};
    CHECK(benchmark_presentation_reset(&p, 1, 8));
    CHECK(benchmark_presentation_open(&p, 1));
    struct benchmark_presentation_token a = reserve(&p), b = reserve(&p), c = reserve(&p);
    /* Arrival times could be arbitrarily delayed: only captured timestamps enter. */
    ACCEPT(&p,c,1.04); ACCEPT(&p,a,1.0); ACCEPT(&p,b,1.02);
    CHECK(benchmark_presentation_complete(&p,b,99) == BENCHMARK_PRESENTATION_DUPLICATE);
    const struct benchmark_presentation_summary *s = benchmark_presentation_seal(&p,0);
    CHECK(s->received == 3 && s->positive == 3 && s->intervals == 2 && s->duplicates == 1);
    CHECK(NEAR(s->interval.mean,20) && NEAR(s->interval.high,20));
    CHECK(p.records[0].span == BENCHMARK_PRESENTATION_NO_SPAN);
    CHECK(p.records[1].presented == 1.02 && p.records[2].previous_index == 1);
    struct benchmark_presentation_summary frozen = *s;
    CHECK(benchmark_presentation_complete(&p,a,2) == BENCHMARK_PRESENTATION_LATE);
    CHECK(memcmp(s,&frozen,sizeof frozen) == 0);
    CHECK(benchmark_presentation_seal(&p,1) == s && !s->timed_out);
    benchmark_presentation_free(&p);
}
static void distribution(void) {
    struct benchmark_presentation p = {0};
    CHECK(benchmark_presentation_reset(&p, 1, 5)); CHECK(benchmark_presentation_open(&p, 1));
    /* Source intervals are 10, 70, 20, 40 ms; only their distribution is sorted. */
    double times[] = {1, 1.01, 1.08, 1.1, 1.14};
    for (size_t i = 0; i < 5; i++) { struct benchmark_presentation_token t = reserve(&p); ACCEPT(&p, t, times[i]); }
    const struct benchmark_presentation_summary *s = benchmark_presentation_seal(&p, 0);
    CHECK(s->intervals == 4 && s->stalls == 1 && s->unknown_gaps == 0);
    CHECK(NEAR(s->interval.mean, 35) && NEAR(s->interval.median, 20));
    CHECK(NEAR(s->interval.high, 70) && NEAR(s->interval.higher, 70) && NEAR(s->interval.worst, 70));
    CHECK(NEAR(p.records[2].milliseconds, 70) && NEAR(p.records[4].milliseconds, 40));
    benchmark_presentation_free(&p);
}
static void gaps_and_order(void) {
    struct benchmark_presentation p = {0};
    CHECK(benchmark_presentation_reset(&p,2,16)); CHECK(benchmark_presentation_open(&p,1));
    double times[] = {1,0,NAN,-1,INFINITY,1.1,1.1,1.05,1.2,1.26};
    for (size_t i=0;i<10;i++) { struct benchmark_presentation_token t=reserve(&p); ACCEPT(&p,t,times[i]); }
    reserve(&p); /* Missing callback differs from zero and invalid callbacks. */
    struct benchmark_presentation_token last=reserve(&p); ACCEPT(&p,last,1.4);
    const struct benchmark_presentation_summary *s=benchmark_presentation_seal(&p,1);
    CHECK(s->received==11 && s->positive==7 && s->unpresented==1 && s->invalid==3 && s->missing==1);
    CHECK(s->order_errors==2 && p.records[6].order_error && p.records[7].order_error);
    CHECK(s->unknown_gaps==3 && s->unknown_stalls==3 && s->intervals==1 && s->stalls==1);
    CHECK(p.records[5].previous_index==0 && NEAR(p.records[5].milliseconds,100));
    CHECK(p.records[8].previous_index==5 && NEAR(p.records[8].milliseconds,100));
    CHECK(p.records[11].previous_index==9 && NEAR(p.records[11].milliseconds,140));
    CHECK(NEAR(s->interval.mean,60) && s->timed_out);
    benchmark_presentation_free(&p);
}
static void segments_and_reuse(void) {
    struct benchmark_presentation p={0};
    CHECK(benchmark_presentation_reset(&p,3,8)); CHECK(benchmark_presentation_open(&p,1));
    struct benchmark_presentation_token a=reserve(&p), b=reserve(&p);
    benchmark_presentation_close_segment(&p);
    CHECK(reserve(&p).generation==0);
    CHECK(!benchmark_presentation_open(&p,1)); CHECK(benchmark_presentation_open(&p,2));
    struct benchmark_presentation_token c=reserve(&p), d=reserve(&p);
    benchmark_presentation_close(&p);
    CHECK(reserve(&p).generation==0 && !benchmark_presentation_open(&p,3));
    ACCEPT(&p,c,9); ACCEPT(&p,a,1); ACCEPT(&p,b,1.02);
    CHECK(benchmark_presentation_pending(&p)==1);
    const struct benchmark_presentation_summary *s=benchmark_presentation_seal(&p,1);
    CHECK(s->intervals==1 && s->missing==1 && s->unknown_gaps==0 && s->timed_out);
    CHECK(p.records[2].span==BENCHMARK_PRESENTATION_NO_SPAN);
    CHECK(benchmark_presentation_complete(&p,d,9.02)==BENCHMARK_PRESENTATION_LATE);
    CHECK(!benchmark_presentation_reset(&p,3,8));
    CHECK(benchmark_presentation_reset(&p,4,8)); CHECK(benchmark_presentation_open(&p,1));
    struct benchmark_presentation_token n=reserve(&p);
    CHECK(benchmark_presentation_complete(&p,a,100)==BENCHMARK_PRESENTATION_STALE);
    ACCEPT(&p,n,4);
    s=benchmark_presentation_seal(&p,0);
    CHECK(s->received==1 && s->intervals==0 && s->missing==0);
    benchmark_presentation_free(&p);
}
static void bounded(void) {
    struct benchmark_presentation p={0};
    CHECK(!benchmark_presentation_reset(&p,0,4));
    CHECK(!benchmark_presentation_reset(&p,1,BENCHMARK_PRESENTATION_CAPACITY_MAX+1));
    CHECK(!benchmark_presentation_reset(&p,1,0));
    CHECK(benchmark_presentation_reset(&p,1,2)); CHECK(benchmark_presentation_open(&p,1));
    struct benchmark_presentation_token a=reserve(&p); reserve(&p);
    CHECK(reserve(&p).generation==0); CHECK(reserve(&p).generation==0);
    struct benchmark_presentation_token bad={1,99};
    CHECK(benchmark_presentation_complete(&p,bad,1)==BENCHMARK_PRESENTATION_BAD_TOKEN);
    ACCEPT(&p,a,1);
    const struct benchmark_presentation_summary *s=benchmark_presentation_seal(&p,1);
    CHECK(s->expected==4 && s->stored==2 && s->overflow==2 && s->missing==1 && s->bad_tokens==1);
    CHECK(NEAR(s->callback_coverage,0.25) && NEAR(s->positive_coverage,0.25));
    benchmark_presentation_free(&p);
    CHECK(benchmark_presentation_reset(&p,2,1));
    s=benchmark_presentation_seal(&p,0);
    CHECK(s->expected==0 && s->intervals==0 && s->callback_coverage==0 && s->interval.mean==0);
    benchmark_presentation_free(&p);
}
static void submission_is_not_presentation(void) {
    struct benchmark_presentation p={0};
    struct benchmark_presentation_token tokens[355];
    CHECK(benchmark_presentation_reset(&p,9,355)); CHECK(benchmark_presentation_open(&p,1));
    for(int i=0;i<355;i++) tokens[i]=reserve(&p);
    /* 60 positives spaced among 355 submitted frames; reverse callback arrival. */
    for(int i=354;i>=0;i--) ACCEPT(&p,tokens[i], i%6==0 ? 1.0+(i/6)/60.0 : 0);
    const struct benchmark_presentation_summary *s=benchmark_presentation_seal(&p,0);
    CHECK(s->expected==355 && s->received==355 && s->positive==60 && s->unpresented==295);
    CHECK(s->missing==0 && s->intervals==0 && s->unknown_gaps==59 && s->unknown_stalls==0);
    CHECK(NEAR(s->callback_coverage,1) && NEAR(s->positive_coverage,60.0/355));
    CHECK(NEAR(p.records[354].presented-p.records[0].presented,59.0/60));
    benchmark_presentation_free(&p);
}
int main(void) {
    reordered(); distribution(); gaps_and_order(); segments_and_reuse(); bounded(); submission_is_not_presentation();
    printf("benchmark presentation: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
