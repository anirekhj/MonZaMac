#!/usr/bin/env python3
"""Summarize existing room-sweep logs without launching the game.

  tools/perf_sweep.py report   per-room table -> build/sweep/report.txt

The legacy room-warp hook is not implemented by MonZaMac. For new measurements,
use perf_scene.py with a copied save or record/replay a route in the Developer tab.
"""
import json
import os
import re
import sys
from typing import NamedTuple

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
OUT = os.path.join(ROOT, 'build', 'sweep')
FIRST, STEP, SETTLE = 2700, 360, 150


class Sample(NamedTuple):
    frame: int
    fps: float
    max_ms: float
    level: str | None
    passes: int
    gpu_ms: float
    fbcopy: int
    frames: int


# Accept both current detailed skip counters and the older room-labelled lines.
# Frame deltas, rather than a presumed logging period, normalize interval totals.
LINE = re.compile(
    r'\[metal\] t=[\d.]+ frame (\d+) ([\d.]+) fps max ([\d.]+)ms'
    r'(?: level (\S+))? \((\d+) draws, \d+ skipped(?: \([^)]*\))?, '
    r'(\d+) passes.*?\bgpu ([\d.]+)ms.*?\bfbcopy (\d+)\b')


def parse_samples(text):
    samples = []
    previous = 0
    for match in LINE.finditer(text):
        frame, fps, maximum, level, _, passes, gpu, copies = match.groups()
        frame = int(frame)
        frames = frame - previous if frame > previous else frame
        previous = frame
        if frames > 0:
            samples.append(Sample(frame, float(fps), float(maximum), level,
                                  int(passes), float(gpu), int(copies), frames))
    return samples


def run():
    raise SystemExit('Room sweeps are unavailable: MonZaMac does not implement the legacy '
                     'LARECOMP_WARP_AT hook. Use tools/perf_scene.py with a copied save, '
                     'or record/replay a route in the Developer tab. '
                     'Existing sweep logs can still be summarized with report.')


def summarize(chunk, text):
    rows = []
    lines = parse_samples(text)
    crashed = '*** signal' in text or '[crash]' in text
    for i, (level, locator) in enumerate(chunk):
        start, end = FIRST + i * STEP + SETTLE, FIRST + (i + 1) * STEP - 30
        samples = [s for s in lines if start <= s.frame < end]
        if not samples:
            rows.append((level, locator, None, None, None, None, None,
                         'no data' + (' (crash)' if crashed else '')))
            continue
        frames = sum(s.frames for s in samples)
        per_frame = lambda field: sum(getattr(s, field) for s in samples) / frames
        seen = samples[-1].level
        note = 'level not logged' if seen is None else f'(level {seen})' if seen != level else ''
        rows.append((level, locator, min(s.fps for s in samples), max(s.max_ms for s in samples),
                     per_frame('passes'), per_frame('gpu_ms'), per_frame('fbcopy'), note))
    return rows


def report():
    rows = []
    n = 0
    while os.path.exists(os.path.join(OUT, f'batch_{n:02d}.json')):
        with open(os.path.join(OUT, f'batch_{n:02d}.json')) as f:
            chunk = json.load(f)
        with open(os.path.join(OUT, f'batch_{n:02d}.log'), errors='replace') as f:
            rows.extend(summarize(chunk, f.read()))
        n += 1
    rows.sort(key=lambda r: (r[2] is None, r[2] if r[2] is not None else 0))
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, 'report.txt'), 'w') as f:
        f.write(f'{"level":22} {"locator":34} {"minfps":>6} {"maxms":>5} {"passes/f":>8} {"gpu/f":>6} {"fbcopy/f":>8}\n')
        for level, locator, fps, maximum, passes, gpu, copies, note in rows:
            if fps is None:
                f.write(f'{level:22} {locator:34} {note}\n')
            else:
                f.write(f'{level:22} {locator:34} {fps:6.1f} {maximum:5.0f} {passes:8.1f} {gpu:6.1f} {copies:8.1f} {note}\n')
    slow = [r for r in rows if r[2] is not None and (r[2] < 58 or r[3] > 25)]
    print(f'{len(rows)} rooms, {len(slow)} slow or hitchy, {sum(1 for r in rows if r[2] is None)} without data')


if __name__ == '__main__':
    action = sys.argv[1] if len(sys.argv) > 1 else 'run'
    if action in ('report', '--report'):
        report()
    elif action == 'run':
        run()
    else:
        raise SystemExit('usage: perf_sweep.py report')
