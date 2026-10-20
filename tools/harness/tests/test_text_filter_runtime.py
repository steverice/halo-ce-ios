"""The display filter changes only displayed copies while its toggle is on."""

from harness import build, read, run


def test_streamer_toggle_preserves_source_text():
    names = ("text_filter.h", "text_filter_fold.inc", "text_filter_terms.inc",
             "text_filter_match.c", "text_filter.c")
    generated = tuple((name, read(f"port/linux/game/{name}")) for name in names)
    generated += (("under_test.inc", '#include "text_filter_match.c"\n#include "text_filter.c"\n'),)
    code, output = run(build("text_filter_runtime", generated), "toggle")
    assert code == 0, output
