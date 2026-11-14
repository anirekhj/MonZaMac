#!/usr/bin/env python3
"""Isolated macOS launcher regression; synthetic files and a mock binary only."""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
if __import__('sys').platform != 'darwin':
    print('SKIP: playtest launcher uses macOS utilities and exclusive rename')
    raise SystemExit(0)
launcher = (ROOT/'tools/playtest_macos.sh').read_text()
assert '/bin/ps -axo pid=,comm=' in launcher
# Only the disposable copy replaces process enumeration. Every game invocation is a fake shell script.
launcher = launcher.replace('/bin/ps -axo pid=,comm=', '"$HOME/mock-ps"')
with tempfile.TemporaryDirectory(prefix='monzamac-launcher-mock-') as temporary:
    base = Path(temporary)
    home = base/'home'
    workspace = base/'workspace'
    outputs = workspace/'tools'
    outputs.mkdir(parents=True)
    runscript = outputs/'playtest_macos.sh'
    runscript.write_text(launcher)
    runscript.chmod(0o700)
    source = home/'Library/Application Support/com.monzamac.app'
    build = workspace/'build/native-enhancements'
    build.mkdir(parents=True)
    for folder in ('save', 'saves', 'mods', 'cache', 'fonts', 'extracted/exefs'):
        (source/folder).mkdir(parents=True)
    (source/'extracted/exefs/main').write_text('fake assets only')
    (build/'lasgame.dylib').write_text('fake library only')
    (build/'lasgame.dylib.id').write_text('fake ID only')
    (source/'save/progress').write_text('original progress\n')
    (source/'saves/slots').write_text('Save 1\n')
    (source/'larecomp.ini').write_text('upscaler=metalfx\n')
    (source/'fonts/font').write_text('fake font')
    (source/'cache/pipelines.bin').write_text('original native archive')
    (source/'cache/pipelines.saving').write_text('original saving marker')
    (source/'cache/pipelines.crashed').write_text('original crashed marker')
    (source/'cache/a').write_text('fake shader data')
    os.link(source/'cache/a', source/'cache/b')
    (home/'external-mod-data').write_text('fake mod data')
    (source/'mods/private-copy').symlink_to(home/'external-mod-data')
    old = 1_700_000_000
    os.utime(source/'cache/pipelines.bin', (old, old))
    os.utime(source/'larecomp.ini', (old, old))
    ps = home/'mock-ps'
    ps.write_text('#!/bin/bash\n/bin/cat "$HOME/ps-fixture"\n')
    ps.chmod(0o700)
    (home/'ps-fixture').write_text(' 123 /sbin/launchd\n')
    binary = build/'monzamac'
    binary.write_text('''#!/bin/bash
printf 'MOCK GAME; cwd=%s; args=%s\\n' "$PWD" "$*"
/usr/bin/env | /usr/bin/sort
printf 'test-only progress\\n' >> "$LARECOMP_DATA_ROOT/save/progress"
printf 'framegen/motion mock diagnostic\\n' >> "$LARECOMP_DEVLOG_FILE"
if [[ -f "$LARECOMP_DATA_ROOT/cache/pipelines.bin" ]]; then echo ACTIVE_ARCHIVE=existing; else echo ACTIVE_ARCHIVE=fresh; fi
printf 'new test archive' > "$LARECOMP_DATA_ROOT/cache/pipelines.bin"
''')
    binary.chmod(0o700)
    env = dict(os.environ, HOME=str(home), MTL_DEBUG_LAYER='1', MTL_SHADER_VALIDATION='1',
               LARECOMP_INPUT='must be cleared', LARECOMP_PLAY_SAVE='must be cleared',
               LARECOMP_SAVE_DIR=str(source/'save'), LARECOMP_NO_SHADER_WARM='1', DYLD_INSERT_LIBRARIES='')
    def run(*args):
        return subprocess.run(['/bin/bash', str(runscript), *map(str,args)], env=env, input='', text=True, capture_output=True, timeout=30)
    source_before = {str(p.relative_to(source)): (p.read_bytes(), p.stat().st_mtime_ns)
                     for p in source.rglob('*') if p.is_file() and not p.is_symlink()}
    small = run()
    assert small.returncode == 1 and 'small placeholder' in small.stderr
    assert not (workspace/'build/playtest-profile').exists()
    with (build/'lasgame.dylib').open('r+b') as library:
        library.truncate(1048577)
    print('PASS incomplete-library refusal before profile copy')
    first = run()
    assert first.returncode == 0, (first.stdout, first.stderr)
    profile = workspace/'build/playtest-profile'
    assert (profile/'.isolated-playtest-profile').is_file()
    assert (profile/'larecomp.ini').stat().st_mtime_ns == (source/'larecomp.ini').stat().st_mtime_ns
    assert not any(p.is_symlink() for p in profile.rglob('*'))
    for p in profile.rglob('*'):
        if p.is_file(): assert p.stat().st_nlink == 1, p
    assert (profile/'mods/private-copy').read_text() == 'fake mod data'
    assert (profile/'save/progress').read_text().endswith('test-only progress\n')
    assert source_before == {str(p.relative_to(source)): (p.read_bytes(), p.stat().st_mtime_ns)
                             for p in source.rglob('*') if p.is_file() and not p.is_symlink()}
    logs = list((profile/'logs').glob('play-*.log'))
    assert len(logs) == 1
    log = logs[0].read_text()
    assert f'args={source.resolve()}/extracted\n' in log, log
    assert f'cwd={workspace.resolve()};' in log or f'cwd={workspace};' in log, log
    assert '--play' not in log
    assert 'ACTIVE_ARCHIVE=fresh' in log
    for marker in ('pipelines.saving','pipelines.crashed'):
        assert not (profile/'cache'/marker).exists()
        assert (profile/'cache-backups/imported'/marker).read_bytes() == (source/'cache'/marker).read_bytes()
    assert 'LARECOMP_NO_SHADER_WARM=' not in log
    assert (profile/'cache-backups/imported/pipelines.bin').read_text() == 'original native archive'
    assert (profile/'cache-backups/imported/pipelines.bin').stat().st_mtime_ns == (source/'cache/pipelines.bin').stat().st_mtime_ns
    assert (profile/'cache/pipelines.bin').read_text() == 'new test archive'
    assert f'LARECOMP_DATA_ROOT={profile.resolve()}\n' in log
    assert f'LARECOMP_GAME_LIB={build.resolve()}/lasgame.dylib\n' in log
    diagnostics = list((profile/'logs').glob('play-*-framegen.txt'))
    assert len(diagnostics) == 1
    assert diagnostics[0].read_text() == 'framegen/motion mock diagnostic\n'
    assert 'LARECOMP_DEVLOG=framegen,motion\n' in log
    assert f'LARECOMP_DEVLOG_FILE={diagnostics[0].resolve()}\n' in log
    assert f'Frame generation and motion diagnostics: {diagnostics[0].resolve()}' in first.stdout
    assert 'MTL_' not in log and 'LARECOMP_INPUT=' not in log and 'LARECOMP_SAVE_DIR=' not in log
    assert 'LARECOMP_PLAY_SAVE=' not in log and 'DYLD_INSERT_LIBRARIES=' not in log
    print('PASS first-run staging, private dereferenced copies, timestamps, source unchanged, clean environment, assets/cwd and logs')
    (source/'saves/slots').write_text('new installed state; must not be re-copied')
    second = run('--no-shader-warm')
    assert second.returncode == 0, (second.stdout, second.stderr)
    assert 'no files are re-copied' in second.stdout
    assert (profile/'saves/slots').read_text() == 'Save 1\n'
    assert len(list((profile/'logs').glob('play-*.log'))) == 2
    assert len(list((profile/'logs').glob('play-*-framegen.txt'))) == 2
    second_logs = [p for p in (profile/'logs').glob('play-*.log') if p != logs[0]]
    assert len(second_logs) == 1
    assert 'LARECOMP_NO_SHADER_WARM=1\n' in second_logs[0].read_text()
    assert 'ACTIVE_ARCHIVE=existing' in second_logs[0].read_text()
    assert (profile/'cache-backups/imported/pipelines.bin').read_text() == 'original native archive'
    print('PASS subsequent-run profile preservation, new archive reuse, warmup opt-in and unique retained logs')
    separate = workspace/'build/custom-playtest'
    custom = run('--build-dir',build,'--source-profile',source,'--profile',separate)
    assert custom.returncode == 0, (custom.stdout,custom.stderr)
    assert (separate/'save/progress').is_file()
    nested = run('--profile',source/'must-not-create')
    assert nested.returncode == 1 and not (source/'must-not-create').exists()
    assert run('--help').returncode == 0
    assert run('--unknown').returncode == 2
    assert run('--build-dir').returncode == 2
    for marker in ('pipelines.saving','pipelines.crashed'):
        (profile/'cache'/marker).write_text('test marker')
    reset = run('--reset-pipeline-cache')
    assert reset.returncode == 0, (reset.stdout,reset.stderr)
    backups = list((profile/'cache-backups').glob('reset-*/pipelines.bin'))
    assert len(backups) == 1 and backups[0].read_text() == 'new test archive'
    for marker in ('pipelines.saving','pipelines.crashed'):
        assert not (profile/'cache'/marker).exists()
        assert (backups[0].parent/marker).read_text() == 'test marker'
    assert (source/'cache/pipelines.bin').read_text() == 'original native archive'
    latest = max((profile/'logs').glob('play-*.log'),key=lambda p:p.stat().st_mtime_ns)
    assert 'ACTIVE_ARCHIVE=fresh' in latest.read_text()
    (profile/'cache/pipelines.bin').unlink()
    (profile/'cache/pipelines.crashed').write_text('marker only')
    marker_reset = run('--reset-pipeline-cache')
    assert marker_reset.returncode == 0, (marker_reset.stdout,marker_reset.stderr)
    marker_backups = list((profile/'cache-backups').glob('reset-*/pipelines.crashed'))
    assert len(marker_backups) == 2
    assert any(p.read_text() == 'marker only' for p in marker_backups)
    log_count_before_busy = len(list((profile/'logs').glob('play-*.log')))
    print('PASS explicit test-archive/marker-only reset preserves backup/source and CLI paths/help/errors/overlap refusal')
    (home/'ps-fixture').write_text(' 333 /Applications/MonZaMac.app/Contents/MacOS/monzamac\n')
    busy = run()
    assert busy.returncode == 1 and 'already running' in busy.stderr
    assert len(list((profile/'logs').glob('play-*.log'))) == log_count_before_busy
    print('PASS running-app refusal before copy or launch')
    (home/'ps-fixture').write_text(' 334 '+str(build/'monzamac')+'\n')
    assert run().returncode == 1
    print('PASS running-test refusal')
    (home/'ps-fixture').write_text(' 123 /sbin/launchd\n')
    (profile/'bad-link').symlink_to(source/'save')
    linked = run()
    assert linked.returncode == 1 and 'symbolic or hard link' in linked.stderr
    (profile/'bad-link').unlink()
    print('PASS existing-profile link refusal')
    binary.write_text('#!/bin/bash\necho intentional-mock-failure >&2\nexit 7\n')
    binary.chmod(0o700)
    failed = run()
    assert failed.returncode == 7 and 'intentional-mock-failure' in failed.stderr
    assert 'launcher stopped' not in failed.stderr
    assert not (workspace/'build/playtest-profile.launch-lock').exists()
    print('PASS child-error status/log visibility and lock cleanup')
    shutil.rmtree(profile)
    profile.mkdir()
    (profile/'keep').write_text('do not overwrite')
    refused = run()
    assert refused.returncode == 1 and 'will not be overwritten' in refused.stderr
    assert sorted(p.name for p in profile.iterdir()) == ['keep']
    print('PASS unrecognized existing-profile refusal')
print('PASS all portable-launcher disposable mock checks; no real saves copied and no game launched')
