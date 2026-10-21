"""Choose a catalog case's measured window from a take: the heaviest clean stretch.

A take is replayed once to get a cost for every second of its recorded time (for example the mean GPU or frame time
of that second, or its actor count). This picks where a window of a given length should start:

- clean: no rewind (a death, a reload or a skipped cutscene) inside the window or in the margin after it, so the
  window is not cut off by a revert, and the window ends at or before the take's last controller state, because a
  replay ends there;
- heaviest: the highest mean cost over the window;
- earlier on a near tie: a separate clean window that starts before the heaviest one and costs within the tolerance
  (10%) of it wins, since an earlier start needs a shorter lead-in on every device.

    python tools/heavy_window.py COSTS.csv --window 60 --last-state 236 --rewind 3.9 --rewind 77.5 --rewind 234.5

COSTS.csv has one "second,cost" line per second of recorded time. Prints the start in seconds and the mean cost.
"""
import argparse
import csv

TOLERANCE = 0.10
REWIND_MARGIN = 2.0


def window_costs(costs, window):
    """[(start, mean cost)] for every start second at which a whole window fits in the series"""
    seconds = sorted(costs)
    values = [costs[second] for second in seconds]
    result = []
    for index, start in enumerate(seconds):
        end = start + window
        inside = [value for second, value in zip(seconds[index:], values[index:]) if second < end]
        if len(inside) >= window and seconds[index + len(inside) - 1] + 1 >= end:
            result.append((start, sum(inside) / len(inside)))
    return result


def choose(costs, window, rewinds=(), last_state=None, first_start=0.0, margin=REWIND_MARGIN, tolerance=TOLERANCE):
    """The (start, mean cost) of the chosen window, or None when no clean window fits."""
    clean = []
    for start, cost in window_costs(costs, window):
        end = start + window
        if start < first_start or (last_state is not None and end > last_state):
            continue
        if any(start <= rewind <= end + margin for rewind in rewinds):
            continue
        clean.append((start, cost))
    if not clean:
        return None
    best = max(clean, key=lambda candidate: (candidate[1], -candidate[0]))
    # a separate window (one that does not overlap the heaviest) that starts earlier and costs within the tolerance of
    # it wins: of two such windows the earlier needs the shorter lead-in. Windows that merely slide a second or two
    # around the heaviest one are the same stretch and do not count.
    rivals = [candidate for candidate in clean
              if candidate[0] + window <= best[0] and candidate[1] >= best[1] * (1.0 - tolerance)]
    return min(rivals, key=lambda candidate: candidate[0]) if rivals else best


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("costs", help="CSV of second,cost")
    parser.add_argument("--window", type=float, required=True)
    parser.add_argument("--rewind", type=float, action="append", default=[], help="a rewind's recorded second (repeatable)")
    parser.add_argument("--last-state", type=float, help="the take's last controller state, in seconds")
    parser.add_argument("--first-start", type=float, default=0.0, help="no window starts before this second (an anchor's)")
    args = parser.parse_args()
    with open(args.costs, newline="") as file:
        costs = {int(float(row[0])): float(row[1]) for row in csv.reader(file) if row and row[0][:1].isdigit()}
    picked = choose(costs, int(args.window), args.rewind, args.last_state, args.first_start)
    if picked is None:
        raise SystemExit("no clean window fits")
    print(f"start {picked[0]} s, mean cost {picked[1]:.3f}")


if __name__ == "__main__":
    main()
