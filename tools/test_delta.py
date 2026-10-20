"""The Delta game list: the request layer (port/linux/src/posix_update.c's posix_https_request) against a stand-in
server on this machine, and the list's parsing and announcing (port/ios/tests/delta_probe.c, which tools/ios_test.py
also runs). Nothing here reaches the real service."""
import http.server
import json
import shutil
import subprocess
import threading
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
MBEDTLS = ROOT / "port/third_party/mbedtls"
CLANG = shutil.which("clang")

pytestmark = pytest.mark.skipif(not CLANG, reason="needs clang")

LIST = {"games": [{"invite": "a" * 64, "name": "Test", "map": "bloodgulch", "version": 29}]}


def compile_native(arguments, output):
    result = subprocess.run([CLANG, *arguments, "-o", str(output)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr[-4000:]


@pytest.fixture(scope="module")
def native(tmp_path_factory):
    """Mbed TLS built for this machine, and a directory for the programs that use it"""
    work = tmp_path_factory.mktemp("delta")
    objects = []
    processes = []
    for source in sorted((MBEDTLS / "library").glob("*.c")):
        object_file = work / (source.stem + ".o")
        objects.append(str(object_file))
        processes.append(subprocess.Popen(
            [CLANG, "-std=gnu11", "-O1", "-w", f"-I{MBEDTLS / 'include'}", f"-I{MBEDTLS / 'library'}", "-c",
             str(source), "-o", str(object_file)], stderr=subprocess.PIPE, text=True))
        if len(processes) >= 8:
            assert processes.pop(0).wait() == 0
    for process in processes:
        assert process.wait() == 0, process.stderr.read()[-2000:]
    return work, objects


@pytest.fixture(scope="module")
def transport(native):
    """tools/delta_transport_check.c with posix_update.c and Mbed TLS"""
    work, objects = native
    program = work / "delta_transport_check"
    compile_native(["-std=gnu11", "-O1", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Iport/linux/src",
                    f"-I{MBEDTLS / 'include'}", "tools/delta_transport_check.c", "port/linux/src/posix_update.c",
                    *objects], program)
    return program


@pytest.fixture(scope="module")
def worker(native):
    """tools/delta_worker_check.c with p2p_delta.c, delta_list.c and the request layer, p2p_delta.c compiled
    beside a platform.h that declares only what it takes from the game's"""
    work, objects = native
    shim = work / "shim"
    shim.mkdir()
    (shim / "platform.h").write_text("#include <pthread.h>\nvoid platform_log(const char *format, ...);\n"
                                     "void Sleep(unsigned long milliseconds);\n")
    (shim / "p2p_delta.c").write_text((ROOT / "port/linux/src/p2p_delta.c").read_text())
    program = work / "delta_worker_check"
    compile_native(["-std=gnu11", "-O1", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Iport/linux/src",
                    f"-I{MBEDTLS / 'include'}", "tools/delta_worker_check.c", str(shim / "p2p_delta.c"),
                    "port/linux/src/delta_list.c", "port/linux/src/posix_update.c", *objects], program)
    return program


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        self.requests = []
        super().__init__(("127.0.0.1", 0), Handler)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *arguments):
        pass

    def respond(self, status, body, chunked=False):
        data = body if isinstance(body, bytes) else body.encode()
        self.send_response(status)
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for start in range(0, len(data), 7):
                piece = data[start:start + 7]
                self.wfile.write(b"%x\r\n%s\r\n" % (len(piece), piece))
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(data)
        self.close_connection = True

    def handle_request(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode() if length else ""
        self.server.requests.append({"method": self.command, "path": self.path, "body": body,
                                     "agent": self.headers.get("User-Agent"),
                                     "type": self.headers.get("Content-Type")})
        if self.path == "/v1/games":
            self.respond(200, json.dumps(LIST))
        elif self.path == "/chunked":
            self.respond(200, json.dumps(LIST), chunked=True)
        elif self.path == "/v1/announce":
            self.respond(200, "ok 150")
        elif self.path == "/v1/withdraw":
            self.respond(200, "ok")
        elif self.path == "/refused":
            self.respond(403, "refused")
        elif self.path == "/slow":
            time.sleep(3)
            self.respond(200, "late")
        elif self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/v1/games")
            self.send_header("Content-Length", "0")
            self.end_headers()
        else:
            self.respond(404, "404 not found")

    do_GET = do_POST = handle_request


@pytest.fixture()
def server():
    server = Server()
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield server
    server.shutdown()
    server.server_close()


def request(transport, server, path, *arguments):
    result = subprocess.run([str(transport), f"http://127.0.0.1:{server.server_address[1]}{path}", *arguments],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    fields = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
    return int(fields["status"]), fields.get("error", ""), fields.get("body", "")


def test_get_reads_the_list(transport, server):
    status, error, body = request(transport, server, "/v1/games")
    assert status == 200 and json.loads(body) == LIST
    assert server.requests == [{"method": "GET", "path": "/v1/games", "body": "", "agent": "VISR", "type": None}]


def test_get_reads_a_chunked_answer(transport, server):
    status, error, body = request(transport, server, "/chunked")
    assert status == 200 and json.loads(body) == LIST


def test_post_sends_the_form(transport, server):
    form = "invite=" + "a" * 64 + "&name=Fun%20%26%20games&delta=0"
    status, error, body = request(transport, server, "/v1/announce", form)
    assert (status, body) == (200, "ok 150")
    assert server.requests == [{"method": "POST", "path": "/v1/announce", "body": form, "agent": "VISR",
                                "type": "application/x-www-form-urlencoded"}]


def test_other_statuses_have_no_body(transport, server):
    assert request(transport, server, "/refused", "x=1")[::2] == (403, "")
    assert request(transport, server, "/missing")[::2] == (404, "")
    # a redirect is reported, not followed
    assert request(transport, server, "/redirect")[::2] == (302, "")
    assert [item["path"] for item in server.requests] == ["/refused", "/missing", "/redirect"]


def test_a_body_larger_than_the_buffer_fails(transport, server):
    status, error, body = request(transport, server, "/v1/games", "-", "40")
    assert status == 0 and body == ""
    # exactly the size, with the end after it, fits
    size = len(json.dumps(LIST)) + 1
    assert request(transport, server, "/v1/games", "-", str(size))[0] == 200
    assert request(transport, server, "/v1/games", "-", str(size - 1))[0] == 0


def test_a_slow_server_times_out(transport, server):
    started = time.monotonic()
    status, error, body = request(transport, server, "/slow", "-", "1024", "500")
    assert status == 0 and body == "" and time.monotonic() - started < 3


def test_only_this_machine_may_be_plain(transport, server):
    port = server.server_address[1]
    for url in (f"http://192.0.2.1:{port}/v1/games", "http://halo.example/v1/games", "ftp://127.0.0.1/x", "nonsense"):
        result = subprocess.run([str(transport), url], capture_output=True, text=True, timeout=30)
        assert result.stdout.startswith("status=0\n"), (url, result.stdout)
    # TLS to a server that does not speak it is a failure
    status, error, body = request(transport, server, "/v1/games")
    result = subprocess.run([str(transport), f"https://127.0.0.1:{port}/v1/games", "-", "1024", "1000"],
                            capture_output=True, text=True, timeout=30)
    assert result.stdout.startswith("status=0\n")
    assert [item["path"] for item in server.requests].count("/v1/games") == 1


def test_the_thread_browses_announces_and_withdraws(worker, server):
    result = subprocess.run([str(worker)], capture_output=True, text=True, timeout=60,
                            env={"HALO_DELTA_URL": f"http://127.0.0.1:{server.server_address[1]}/"})
    assert result.returncode == 0, result.stdout + result.stderr
    assert "GAMES 1 Test bloodgulch 29" in result.stdout
    assert "PHASE quit" in result.stdout
    requests = [(item["method"], item["path"], item["body"]) for item in server.requests]
    invite = {letter: letter * 64 for letter in "abc"}

    def form(letter, players):
        return (f"invite={invite[letter]}&name=Checked%20game&map=bloodgulch&engine=2&players={players}"
                "&maximum_players=16&open=1&version=29&teams=0&delta=0")

    assert requests == [
        ("GET", "/v1/games", ""),
        ("POST", "/v1/announce", form("a", 3)),
        ("POST", "/v1/withdraw", f"invite={invite['a']}"),
        ("POST", "/v1/announce", form("b", 3)),
        ("POST", "/v1/withdraw", f"invite={invite['b']}"),
        ("POST", "/v1/announce", form("c", 5)),
        ("POST", "/v1/withdraw", f"invite={invite['c']}"),
    ], result.stdout
    assert {item["agent"] for item in server.requests} == {"VISR"}


def test_the_probe_of_the_list_and_announcing(tmp_path):
    program = tmp_path / "delta_probe"
    compile_native(["-std=gnu89", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    "-fsanitize=address,undefined", "-Iport/linux/src", "-iquote", "port/linux/include",
                    "port/ios/tests/delta_probe.c", "port/linux/src/delta_list.c"], program)
    result = subprocess.run([str(program)], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0 and "delta probe ok" in result.stdout, result.stderr
