"""An ignored client-build frame must leave the reliable stream usable."""
from pathlib import Path
import hashlib
import re
import subprocess

from test_network_shield_policy import block

ROOT = Path(__file__).resolve().parents[1]


def test_client_build_is_ignored_without_losing_next_frame(tmp_path):
    network = (ROOT / "port/linux/game/network_distributed.c").read_text()
    header = (ROOT / "port/linux/game/network_distributed.h").read_text()
    dispatch = block(network, "void network_distributed_handle_message(")
    # Execute the real validation path; reaching game handlers is a test failure.
    dispatch = dispatch[:dispatch.index("\t/* (the votes to kick")] + "\nassert(!\"unknown kind reached game dispatch\");\n}\n"
    structures = block(header, "struct distributed_message_header\n{").replace(
        "long game_time;", "int32_t game_time;") + ";\n"
    object_sizes = block((ROOT / "port/linux/game/network_objects.c").read_text(),
                         "word network_objects_entry_size(")
    names = set(re.findall(r"sizeof\(struct (\w+)\)", dispatch + object_sizes)) - {"distributed_message_header"}
    structures += "\n".join(f"struct {name} {{ byte unused; }};" for name in sorted(names))
    functions = set(re.findall(r"\b(network_\w+entry_size|network_damage_entry_size)\(", dispatch))
    stubs = "\n".join(f"static word {name}({'byte type' if name in {'network_damage_entry_size', 'network_objects_entry_size'} else 'void'}) {{ return 0; }}" for name in sorted(functions - {"network_objects_entry_size"}))
    connection_source = (ROOT / "source/networking/network_connection.c").read_text()
    reader_marker = "static boolean network_client_reliable_connection_read("
    reader = block(connection_source[connection_source.rindex(reader_marker):], reader_marker)
    swap = block((ROOT / "source/bungie_net/common/message_header.c").read_text(),
                 "void byte_swap_message_header(")
    # Capture SHA-256 is recorded in FORMATS-39.md. Only public build metadata
    # appears in this frame; it contains neither game data nor addresses.
    frame = bytearray(756)
    frame[:8] = bytes.fromhex("2f485c0101000000")
    for offset, text in ((8, b"289"), (20, b"release"),
                         (36, b"OpenCommunityEdition/OpenCE"),
                         (180, b"OpenCE Windows | build 289 (release)")):
        frame[offset:offset + len(text)] = text
    assert hashlib.sha256(frame).hexdigest() == (
        "349f8548e3dda2427eb03325dc0ebb545af098eb426ef32d76c0b546cc33aeb5")
    program = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef unsigned char byte;
typedef uint16_t word;
typedef word message_header;
typedef int boolean;
#define TRUE 1
#define FALSE 0
#define NONE (-1)
#define csmemcpy memcpy
#define TEST_FLAG(f,b) ((f) & (1u << (b)))
#define SET_FLAG(f,b,v) ((f) = ((f) & ~(1u << (b))) | ((!!(v)) << (b)))
#define GET_MESSAGE_SIZE(h) ((h) >> 4)
#define match_assert(f,l,c) assert(c)
#define match_vassert(f,l,c,...) assert(c)
#define error(...) assert(!"stream rejected frame")
enum message_header_byte_order { _byte_order_host, _byte_order_network };
enum { RELIABLE_MESSAGE_MAXIMUM_SIZE=4096, _connection_closed_bit=4,
       _connection_reliable_stream_broken_bit=6, _error_silent,
       DATAGRAM_MAXIMUM_SIZE=4096, DISTRIBUTED_UNIT_STATE_MINIMUM_SIZE=1,
       DISTRIBUTED_RELAYED_ACTION_MINIMUM_SIZE=1 };
struct transport_address { int unused; };
struct queue { byte data[800]; unsigned offset, size; };
struct network_connection { unsigned flags, stream_messages_received; struct queue *reliable_incoming_queue; };
static int circular_queue_dequeue_data(struct queue *q, void *out, unsigned size, int consume)
{
    if (size > q->size - q->offset) return 0;
    memcpy(out, q->data + q->offset, size);
    if (consume) q->offset += size;
    return 1;
}
static unsigned circular_queue_size(struct queue *q) { return q->size - q->offset; }
static void network_connection_get_address(void *a, void *b, void *c) { assert(0); }
static int playing=1, distributed_handling_batch, dispatched;
static int game_in_progress(void) { return playing; }
static int network_aim_assist_handles_message(void const *p, word n) { return 0; }
static void network_aim_assist_handle_message(long i, void const *p, word n) { assert(0); }
static int map_share_handles_message(void const *p, word n) { return 0; }
static void map_share_handle_message(long i, void const *p, word n, int s) { assert(0); }
static int network_connection_last_read_was_unreliable(void) { return 0; }
static int distributed_handling_stream_message=1;
static int distributed_shield_policy_valid(long i, void const *p, word n) { assert(0); return 0; }
static struct { int received; } distributed_statistics;
'''
    program += block(header, "enum\n{") + ";\n" + structures + stubs + object_sizes + swap + reader + dispatch
    program += r'''
int main(void)
{
    struct queue q = {.data={CAPTURE_BYTES}, .size=764};
    byte next[] = {0x00,0x88,0x09,1,0,0,0,0};
    memcpy(q.data+756, next, sizeof(next));
    struct network_connection connection = {.reliable_incoming_queue=&q};
    word message[2048], size;
    for (int active=0; active<=1; active++) {
        playing=active; q.offset=0; connection.stream_messages_received=0;
        size=sizeof(message);
        assert(network_client_reliable_connection_read(&connection,message,&size,0));
        assert(size==756 && q.offset==756 && message[0]==0x2f48);
        network_distributed_handle_message(1,message,size);
        assert(distributed_statistics.received==0 && connection.flags==0);
        size=sizeof(message);
        assert(network_client_reliable_connection_read(&connection,message,&size,0));
        assert(size==8 && ((byte *)message)[2]==9 && q.offset==q.size);
        assert(connection.stream_messages_received==2 && connection.flags==0);
        size=sizeof(message);
        assert(!network_client_reliable_connection_read(&connection,message,&size,0));
    }
    return dispatched;
}
'''
    program = program.replace("CAPTURE_BYTES", ",".join(str(value) for value in frame))
    probe = tmp_path / "client_build.c"
    probe.write_text(program)
    executable = tmp_path / "client_build"
    subprocess.run(["clang", "-std=c11", "-Wall", "-Werror", "-Wno-unused-variable",
                    "-Wno-unused-function", "-fsanitize=address,undefined", str(probe),
                    "--output=" + str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
