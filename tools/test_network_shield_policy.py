"""Exercise message 91's real validation and reliable STOCK sends without game data."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    start = source.index(marker)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def test_shield_policy_requires_host_stream_and_sends_stock_on_readiness(tmp_path):
    source = (ROOT / "port/linux/game/network_distributed.c").read_text()
    header = (ROOT / "port/linux/game/network_distributed.h").read_text()
    kinds = block(header, "enum\n{") + ";\n"
    kinds += block(header, "enum\n{\n\t_distributed_to_clients,") + ";\n"
    # A native probe keeps the guest's 32-bit wire stamp, even on LP64 hosts.
    structures = block(header, "struct distributed_message_header\n{").replace(
        "long game_time;", "int32_t game_time;") + ";\n"
    structures += block(header, "struct distributed_shield_policy_message\n{").replace(
        "unsigned long selector;", "uint32_t selector;") + ";\n"
    send = block(source, "static void distributed_send_shield_policy(")
    fill = block(source, "static void distributed_fill_header(")
    reliable_send = block(source, "void distributed_send_to_machine_reliably(")
    build = block((ROOT / "source/bungie_net/common/message_header.c").read_text(),
                  "void build_message_header(")
    validate = block(source, "static boolean distributed_shield_policy_valid(")
    ready = block(source, "case _distributed_message_client_ready:\n\t{")
    initial = block(source, "if (!distributed_shield_policy_sent)")
    program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "VERSION_HEADER"
typedef unsigned char byte;
typedef uint16_t word;
typedef uint16_t message_header;
typedef int boolean;
#define NONE (-1)
#define TRUE 1
#define FALSE 0
#define csmemcpy memcpy
#define csmemset memset
#define GET_MESSAGE_TYPE(value) (((value) >> 2) & 3)
#define match_assert(file, line, condition) assert(condition)
enum { MAXIMUM_MESSAGE_SIZE = 0xfff, NUMBER_OF_MESSAGE_TYPES = 4, MESSAGE_FLAG_BITS_MASK = 3 };
enum { _game_connection_network_client, _game_connection_network_server, _game_connection_local };
''' + kinds + r'''
static int connection = _game_connection_network_client, in_game = 1, menu, unreliable;
static int distributed_handling_batch;
static unsigned host_version = 38;
static int game_connection(void) { return connection; }
static int game_in_progress(void) { return in_game; }
static int main_menu_is_active(void) { return menu; }
static int network_connection_last_read_was_unreliable(void) { return unreliable; }
static unsigned network_game_host_version(void) { return host_version; }
static long game_time_get(void) { return 209; }
static struct { unsigned sent; } distributed_statistics;
''' + structures + r'''
static byte sent[12];
static int sends, resyncs, objects, aim, statistics, game_state, asked_resync;
static boolean distributed_shield_policy_sent;
static long sent_machine;
static void network_distributed_server_send_to_machine_reliably(long machine, void *message, word size)
{
    assert(size == 12);
    memcpy(sent, message, size);
    sent_machine = machine;
    sends++;
}
''' + build + fill + reliable_send + r'''
static void distributed_send(void *message, byte type, short count, word size, short destination)
{
    assert(destination == _distributed_to_clients_reliably);
    distributed_send_to_machine_reliably(NONE, message, type, count, size);
}
''' + send + validate + r'''
static int distributed_machine_loaded(long machine) { assert(machine == 3); return 1; }
static void network_aim_assist_send_policy(long machine) { assert(machine == 3); aim++; }
static void distributed_count_resync(void) { resyncs++; }
static void network_objects_client_asked(long machine, int resync) { assert(machine == 3); asked_resync = resync; objects++; }
static void distributed_send_all_statistics(long machine) { assert(machine == 3); statistics++; }
static void distributed_send_game_state(long machine) { assert(machine == 3); game_state++; }
static void client_ready(int count)
{
    long machine_index = 3;
    struct distributed_message_header header = {0};
    header.count = (byte)count;
    switch (9) {
''' + ready + r'''
    }
}
static void initial_policy(void) {
''' + initial + r'''
}
int main(void)
{
    /* FORMATS.md: the whole STOCK frame. Only the transport word changes
       from network to host byte order before distributed dispatch. */
    byte frame[16] = {0xc8, 0x00, 0x5b, 0x01, 0x00, 0x00, 0x00, 0x00,
                      0x00, 0x00, 0x00, 0x00};
    word message[8];
    for (int selector = 0; selector <= 6; selector++) {
        frame[8] = (byte)selector;
        /* Initial, changed, late-join and reconnect stamps are all allowed. */
        for (int stamp = 0; stamp < 4; stamp++) {
            frame[4] = (byte[]){0, 1, 50, 209}[stamp];
            memcpy(message, frame, sizeof(frame));
            assert(distributed_shield_policy_valid(NONE, message, 12));
        }
    }
    for (int selector = 7; selector < 256; selector++) {
        frame[8] = (byte)selector;
        memcpy(message, frame, sizeof(frame));
        assert(!distributed_shield_policy_valid(NONE, message, 12));
    }
    frame[8] = 0;
    for (int i = 9; i < 12; i++) {
        frame[i] = 1;
        memcpy(message, frame, sizeof(frame));
        assert(!distributed_shield_policy_valid(NONE, message, 12));
        frame[i] = 0;
    }
    memcpy(message, frame, sizeof(frame));
    for (int size = 0; size <= 16; size++)
        assert(distributed_shield_policy_valid(NONE, message, (word)size) == (size == 12));
    assert(!distributed_shield_policy_valid(3, message, 12));
    unreliable = 1;
    assert(!distributed_shield_policy_valid(NONE, message, 12));
    unreliable = 0;
    distributed_handling_batch = 1;
    assert(!distributed_shield_policy_valid(NONE, message, 12));
    distributed_handling_batch = 0;
    connection = _game_connection_network_server;
    assert(!distributed_shield_policy_valid(NONE, message, 12));
    connection = _game_connection_network_client;
    in_game = 0;
    assert(!distributed_shield_policy_valid(NONE, message, 12));
    in_game = 1;
    menu = 1;
    assert(!distributed_shield_policy_valid(NONE, message, 12));
    menu = 0;
    /* 29 to 35 send no policy; 36 and 37 send selectors 0 to 5; 38 adds 6 */
    for (host_version = 29; host_version <= 38; host_version++) {
        for (int selector = 0; selector <= 7; selector++) {
            int expected = host_version >= 36 && (selector <= 5 || (selector == 6 && host_version == 38));
            frame[8] = (byte)selector;
            memcpy(message, frame, sizeof(frame));
            assert(distributed_shield_policy_valid(NONE, message, 12) == expected);
        }
    }
    frame[8] = 0;
    memcpy(message, frame, sizeof(frame));
    host_version = 38;
    for (int count = 0; count < 3; count++) {
        ((struct distributed_message_header *)message)->count = (byte)count;
        assert(distributed_shield_policy_valid(NONE, message, 12) == (count == 1));
    }
    ((struct distributed_message_header *)message)->count = 1;
    ((struct distributed_message_header *)message)->header = 0xcc;
    assert(!distributed_shield_policy_valid(NONE, message, 12));
    ((struct distributed_message_header *)message)->header = 0xc8;
    ((struct distributed_message_header *)message)->type = 90;
    assert(!distributed_shield_policy_valid(NONE, message, 12));
    /* A client cannot send policy. Every host ready/resync/rejoin sends
       STOCK, irrespective of the last accepted remote selector. */
    distributed_send_shield_policy(3);
    assert(sends == 0);
    connection = _game_connection_network_server;
    for (int count = 0; count < 2; count++) {
        client_ready(count);
        assert(sent_machine == 3 && sends == count + 1);
        assert(asked_resync == count);
        assert(!memcmp(sent, (byte[]){0xc8, 0x00, 0x5b, 0x01, 0xd1, 0, 0, 0, 0, 0, 0, 0}, 12));
    }
    client_ready(0); /* a new connection at the same machine index */
    assert(sends == 3 && aim == 3 && objects == 3 && resyncs == 1 && statistics == 3 && game_state == 3);
    assert(asked_resync == 0);
    initial_policy();
    assert(sends == 4 && sent_machine == NONE);
    initial_policy();
    assert(sends == 4); /* no repeated broadcasts of an unchanged policy */
    puts("PASS: shield policy host stream, selectors, stamps and reliable STOCK readiness/rejoin sends");
}
'''
    program = program.replace("VERSION_HEADER", str(ROOT / "port/linux/include/halo_network_version.h"))
    (tmp_path / "probe.c").write_text(program)
    executable = tmp_path / "probe"
    subprocess.run(["clang", "-std=c11", "-Wall", "-Werror", "-fsanitize=address,undefined",
                    str(tmp_path / "probe.c"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
