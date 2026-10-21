"""tools/heavy_window.py: the heaviest clean stretch of a take."""
import heavy_window


def series(values):
    return {second: value for second, value in enumerate(values)}


def test_picks_the_heaviest_window():
    costs = series([1] * 10 + [5] * 10 + [1] * 10)
    assert heavy_window.choose(costs, 10)[0] == 10


def test_a_rewind_in_the_window_or_its_margin_rules_it_out():
    costs = series([1] * 10 + [5] * 10 + [1] * 10)
    # a rewind at 21 s sits inside the heavy stretch; at 21 s with a window ending at 20 it is inside the 2 s margin too
    start = heavy_window.choose(costs, 10, rewinds=[21])[0]
    assert not start <= 21 <= start + 10 + 2


def test_the_window_stays_inside_the_last_controller_state():
    costs = series([1] * 10 + [5] * 10 + [1] * 10)
    assert heavy_window.choose(costs, 10, last_state=15)[0] == 5


def test_a_near_tie_prefers_the_earlier_window():
    # 100 then 95 (within 10%): the earlier wins; 85 (outside 10%) would not
    costs = series([100] * 10 + [1] * 10 + [95] * 10)
    assert heavy_window.choose(costs, 10)[0] == 0
    costs = series([85] * 10 + [1] * 10 + [100] * 10)
    assert heavy_window.choose(costs, 10)[0] == 20
    costs = series([95] * 10 + [1] * 10 + [100] * 10)
    assert heavy_window.choose(costs, 10)[0] == 0
    # windows sliding around the same peak are one stretch, not a tie
    costs = series([1] * 10 + [5] * 10 + [1] * 10)
    assert heavy_window.choose(costs, 10)[0] == 10


def test_the_heavier_later_window_wins_outside_the_tolerance():
    costs = series([50] * 10 + [100] * 10)
    assert heavy_window.choose(costs, 10)[0] == 10


def test_no_clean_window_fits():
    assert heavy_window.choose(series([1] * 5), 10) is None
    assert heavy_window.choose(series([1] * 30), 10, rewinds=range(0, 30, 3)) is None


def test_first_start_keeps_the_window_after_an_anchor():
    costs = series([9] * 10 + [1] * 20)
    assert heavy_window.choose(costs, 10, first_start=10)[0] == 10
