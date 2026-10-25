"""Network versions 30 to 32: the join range and gates (halo_network_version.h), and that the sources
use them where a machine decides what to take or send."""
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text()


def test_probe_of_range_gates_and_stamp_window():
    with tempfile.TemporaryDirectory() as directory:
        executable = Path(directory) / "network_version_probe"
        subprocess.run(["clang", "-std=c11", "-Wall", "-Werror", "-o", str(executable),
                        str(ROOT / "port/ios/tests/network_version_probe.c")], check=True)
        result = subprocess.run([str(executable)], capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr


def test_host_advertises_its_version_and_clients_gate_by_range():
    server = read("source/networking/network_server_message_handler.c")
    assert "advertisement.reserved[HALO_PORT_ADVERTISED_VERSION_OFFSET] = (byte)(HALO_PORT_NETWORK_VERSION & 0xFF)" in server
    client = read("source/networking/network_client_manager.c")
    assert "halo_network_version_joinable(ours, theirs)" in client
    assert "network_game_client_host_version = " in client
    assert "halo_network_version_joinable(" in read("port/linux/src/p2p_lobby.c")
    assert "halo_network_version_joinable(" in read("port/ios/host/host_lan.c")


def test_pregame_keep_alives_send_zeros():
    server = read("source/networking/network_server_manager.c")
    assert "struct message_server_pregame_keep_alive message_packet = { 0 };" in server
    assert "struct message_server_postgame_keep_alive message_packet = { 0 };" in server


def test_stream_only_messages_and_stamp_window_are_enforced():
    source = read("port/linux/game/network_distributed.c")
    for kind in ("_distributed_message_client_ready", "_distributed_message_client_identity",
                 "_distributed_message_hit_reports"):
        assert f"header.type == {kind}" in source
    assert "!distributed_handling_stream_message" in source
    assert source.count("halo_network_stamp_accepts(") == 2
    assert "void network_distributed_clock_set(" in source
    assert "network_distributed_clock_set();" in read("source/game/game_time.c")


def test_coop_friendly_fire_byte_is_byte_two_of_the_input_entry():
    source = read("port/linux/game/network_distributed.c")
    start = source.index("struct distributed_player_input\n{")
    body = source[start:source.index("};", start)]
    fields = [line.strip() for line in body.splitlines() if line.strip().endswith(";") and not line.strip().startswith("/*")]
    assert fields[0].startswith("byte player_index") and fields[1].startswith("byte structure_bsp_index")
    assert fields[2].startswith("byte coop_no_friendly_fire")


def test_documented_options_content_and_legacy_settings_fixtures():
    with tempfile.TemporaryDirectory() as directory:
        executable = Path(directory) / "network_content_probe"
        subprocess.run(["clang", "-std=c11", "-Wall", "-Werror", "-o", str(executable),
                        str(ROOT / "port/ios/tests/network_content_probe.c")], check=True)
        result = subprocess.run([str(executable)], capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr


def test_host_version_rules_use_the_capability_helpers():
    """Each rule a host version turns on is asked of halo_network_version.h, never compared by number."""
    import re
    uses = {
        "port/linux/game/network_coop.c": "halo_network_version_has_ally_switch(network_game_host_version())",
        "port/linux/game/network_damage.c": "halo_network_version_has_static_shields(network_game_host_version())",
        "source/networking/network_client_message_handler.c":
            "halo_network_version_has_mode_options(network_game_host_version())",
        "port/linux/game/network_distributed.c":
            "halo_network_version_shield_selector_valid(network_game_host_version(), message.selector)",
        "source/networking/network_client_manager.c": "halo_network_version_has_game_kind(",
    }
    for path, use in uses.items():
        assert use in read(path), path
    assert "halo_network_version_has_shield_policy(network_game_host_version())" in read(
        "port/linux/game/network_distributed.c")
    numeric = re.compile(r"(network_game_host_version\(\)|\.version)\s*(<|>|<=|>=)\s*\d")
    for path in list(uses) + ["port/linux/game/network_aim_assist.c"]:
        assert not numeric.search(read(path)), path
