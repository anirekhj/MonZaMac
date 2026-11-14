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
#include <string.h>
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
    printf("%d %.9g %d %d %d %.9g %d %d\n", g_settings.gyro_camera, g_settings.gyro_speed,
           g_settings.invert_gyro_x, g_settings.invert_gyro_y, g_settings.mouse_camera,
           g_settings.mouse_speed, g_settings.invert_mouse_x, g_settings.gyro_camera_mode);
    if (argc == 2) {
        if (!strcmp(argv[1], "disable")) settings_set_gyro_camera_enabled(false);
        else if (!strcmp(argv[1], "enable")) settings_set_gyro_camera_enabled(true);
        else if (!strcmp(argv[1], "hold")) settings_set_gyro_camera_mode(GYRO_CAM_HOLD);
        else if (!strcmp(argv[1], "always")) settings_set_gyro_camera_mode(GYRO_CAM_ALWAYS);
        else return 2;
    }
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
    executions = [0]
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
            executions[0] += 1
            values = list(map(float, result.stdout.split()))
            assert len(values) == 8 and all(math.isfinite(value) for value in values), values
            return values
        # An old or absent settings file must leave gyro disabled.
        ini.unlink(missing_ok=True)
        assert run() == [0, 1, 0, 0, 1, 1, 0, 2]
        for text, mode, speed, x, y in CASES:
            ini.write_text(LEGACY + text)
            expected = [mode, speed, x, y, 1, 2.25, 1, mode if mode else 2]
            first = run()
            assert all(abs(a - b) < 1e-6 for a, b in zip(first, expected)), (text, first, expected)
            saved = read_saved(ini)
            assert saved['gyro_camera'] == ('off', 'hold', 'always')[mode]
            assert saved['gyro_camera_mode'] == ('hold' if mode == 1 else 'always')
            assert math.isfinite(float(saved['gyro_speed'])) and .1 <= float(saved['gyro_speed']) <= 5
            assert saved['invert_gyro_x'] == str(x) and saved['invert_gyro_y'] == str(y)
            second = run()
            assert all(abs(a - b) < 1e-6 for a, b in zip(second, expected)), (text, second, expected)
        # Saving an invalid in-memory mode/speed also writes a finite, bounded setting.
        for mode, raw_speed, saved_speed in [('99', 'nan', 1), ('-1', 'inf', 1), ('1', '-3', .1), ('2', '20', 5)]:
            ini.write_text(LEGACY)
            run((mode, raw_speed))
            saved = read_saved(ini)
            expected_mode = ('off', 'hold', 'always')[int(mode)] if mode in ('1', '2') else 'off'
            assert saved['gyro_camera'] == expected_mode
            assert abs(float(saved['gyro_speed']) - saved_speed) < 1e-6
            assert run()[0] == (int(mode) if mode in ('1', '2') else 0)
        # New remembered-mode keys are order independent; active legacy values win conflicts.
        for text, mode, remembered in [
            ('gyro_camera = off\ngyro_camera_mode = hold\n', 0, 1),
            ('gyro_camera_mode = hold\ngyro_camera = off\n', 0, 1),
            ('gyro_camera = off\ngyro_camera_mode = always\n', 0, 2),
            ('gyro_camera = off\ngyro_camera_mode = invalid\n', 0, 2),
            ('gyro_camera = hold\ngyro_camera_mode = always\n', 1, 1),
            ('gyro_camera_mode = hold\ngyro_camera = always\n', 2, 2),
        ]:
            ini.write_text(LEGACY + text)
            first = run()
            assert (first[0], first[-1]) == (mode, remembered), (text, first)
            assert (run()[0], read_saved(ini)['gyro_camera_mode']) == (mode, 'hold' if remembered == 1 else 'always')
        # Actual UI helpers disable, persist, restart and restore both activation modes.
        for name, mode in [('always', 2), ('hold', 1)]:
            ini.write_text(LEGACY + f'gyro_camera = {name}\ngyro_speed = 1.375\ninvert_gyro_x = 1\n')
            run(('disable',))
            saved = read_saved(ini)
            assert saved['gyro_camera'] == 'off' and saved['gyro_camera_mode'] == name
            restarted = run()
            assert restarted[:4] == [0, 1.375, 1, 0] and restarted[-1] == mode
            run(('enable',))
            restarted = run()
            assert restarted[:4] == [mode, 1.375, 1, 0] and restarted[-1] == mode
        # Editing activation while disabled must keep the camera disabled until enabled.
        ini.write_text(LEGACY + 'gyro_camera = off\ngyro_camera_mode = always\n')
        run(('hold',))
        state = run()
        assert state[0] == 0 and state[-1] == 1
        run(('always',))
        state = run()
        assert state[0] == 0 and state[-1] == 2
        run(('enable',))
        state = run()
        assert state[0] == state[-1] == 2
    print(f'PASS: {executions[0]} native/portable gyro settings, legacy migration, enable/disable restoration and round trips')
