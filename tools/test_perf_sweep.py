#!/usr/bin/env python3
"""Current/legacy performance-log parsing and the unsupported sweep guard."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('perf_sweep', ROOT / 'tools/perf_sweep.py')
sweep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sweep)


def current(frame, *, passes=100, gpu=200, copies=1, fps=60):
    return (f'[metal] t={frame / 60:.1f} frame {frame} {fps}.0 fps max 23ms '
            f'(180 draws, 2 skipped (0 no shader, 1 failed, 1 compiling), '
            f'{passes} passes, 0 dispatches, 50 flushes, gpu {gpu}ms, wait 0ms, '
            f'compile 0/0ms, fbcopy {copies} | tex 12 (20 MB))\n')


class SweepTests(unittest.TestCase):
    def test_current_samples_use_actual_frame_deltas(self):
        text = current(2800) + current(2850) + current(2880, passes=90, gpu=150, fps=40)
        samples = sweep.parse_samples(text)
        self.assertEqual([s.frames for s in samples], [2800, 50, 30])
        row = sweep.summarize([('plaza', 'start')], text)[0]
        self.assertEqual(row[:4], ('plaza', 'start', 40, 23))
        self.assertAlmostEqual(row[4], 190 / 80)
        self.assertAlmostEqual(row[5], 350 / 80)
        self.assertAlmostEqual(row[6], 2 / 80)
        self.assertEqual(row[7], 'level not logged')

    def test_legacy_level_and_skip_format(self):
        text = ('[metal] t=50.0 frame 2850 58.5 fps max 28ms level plaza '
                '(180 draws, 0 skipped, 80 passes, gpu 160ms, fbcopy 2)\n')
        sample = sweep.parse_samples(text)[0]
        self.assertEqual((sample.level, sample.fps, sample.passes, sample.gpu_ms), ('plaza', 58.5, 80, 160))
        self.assertEqual(sweep.summarize([('plaza', 'start')], text)[0][-1], '')
        self.assertEqual(sweep.summarize([('other', 'start')], text)[0][-1], '(level plaza)')

    def test_generation_output_is_not_a_game_frame_sample(self):
        self.assertFalse(sweep.parse_samples('[framegen-present] real 300 generated 280 display 100.0 fps dropped 1'))
        self.assertEqual(sweep.summarize([('plaza', 'start')], '[crash] stopped')[0][-1], 'no data (crash)')

    def test_report_writes_current_samples(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(sweep, 'OUT', tmp):
            (Path(tmp) / 'batch_00.json').write_text(json.dumps([['plaza', 'start']]))
            (Path(tmp) / 'batch_00.log').write_text(current(2800) + current(2850))
            with contextlib.redirect_stdout(io.StringIO()) as output:
                sweep.report()
            self.assertIn('1 rooms, 0 slow or hitchy, 0 without data', output.getvalue())
            report = (Path(tmp) / 'report.txt').read_text()
            self.assertIn('plaza', report)
            self.assertIn('level not logged', report)

    def test_unsupported_run_stops_before_launching_game(self):
        result = subprocess.run([sys.executable, str(ROOT / 'tools/perf_sweep.py'), 'run'],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('does not implement the legacy LARECOMP_WARP_AT hook', result.stderr)
        self.assertIn('perf_scene.py', result.stderr)


if __name__ == '__main__':
    unittest.main()
