#!/usr/bin/env python3
"""Check from this machine's network what an invite link needs (port/linux/src/p2p_signal.c,
p2p_lobby.c, p2p.c): each MQTT broker in brokers.txt answers a connection (MQTT 3.1.1 over TCP),
the server browser's public listings it keeps (how many, of which network versions), and the
STUN servers answer over UDP. With an invite link (or its 64 hex digits), also whether its host's
public listing is on the brokers: running, ended, or not there (a private game, or one that
ended).

    python3 tools/p2p_broker_probe.py [halo://join/<64 hex digits>]

It connects once to each server, listens for a few seconds, and prints what it found. It
publishes nothing."""

import argparse
import os
import re
import socket
import struct
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BROKERS = ROOT / "port/assets/network/brokers.txt"
STUN_SERVERS = ("stun.l.google.com:19302", "stun.cloudflare.com:3478")
SLOT_PREFIX = "hceu/3/lobby/s/"
LISTEN_SECONDS = 4.0


def brokers(path=BROKERS):
    """the host:port lines of a brokers.txt ("#" starts a comment)"""
    result = []
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            host, _, port = line.partition(":")
            result.append((host, int(port or 1883)))
    return result


def mqtt_string(text):
    data = text.encode()
    return struct.pack(">H", len(data)) + data


def mqtt_packet(kind, body):
    length, encoded = len(body), b""
    while True:
        byte, length = length % 128, length // 128
        encoded += bytes([byte | (0x80 if length else 0)])
        if not length:
            return bytes([kind]) + encoded + body


def read_packet(sock):
    """(first byte, body) of the next MQTT packet, or None when the connection ends"""
    head = sock.recv(1)
    if not head:
        return None
    length, shift = 0, 0
    while True:
        byte = sock.recv(1)
        if not byte:
            return None
        length |= (byte[0] & 0x7F) << shift
        shift += 7
        if not byte[0] & 0x80:
            break
    body = b""
    while len(body) < length:
        chunk = sock.recv(length - len(body))
        if not chunk:
            return None
        body += chunk
    return head[0], body


def listing_summary(payload):
    """(network version, ended, Unix time) of a public game's listing (p2p_lobby.c), or None"""
    if len(payload) < 14 or payload[:2] != b"HL" or payload[2] != 1:
        return None
    version = payload[3] << 8 | payload[4]
    return version, bool(payload[5] & 8), struct.unpack(">I", payload[10:14])[0]


def probe_broker(host, port, slot=None, listen=LISTEN_SECONDS):
    """a line about the broker, and the listings it kept: {topic: (version, ended, time)}"""
    started = time.monotonic()
    try:
        sock = socket.create_connection((host, port), timeout=10)
    except OSError as error:
        return f"cannot connect ({error})", {}
    with sock:
        sock.settimeout(10)
        client = "hceu-probe-" + os.urandom(4).hex()
        sock.sendall(mqtt_packet(0x10, mqtt_string("MQTT") + bytes([4, 0x02]) + struct.pack(">H", 30) +
                                 mqtt_string(client)))
        try:
            packet = read_packet(sock)
        except OSError as error:
            return f"connected, but no MQTT answer ({error})", {}
        if not packet or packet[0] >> 4 != 2 or len(packet[1]) < 2:
            return "connected, but the answer is not MQTT", {}
        if packet[1][1]:
            return f"refused the connection (code {packet[1][1]})", {}
        answered = time.monotonic() - started
        topics = [SLOT_PREFIX + "+"] + ([slot] if slot else [])
        body = struct.pack(">H", 1) + b"".join(mqtt_string(topic) + b"\0" for topic in topics)
        sock.sendall(mqtt_packet(0x82, body))
        listings = {}
        deadline = time.monotonic() + listen
        while time.monotonic() < deadline:
            sock.settimeout(max(0.05, deadline - time.monotonic()))
            try:
                packet = read_packet(sock)
            except socket.timeout:
                break
            except OSError:
                break
            if not packet:
                break
            kind, body = packet
            if kind >> 4 != 3 or len(body) < 2:
                continue
            (size,) = struct.unpack(">H", body[:2])
            topic = body[2:2 + size].decode(errors="replace")
            offset = 2 + size + (2 if kind & 0x06 else 0)
            summary = listing_summary(body[offset:])
            if summary and topic.startswith(SLOT_PREFIX):
                listings[topic] = summary
        return f"answered MQTT in {answered:.2f} s", listings


def probe_stun(server):
    host, _, port = server.partition(":")
    request = struct.pack(">HHI", 1, 0, 0x2112A442) + os.urandom(12)
    started = time.monotonic()
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.settimeout(3)
            sock.sendto(request, (socket.gethostbyname(host), int(port)))
            reply, _ = sock.recvfrom(512)
    except OSError as error:
        return f"no answer ({error})"
    if reply[:2] != b"\x01\x01" or reply[8:20] != request[8:20]:
        return "an answer that is not STUN's"
    return f"answered in {time.monotonic() - started:.2f} s"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("invite", nargs="?", help="an invite link, or its 64 hex digits")
    parser.add_argument("--brokers", type=Path, default=BROKERS, help="a brokers.txt (default: the game's)")
    args = parser.parse_args()
    slot = None
    if args.invite:
        match = re.search(r"([0-9a-fA-F]{64})", args.invite)
        if not match:
            parser.error("an invite link holds 64 hex digits")
        slot = SLOT_PREFIX + match.group(1)[:32].lower()
    found = {}
    for host, port in brokers(args.brokers):
        line, listings = probe_broker(host, port, slot)
        versions = {}
        for version, ended, _ in listings.values():
            if not ended:
                versions[version] = versions.get(version, 0) + 1
        kept = ", ".join(f"{count} of version {version}" for version, count in sorted(versions.items()))
        print(f"broker {host}:{port}: {line}" + (f"; public games listed: {kept or 'none'}" if "MQTT in" in line
                                                  else ""))
        if slot and slot in listings:
            found[f"{host}:{port}"] = listings[slot]
    for server in STUN_SERVERS:
        print(f"STUN {server}: {probe_stun(server)}")
    if slot:
        if not found:
            print("the invite's host: no listing on any broker (a private game, or its game ended)")
        for broker, (version, ended, stamp) in found.items():
            age = int(time.time()) - stamp
            print(f"the invite's host on {broker}: {'ended' if ended else 'running'}, network version "
                  f"{version}, listed {age} s ago")


if __name__ == "__main__":
    main()
