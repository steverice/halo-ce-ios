"""Exercise master listing admission and snapshot merging with the lobby parser."""
from pathlib import Path
import subprocess
from test_network_discovery import function

ROOT = Path(__file__).resolve().parents[1]


def test_master_lobby_admission_and_snapshot(tmp_path):
    source = (ROOT / 'port/linux/src/p2p_lobby.c').read_text()
    assert 'int p2p_lobby_master_read(' in source, 'master listings need a verified lobby admission path'
    declarations = '\n'.join(function(source, 'struct ' + name + '\n') + ';'
                             for name in ('listing', 'game', 'tombstone', 'queued'))
    functions = '\n'.join(function(source, signature) for signature in (
        'static unsigned long get_long(', 'static int get_text(', 'static int listing_read(',
        'static void listing_show(', 'static void master_text(', 'int p2p_lobby_master_read(',
        'void p2p_lobby_master_games(', 'static int listing_order(', 'int p2p_lobby_games('))
    fixture = (ROOT / 'port/ios/tests/network_internet_kind_probe.c').read_text()
    fixture = fixture[:fixture.index('int main(void)')]
    fixture = fixture.replace('/* DECLARATIONS */', declarations)
    start = fixture.index('static struct {')
    end = fixture.index('static struct tombstone *', start)
    fixture = fixture[:start] + r'''
#define MAXIMUM_MASTER_GAMES 512
#define MASTER_EXPIRY 300000
#define DELTA_EXPIRY 60000
#define P2P_IDENTIFIER_SIZE 6
static unsigned long clock_now = 1;
static struct { int browsing, master_count, delta_count; unsigned long master_time, delta_time;
    struct p2p_listing master_games[512], delta_games[2]; struct game games[MAXIMUM_GAMES]; } lobby;
static int p2p_lock;
static void pthread_mutex_lock(int *lock) { (void)lock; }
static void pthread_mutex_unlock(int *lock) { (void)lock; }
static int elapsed(unsigned long since, unsigned long interval) { return clock_now - since >= interval; }
static int identifier_failed(const unsigned char *id) { (void)id; return 0; }
static int signing_key_hash(const unsigned char *key, unsigned char *hash) { memcpy(hash,key,16); return 1; }
static int listing_signed(const unsigned char *bytes, const struct listing *parsed) { return bytes[parsed->signed_size] == 0x5a; }
''' + fixture[end:]
    fixture = fixture.replace('static unsigned long p2p_now(void) { return 1; }',
                              'static unsigned long p2p_now(void) { return clock_now; }')
    fixture = '#include <stdlib.h>\n' + fixture.replace('/* PRODUCTION */', functions)
    fixture += r'''
int main(void)
{
    unsigned char bytes[MAXIMUM_LISTING_SIZE + 1];
    struct p2p_listing row, rows[514], many[513];
    int size = record(bytes, 0, 7, 0);
    bytes[14] = 0x73;
    bytes[66] = 'A'; bytes[67] = '\n'; bytes[68] = 'B'; bytes[69] = 0x7f;
    bytes[70] = 'C'; bytes[71] = 0; bytes[72] = 'D';
    bytes[size - 64] = 0x5a;
    assert(p2p_lobby_master_read(bytes,size,&row));
    assert(row.from_master && !strcmp(row.name,"ABCD") && row.key_hash[0] == 0x73);
    assert(!strncmp(row.invite,"halo://join/",12));
    bytes[size - 64] ^= 1;
    assert(!p2p_lobby_master_read(bytes,size,&row));
    bytes[size - 64] ^= 1;
    bytes[5] |= _listing_closed;
    assert(!p2p_lobby_master_read(bytes,size,&row));
    bytes[5] &= ~_listing_closed;
    bytes[4] = 255;
    assert(!p2p_lobby_master_read(bytes,size,&row));
    bytes[4] = HALO_PORT_NETWORK_VERSION;
    assert(p2p_lobby_master_read(bytes,size,&row));
    lobby.browsing = 1;
    lobby.games[0].used = 1; lobby.games[0].listing = row; memcpy(lobby.games[0].key_hash,row.key_hash,16);
    lobby.games[0].listing.from_master = 0; strcpy(lobby.games[0].listing.name,"Broker");
    lobby.delta_count = 1; lobby.delta_time = clock_now; lobby.delta_games[0] = row;
    many[0]=row; many[1]=row;
    p2p_lobby_master_games(many,2);
    assert(p2p_lobby_games(rows,514) == 1 && rows[0].from_master && !strcmp(rows[0].name,"Broker"));
    clock_now += 299999;
    assert(p2p_lobby_games(rows,514) == 1 && rows[0].from_master);
    p2p_lobby_master_games(&row,1); /* a 304 revalidation refreshes the lifetime */
    clock_now += 2;
    assert(p2p_lobby_games(rows,514) == 1 && rows[0].from_master);
    clock_now += 300000;
    assert(p2p_lobby_games(rows,514) == 1 && !rows[0].from_master);
    memset(lobby.games,0,sizeof(lobby.games));
    for (int i=0;i<513;i++) { many[i]=row; many[i].key_hash[0]=i; many[i].key_hash[1]=i>>8; }
    p2p_lobby_master_games(many,513);
    assert(p2p_lobby_games(rows,514)==512);
    lobby.browsing=0;
    p2p_lobby_master_games(&row,1);
    assert(p2p_lobby_games(rows,514)==0);
    puts("PASS: master lobby admission, deduplication, expiry, cap");
    return 0;
}
'''
    probe, binary = tmp_path / 'master_lobby.c', tmp_path / 'master_lobby'
    probe.write_text(fixture)
    subprocess.run(['clang', '-std=c11', '-Wall', '-Werror', '-Wno-unused-function',
                    '-iquote', str(ROOT / 'port/linux/src'), '-iquote', str(ROOT / 'port/linux/include'),
                    str(probe), '--output=' + str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def test_browse_only_logs_rows_without_joining(tmp_path):
    source = (ROOT / 'port/linux/game/network_test.c').read_text()
    assert '"browse-only"' in source, 'runner browsing must support observation without joining'
    helper = function(source, 'static void network_test_browse_observe(')
    update = function(source, 'void network_test_update(')
    branch = update[update.index('\tcase _network_test_browse:'):update.index('\tcase _network_test_host:')]
    program = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "p2p.h"
#define TRUE 1
#define FALSE 0
#define NUMBEROF(a) (sizeof(a)/sizeof((a)[0]))
#define _network_test_browse 3
#define _network_test_join 2
static struct { int browsing, browse_only, mode; } network_test;
static int logs, joins, opens;
static struct p2p_listing row;
static void platform_log(const char *format, ...) { (void)format; logs++; }
static int p2p_join_invite(const char *invite) { (void)invite; joins++; return 1; }
static void p2p_lobby_browse(int on) { opens+=on; }
static int p2p_lobby_games(struct p2p_listing *games,int max) { assert(max>0); games[0]=row; return 1; }
/* HELPER */
static void update(void) { switch(network_test.mode) { /* BRANCH */ } }
int main(void) {
    network_test.mode=_network_test_browse; network_test.browse_only=1;
    strcpy(row.invite,"halo://join/some-public-game"); strcpy(row.name,"Busy server"); row.player_count=16;
    row.from_master=1; strcpy(row.region,"US");
    update(); assert(joins==0 && opens==1 && logs>=2);
    int first=logs; update(); assert(joins==0 && logs==first);
    strcpy(row.region,"EU"); update(); assert(joins==0 && logs>first);
    return 0;
}
'''
    # The implementation uses the public API; these test stand-ins provide it.
    program = program.replace('static int p2p_join_invite(', 'int p2p_join_invite(').replace(
        'static void p2p_lobby_browse(', 'void p2p_lobby_browse(').replace(
        'static int p2p_lobby_games(', 'int p2p_lobby_games(')
    probe, binary = tmp_path / 'browse_only.c', tmp_path / 'browse_only'
    probe.write_text(program.replace('/* HELPER */', helper).replace('/* BRANCH */', branch))
    subprocess.run(['clang', '-std=c11', '-Wall', '-Werror', '-iquote', str(ROOT / 'port/linux/src'),
                    str(probe), '--output=' + str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_master_admission_verifies_captured_host_signature(tmp_path):
    import base64
    import json

    source = (ROOT / 'port/linux/src/p2p_lobby.c').read_text()
    capture = json.loads((ROOT / 'port/ios/tests/fixtures/master-list/official-populated.json').read_text())
    listing = base64.b64decode(capture['servers'][0]['listing'])
    production = function(source, 'struct listing\n') + ';\n'
    production += '\n'.join(function(source, signature) for signature in (
        'static void sanitize(', 'static int signing_key_hash(', 'static unsigned long get_long(',
        'static int get_text(', 'static int listing_read(', 'static int listing_signed(',
        'static void listing_show(', 'static void master_text(', 'int p2p_lobby_master_read('))
    harness = (ROOT / 'tools/p2p_lobby_check.c').read_text()
    production += '\n' + function(harness, 'void p2p_hex(')
    program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "p2p_internal.h"
#include "halo_network_version.h"
#include "halo_network_content.h"
#define LISTING_FORMAT 1
#define STAMP_SIZE 8
#define MINIMUM_LISTING_SIZE 140
#define MAXIMUM_LISTING_SIZE 268
#define _listing_open 1
#define _listing_in_progress 2
#define _listing_has_teams 4
#define _listing_closed 8
#define _listing_password 16
static const char signature_label[]="hceu-lobby-1";
void posix_random_bytes(void *buffer, unsigned long size) { memset(buffer,0,size); }
void p2p_key_hash(const unsigned char *key,unsigned char *hash) { unsigned char digest[32]; p2p_sha256(key,32,digest); memcpy(hash,digest,16); }
void p2p_identifier_from_hash(const unsigned char *hash,unsigned char *identifier) { memcpy(identifier,hash,6); }
/* PRODUCTION */
int main(void) {
    unsigned char bytes[]={ /* LISTING */ };
    struct p2p_listing game;
    assert(p2p_lobby_master_read(bytes,sizeof(bytes),&game));
    assert(game.from_master && game.dedicated && game.network_version==39 && game.invite[0]);
    for (unsigned i=0;i<sizeof(bytes);i++) {
        bytes[i]^=1;
        assert(!p2p_lobby_master_read(bytes,sizeof(bytes),&game));
        bytes[i]^=1;
    }
    return 0;
}
'''
    program = program.replace('/* PRODUCTION */', production).replace(
        '/* LISTING */', ','.join(str(value) for value in listing))
    probe, binary = tmp_path / 'master_signed.c', tmp_path / 'master_signed'
    probe.write_text(program)
    subprocess.run(['clang', '-std=c11', '-Wall', '-Werror', '-Wno-unused-function',
                    '-iquote', str(ROOT / 'port/linux/src'), '-iquote', str(ROOT / 'port/linux/include'),
                    '-I' + str(ROOT / 'port/third_party/monocypher'), str(probe),
                    str(ROOT / 'port/linux/src/p2p_crypto.c'),
                    str(ROOT / 'port/third_party/monocypher/monocypher.c'),
                    str(ROOT / 'port/third_party/monocypher/monocypher-ed25519.c'),
                    '--output=' + str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
