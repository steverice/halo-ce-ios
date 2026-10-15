"""tools/p2p_broker_probe.py's reading of brokers.txt, MQTT packets and listings (no network)"""

import socket
import struct

from tools import p2p_broker_probe as probe


def test_brokers_file(tmp_path):
    path = tmp_path / "brokers.txt"
    path.write_text("# a comment\nbroker.example:1883\n\n  other.example  # after\n")
    assert probe.brokers(path) == [("broker.example", 1883), ("other.example", 1883)]
    assert len(probe.brokers()) >= 1


def test_packets_round_trip():
    body = b"x" * 300
    left, right = socket.socketpair()
    with left, right:
        left.sendall(probe.mqtt_packet(0x30, body))
        assert probe.read_packet(right) == (0x30, body)


def test_listing_summary():
    listing = b"HL\x01" + struct.pack(">H", 23) + b"\x08" + struct.pack(">I", 5) + struct.pack(">I", 1700000000)
    assert probe.listing_summary(listing + b"\0" * 40) == (23, True, 1700000000)
    assert probe.listing_summary(b"HL\x02" + listing[3:] + b"\0" * 40) is None
    assert probe.listing_summary(b"") is None
