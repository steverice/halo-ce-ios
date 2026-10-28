"""Exercise master fetch scheduling and cache handoff without network traffic."""
from pathlib import Path
import subprocess
import shutil

ROOT = Path(__file__).resolve().parents[1]


def test_master_worker(tmp_path):
    shim = tmp_path / "platform.h"
    shim.write_text("void platform_log(const char *, ...);\nvoid Sleep(unsigned long);\n")
    source = tmp_path / "p2p_master.c"
    source.write_text((ROOT / "port/linux/src/p2p_master.c").read_text())
    probe = tmp_path / "worker.c"
    probe.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "p2p_master.c"
pthread_mutex_t p2p_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long clock_now = 1000;
static int online = 1, enabled = 1, requests, handoffs, clears, status = 200, parsed = 1;
static char sent_etag[128];
static int close_during_request;
unsigned long p2p_now(void) { return clock_now; }
void Sleep(unsigned long time) { (void)time; }
void platform_log(const char *format, ...) { (void)format; }
int config_boolean(const char *name) {
    if (!strcmp(name, "debug.master_list")) return enabled;
    if (!strcmp(name, "network.online")) return online;
    return 1;
}
int posix_https_get_signed(const char *url, const char *etag, char *body, int size,
    int timeout, struct posix_https_headers *headers, char *error, int error_size) {
    assert(!strcmp(url, "https://master.opencommunityedition.com/v1/servers"));
    requests++;
    if (close_during_request) {
        close_during_request = 0; p2p_master_browse(0); p2p_master_browse(1);
    }
    snprintf(sent_etag, sizeof(sent_etag), "%s", etag ? etag : "");
    memset(headers, 0, sizeof(*headers));
    strcpy(headers->etag, "\"capture\"");
    strcpy(headers->signature, "test signature");
    strcpy(headers->signature_key, "test key");
    strcpy(headers->retry_after, "20");
    strcpy(body, "{}"); headers->body_size = 2;
    return status;
}
int master_parse_list(const char *body, int size, const char *sig, const char *key, long long now,
    struct master_game *games, int maximum, char *error, int error_size) {
    memset(games, 0, sizeof(*games));
    games[0].listing_size = 1; strcpy(games[0].region, "eu");
    if (parsed < 0) snprintf(error, error_size, "clock mismatch");
    return parsed;
}
int p2p_lobby_master_read(const unsigned char *bytes, int size, struct p2p_listing *shown) {
    memset(shown, 0, sizeof(*shown)); strcpy(shown->name, "Official"); return 1;
}
void p2p_lobby_master_games(const struct p2p_listing *games, int count) {
    if (!count) { clears++; return; }
    handoffs++; assert(count == 1); assert(!strcmp(games[0].region, "eu"));
}
int main(void) {
    assert(master_retry_delay("172800") == 172800000ULL);
    assert(master_retry_delay("M, 27 Oct 2026 12:00:00 GMT") == 60000);
    assert(master_retry_delay("99999999999999999999999999999999") == ULLONG_MAX);
    {
        time_t future = time(NULL) + 172800;
        char date[128];
        strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", gmtime(&future));
        unsigned long long wait = master_retry_delay(date);
        assert(wait >= 172799000ULL && wait <= 172800000ULL);
    }
    /* Direct passes avoid timing-sensitive thread sleeps. */
    p2p_master_browse(1); master_pass();
    assert(requests == 1 && handoffs == 1 && !sent_etag[0]);
    master_pass(); assert(requests == 1); /* no polling inside the minute */
    p2p_master_refresh(); master_pass(); assert(requests == 1);
    clock_now += 4999; master_pass(); assert(requests == 1);
    clock_now++; status = 304; master_pass();
    assert(requests == 2 && handoffs == 2 && !strcmp(sent_etag, "\"capture\""));
    p2p_master_browse(0); p2p_master_browse(1); master_pass(); assert(requests == 2);
    clock_now += 5000; parsed = -1; master_pass();
    assert(requests == 3 && handoffs == 2 && clears == 1); /* stale 304 is rejected */
    clock_now += 5000; parsed = 1; status = 429; p2p_master_refresh(); master_pass();
    assert(requests == 4 && !sent_etag[0]);
    clock_now += 5000; p2p_master_refresh(); master_pass(); assert(requests == 4);
    p2p_master_browse(0); p2p_master_browse(1); master_pass(); assert(requests == 4);
    clock_now += 14999; master_pass(); assert(requests == 4);
    clock_now++; status = 200; master_pass(); assert(requests == 5 && handoffs == 3);
    clock_now += 5000; online = 0; p2p_master_refresh(); master_pass(); assert(requests == 5);
    online = 1; enabled = 0; master_pass(); assert(requests == 5);
    enabled = 1; master_pass(); assert(requests == 6);
    /* Closing and reopening during a request discards its handoff. */
    clock_now += 5000; close_during_request = 1; p2p_master_refresh();
    int before = handoffs;
    master_pass(); assert(requests == 7 && handoffs == before);
    master_pass(); assert(requests == 7);
    clock_now += 5000; master_pass(); assert(requests == 8 && handoffs == before + 1);
    p2p_master_browse(0); clock_now += 5000; p2p_master_refresh(); master_pass(); assert(requests == 8);
    /* An open browser asks again once a minute, and never while closed. */
    clock_now += 60000; master_pass(); assert(requests == 8);
    p2p_master_browse(1); master_pass(); assert(requests == 9);
    clock_now += 59999; master_pass(); assert(requests == 9);
    clock_now++; master_pass(); assert(requests == 10);
    puts("PASS: master open, refresh, throttle, cached clock, Retry-After, configuration");
    return 0;
}
''')
    binary = tmp_path / "worker"
    built = subprocess.run([shutil.which("clang") or "clang", "-std=gnu11", "-Wall", "-Werror",
                            "-Wno-unused-parameter", "-Iport/linux/src", "-iquote", "port/linux/include",
                            str(probe), "-pthread", "--output=" + str(binary)], cwd=ROOT,
                           capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    ran = subprocess.run([str(binary)], capture_output=True, text=True)
    assert ran.returncode == 0, ran.stdout + ran.stderr
