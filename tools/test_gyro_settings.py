#!/usr/bin/env python3
"""Gyro persistence/default/range checks using temporary settings and no SDL/device."""
import math
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include "runtime/settings.h"
#include "runtime/ui/i18n.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef __APPLE__
bool fg_supported(void) { return false; }
bool temporal_supported(void) { return false; }
#endif
int g_nmodules = 2;
const char *const ui_language_codes[UI_LANGUAGE_COUNT] = {
    "auto", "en", "it", "fr", "es", "de", "pt-BR", "ja", "zh-Hans", "zh-Hant", "ko", "ru"
};
int ui_auto_language(void) { return UI_LANG_EN; }
bool bind_settings_load(const char *key, const char *value) { (void)key; (void)value; return false; }
void bind_settings_save(FILE *file) { (void)file; }
int main(int argc, char **argv) {
    settings_load();
    printf("%d %.9g %d %d %d %.9g %d\n", g_settings.gyro_camera, g_settings.gyro_speed,
           g_settings.invert_gyro_x, g_settings.invert_gyro_y, g_settings.mouse_camera,
           g_settings.mouse_speed, g_settings.invert_mouse_x);
    if (argc == 3) { g_settings.gyro_camera = atoi(argv[1]); g_settings.gyro_speed = strtof(argv[2], NULL); }
    settings_save();
    return 0;
}
'''
LEGACY = 'mouse_camera = middle\nmouse_speed = 2.25\ninvert_mouse_x = 1\n'
CASES = [('', 0, 1.0, 0, 0)]
for value, mode in [('off', 0), ('hold', 1), ('always', 2), ('hold \t\r', 1),
                    ('invalid', 0), ('1', 0), ('-1', 0), ('hold extra', 0), ('', 0)]:
    CASES.append((f'gyro_camera = {value}\n', mode, 1.0, 0, 0))
for value, speed in [('0.1', .1), ('5', 5), ('1.375', 1.375), ('-1', .1), ('0', .1),
                     ('0.05', .1), ('5.1', 5), ('999', 5), ('nan', 1), ('inf', 1),
                     ('-inf', 1), ('1e9999', 1), ('junk', 1), ('2oops', 1), ('', 1), ('2.25 \t\r', 2.25)]:
    CASES.append((f'gyro_camera = hold\ngyro_speed = {value}\n', 1, speed, 0, 0))
for x, y in [(0, 0), (1, 0), (0, 1), (1, 1), (-1, 2)]:
    CASES.append((f'gyro_camera = always\ninvert_gyro_x = {x}\ninvert_gyro_y = {y}\n', 2, 1, int(bool(x)), int(bool(y))))


def read_saved(path):
    pairs = (line.split('=', 1) for line in path.read_text().splitlines() if '=' in line and not line.startswith('#'))
    return {key.strip(): value.strip() for key, value in pairs}


with tempfile.TemporaryDirectory(prefix='gyro-settings-') as name:
    directory = Path(name)
    (directory / 'check.c').write_text(SOURCE)
    cases = 0
    for portable in (False, True):
        if not portable and sys.platform != 'darwin':
            continue
        binary = directory / ('portable' if portable else 'native')
        subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        *(['-U__APPLE__'] if portable else []), '-I', str(ROOT), str(directory / 'check.c'),
                        str(ROOT / 'runtime/settings.c'), '-lm', '-o', str(binary)], check=True)
        ini = directory / 'settings.ini'
        env = {key: value for key, value in os.environ.items() if not key.startswith('LARECOMP_')}
        env['LARECOMP_SETTINGS'] = str(ini)
        def run(arguments=()):
            result = subprocess.run([str(binary), *arguments], env=env, capture_output=True, text=True, check=True)
            values = list(map(float, result.stdout.split()))
            assert len(values) == 7 and all(math.isfinite(value) for value in values), values
            return values
        # An old or absent settings file must leave gyro disabled.
        ini.unlink(missing_ok=True)
        assert run() == [0, 1, 0, 0, 1, 1, 0]
        cases += 1
        for text, mode, speed, x, y in CASES:
            ini.write_text(LEGACY + text)
            expected = [mode, speed, x, y, 1, 2.25, 1]
            first = run()
            assert all(abs(a - b) < 1e-6 for a, b in zip(first, expected)), (text, first, expected)
            saved = read_saved(ini)
            assert saved['gyro_camera'] == ('off', 'hold', 'always')[mode]
            assert math.isfinite(float(saved['gyro_speed'])) and .1 <= float(saved['gyro_speed']) <= 5
            assert saved['invert_gyro_x'] == str(x) and saved['invert_gyro_y'] == str(y)
            second = run()
            assert all(abs(a - b) < 1e-6 for a, b in zip(second, expected)), (text, second, expected)
            cases += 2
        # Saving an invalid in-memory mode/speed also writes a finite, bounded setting.
        for mode, raw_speed, saved_speed in [('99', 'nan', 1), ('-1', 'inf', 1), ('1', '-3', .1), ('2', '20', 5)]:
            ini.write_text(LEGACY)
            run((mode, raw_speed))
            saved = read_saved(ini)
            expected_mode = ('off', 'hold', 'always')[int(mode)] if mode in ('1', '2') else 'off'
            assert saved['gyro_camera'] == expected_mode
            assert abs(float(saved['gyro_speed']) - saved_speed) < 1e-6
            assert run()[0] == (int(mode) if mode in ('1', '2') else 0)
            cases += 2
    print(f'PASS: {cases} native/portable gyro defaults, mode/range/nonfinite checks, legacy mouse settings and round trips')
