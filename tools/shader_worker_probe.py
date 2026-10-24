#!/usr/bin/env python3
"""Exercise worker recovery across independent native probe processes."""
import pathlib
import struct
import subprocess
import tempfile
import sys
if len(sys.argv) == 1:
    build = pathlib.Path(tempfile.mkdtemp(prefix="shader-worker-probe-"))
    probe = build / "shader-upload-probe"
    subprocess.run(["xcrun", "clang", "-fobjc-arc", "-fblocks", "-DHALO_NATIVE_MAC=1", "-DHALO_SHADER_UPLOAD=1", "-DHALO_SHADER_UPLOAD_TESTING=1", '-DHALO_SHADER_CLOUD="iCloud.test"', "-Iport/ios/host", "-Iport/runtime/include", "-framework", "Foundation", "-framework", "AppKit", "-framework", "CloudKit", "-lz", "-lsqlite3", "port/ios/tests/shader_upload_probe.m", "port/ios/host/host_shader_upload.m", "port/ios/host/host_benchmark_outbox.m", "port/ios/host/host_shader_pack.c", "port/runtime/shader_observations.c", "-o", str(probe)], check=True)
    outbox = build / "shader-outbox-probe"
    subprocess.run(["xcrun", "clang", "-fobjc-arc", "-fblocks", "-Iport/ios/host", "-framework", "Foundation", "port/ios/tests/shader_outbox_probe.m", "port/ios/host/host_benchmark_outbox.m", "-o", str(outbox)], check=True)
    subprocess.run([str(outbox)], check=True)
    sys.argv.append(str(probe))
with tempfile.TemporaryDirectory() as folder:
    root = pathlib.Path(folder)
    (root / "maps").mkdir()
    (root / "maps" / "sample.map").write_bytes(b"head" + struct.pack("<I", 7) + bytes(128))
    for mode in ("persist", "recover", "sent"):
        subprocess.run([sys.argv[1], folder, mode], check=True)
print("shader worker: persistence, recovery, sent membership, opt-out and retry probes passed")

for mode in ("offline", "quota", "race", "schema", "limits", "threshold", "lateimport", "timeout-race", "producer-race", "activation-race", "fallback"):
    with tempfile.TemporaryDirectory() as folder:
        root = pathlib.Path(folder)
        (root / "maps").mkdir()
        (root / "maps" / "sample.map").write_bytes(b"head" + struct.pack("<I", 7) + bytes(128))
        subprocess.run([sys.argv[1], folder, mode], check=True)
print("shader worker: offline retry, daily quota, schema pause and late opt-out completion passed")

with tempfile.TemporaryDirectory() as folder:
    root = pathlib.Path(folder)
    (root / "maps").mkdir()
    (root / "maps" / "sample.map").write_bytes(b"head" + struct.pack("<I", 7) + bytes(128))
    for mode in ("pixelpersist", "pixelrecover"):
        subprocess.run([sys.argv[1], folder, mode], check=True)
print("shader worker: 200 pixel descriptors persist and recover within the journal limit")
