"""Exercise the real SDL disc-image offer through dialog failure, cancel, and retry."""
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from harness import function, mutated  # noqa: E402

SOURCE = (ROOT / "port/linux/src/sdl_platform.c").read_text()
PRELUDE = r'''
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define SDLCALL
#define SDL_INIT_VIDEO 1
#define SDL_GLOB_CASEINSENSITIVE 1
#define SDL_MESSAGEBOX_INFORMATION 1
#define SDL_MESSAGEBOX_ERROR 2
#define SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT 1
#define SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT 2
typedef struct { int value; } SDL_AtomicInt;
typedef struct { const char *name, *pattern; } SDL_DialogFileFilter;
typedef struct { int flags, id; const char *text; } SDL_MessageBoxButtonData;
typedef struct { int flags; void *window; const char *title, *message;
                 int button_count; const SDL_MessageBoxButtonData *buttons; void *color; } SDL_MessageBoxData;
static int mode, questions, extracts, errors;
static char **SDL_GlobDirectory(const char *dir, const char *pattern, int flags, int *count) {
    (void)dir; (void)pattern; (void)flags; *count = 0; return NULL;
}
static void SDL_free(void *pointer) { free(pointer); }
static const char *SDL_GetError(void) { return "desktop portal unavailable"; }
static void SDL_SetAtomicInt(SDL_AtomicInt *value, int next) { value->value = next; }
static int SDL_GetAtomicInt(SDL_AtomicInt *value) { return value->value; }
static void SDL_PumpEvents(void) {}
static void SDL_Delay(int time) { (void)time; }
static void SDL_ShowOpenFileDialog(void (*callback)(void *, const char *const *, int),
                                   void *userdata, void *window, const SDL_DialogFileFilter *filters,
                                   int count, const char *default_location, bool many) {
    static const char *cancel[] = { NULL };
    static const char *picked[] = { "/tmp/reviewed.iso", NULL };
    (void)window; (void)filters; (void)count; (void)default_location; (void)many;
    callback(userdata, mode == 1 ? NULL : mode == 2 ? cancel : picked, 0);
}
static BOOL config_boolean(const char *setting) { (void)setting; return FALSE; }
static double config_real(const char *setting) { (void)setting; return 0.0; }
static BOOL SDL_Init(int flags) { (void)flags; return TRUE; }
static BOOL SDL_ShowMessageBox(const SDL_MessageBoxData *box, int *answer) {
    (void)box; questions++;
    *answer = mode == 2 && questions == 2 ? 0 : 1;
    if (*answer == 0) puts("USER_CANCEL");
    return TRUE;
}
static void SDL_ShowSimpleMessageBox(int flags, const char *title, const char *message, void *window) {
    (void)flags; (void)title; (void)window; errors++;
    puts(strstr(message, "No file dialog") ? "NO_DIALOG" : "EXTRACT_FAIL");
}
static void platform_log(const char *fmt, ...) { (void)fmt; }
static BOOL data_extract(const char *image, const char *destination, char *error, int size) {
    (void)image; (void)destination; extracts++;
    if (mode == 3 && extracts == 1) {
        snprintf(error, (size_t)size, "synthetic extraction failure"); return FALSE;
    }
    return TRUE;
}
struct data_image_choice { SDL_AtomicInt done; BOOL failed; char path[1024]; char error[256]; };
'''
MAIN = r'''
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    mode = atoi(argv[1]);
    if (platform_offer_game_data("/tmp/reviewed-data")) {
        if (mode == 3 && extracts == 2 && errors == 1 && questions == 2) {
            puts("EXTRACT_OK"); return 0;
        }
        return 3;
    }
    return 4;
}
'''


def compile_probe(tmp_path, *, missing_failure_signal=False):
    compiler = shutil.which("clang") or shutil.which("cc")
    assert compiler, "A C compiler is required for the disc-image offer probe"
    tmp_path.mkdir(parents=True, exist_ok=True)
    callback = function(SOURCE, "data_image_chosen")
    if missing_failure_signal:
        callback = mutated(callback, "choice->failed = TRUE;", "choice->failed = FALSE;")
    code = PRELUDE + "\n" + "\n".join((callback, function(SOURCE, "data_find_image"),
              function(SOURCE, "data_choose_image"), function(SOURCE, "platform_offer_game_data"))) + "\n" + MAIN
    source = tmp_path / "probe.c"
    binary = tmp_path / "probe"
    source.write_text(code)
    compiled = subprocess.run([compiler, "-std=c99", "-Wall", "-Werror", "-Wno-unused-function",
                               str(source), "-o", str(binary)], capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stderr
    return binary


def run_case(binary, mode):
    return subprocess.run([str(binary), str(mode)], capture_output=True, text=True, timeout=2)


def test_dialog_failure_cancel_and_extraction_retry(tmp_path):
    binary = compile_probe(tmp_path)
    failed = run_case(binary, 1)
    assert failed.returncode == 0 and failed.stdout.count("NO_DIALOG") == 1, failed.stderr
    canceled = run_case(binary, 2)
    assert canceled.returncode == 0 and canceled.stdout.count("USER_CANCEL") == 1, canceled.stderr
    assert "NO_DIALOG" not in canceled.stdout
    retry = run_case(binary, 3)
    assert retry.returncode == 0 and retry.stdout.count("EXTRACT_FAIL") == 1, retry.stderr
    assert retry.stdout.count("EXTRACT_OK") == 1


def test_no_dialog_negative_control_does_not_complete(tmp_path):
    binary = compile_probe(tmp_path, missing_failure_signal=True)
    try:
        failed = run_case(binary, 1)
    except subprocess.TimeoutExpired:
        return  # The old loop would keep asking after a failed desktop portal.
    assert "NO_DIALOG" not in failed.stdout
