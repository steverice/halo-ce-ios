"""Exercise the production LAN policy for each Apple host without discovery."""
from pathlib import Path
import subprocess

import pytest

from test_menu_cancel import block

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("native,catalyst,ipad,entitled,expected", [
    (1, 0, 0, 0, 1), (0, 1, 0, 0, 1), (0, 0, 1, 0, 1),
    (0, 0, 0, 1, 1), (0, 0, 0, 0, 0),
])
def test_broadcast_policy(tmp_path, native, catalyst, ipad, entitled, expected):
    policy = block((ROOT / "port/ios/host/host_bonjour.m").read_text(),
                   "static int broadcast_allowed(void)")
    policy = policy.replace("NSProcessInfo.processInfo.isiOSAppOnMac", str(ipad))
    policy = policy.replace("NSProcessInfo.processInfo.isMacCatalystApp", str(catalyst))
    source = tmp_path / "policy.c"
    binary = tmp_path / "policy"
    source.write_text(f"""
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#define HALO_NATIVE_MAC {native}
#define HALO_MULTICAST_ENTITLED {entitled}
#define HOST_LOG_INFO 0
#define host_logf(...) ((void)0)
static const char *setting;
static void host_config_string(const char *key, const char *fallback, char *out, size_t size) {{
    (void)key; (void)fallback;
    assert(strlen(setting) < size);
    strcpy(out, setting);
}}
{policy}
int main(int argc, char **argv) {{
    assert(argc == 3);
    setting = argv[1];
    assert(broadcast_allowed() == atoi(argv[2]));
    return 0;
}}
""")
    subprocess.run(["clang", "-Wall", "-Werror", str(source), "--output=" + str(binary)], check=True)
    for setting, allowed in [("auto", expected), ("on", 1), ("off", 0)]:
        subprocess.run([str(binary), setting, str(allowed)], check=True)
