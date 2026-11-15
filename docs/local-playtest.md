# Isolated local playtest on macOS

Build a matching `monzamac`, `lasgame.dylib` and `lasgame.dylib.id` from the same
checkout first (see [Building](../CONTRIBUTING.md#building)). An installed app's
library may belong to another runtime revision. The launcher does not build or
download game code and rejects a small placeholder library.

Save your game, quit every MonZaMac instance, then run from the checkout:

```sh
tools/playtest_macos.sh --build-dir build/native-enhancements
```

The default source is `~/Library/Application Support/com.monzamac.app`. First use
makes timestamp-preserving private copies of `save/`, `saves/`, `larecomp.ini`,
`mods/`, `cache/` and optional `fonts/` in the git-ignored `build/playtest-profile`.
It dereferences copied links, verifies private files, and publishes the complete
profile atomically without replacing an existing directory. Original extracted
game assets are read from the source profile; saves and settings use the test
profile. Keep the source profile and its game files available.

The imported `cache/pipelines.bin` and any `pipelines.saving`, `pipelines.crashed`
or `pipelines.loading` markers are preserved under `cache-backups/imported/` in the new profile. They
are not made active:
a large native archive from another app revision/device can spend minutes
parsing at startup. The test build creates its own archive. Other copied shader
caches and learned recipes remain available; first-use compilation can still
hitch. Subsequent runs preserve test progress and the new archive, and never
re-import the source profile. To make a new copy, choose a new `--profile` path.

Choose the copied save at the start screen, then Continue offline. F1 opens
settings; F3 shows FPS/resolution (Fn may be needed). Compare the same scene and
settings after shaders settle. The launcher clears inherited test/Metal validation
hooks, keeps normal startup shader warmup, and enables the `framegen`, `motion` and `kernel-wait`
developer-log subjects for this playtest. It prints unique per-run play and diagnostic
log paths under the test profile's `logs/`. The diagnostic filename keeps its
`-framegen.txt` suffix and contains all three subjects, including native orientation reads and
handle allocation/close history. Invalid waits include the failing handle, caller
and recent history for that handle; this records evidence without changing wait results.
The optional history retains at most 128 allocation/close events in memory and
writes them only on an invalid wait, avoiding routine diagnostic I/O under the
kernel lock. No extra guest memory or stack slots are read. A copied setting that already enables
all developer subjects remains a copied user preference. Keep the terminal open
while playing, and quit normally when finished. Test saves are not copied back.

Options can use absolute paths or paths relative to the current directory:

```sh
tools/playtest_macos.sh --build-dir build/custom --source-profile /path/to/app-profile \
  --profile build/another-playtest
```

For an existing private profile that stalls loading its own archive, save and
quit every instance first, then use:

```sh
tools/playtest_macos.sh --reset-pipeline-cache
```

This moves only the test profile's active archive and associated saving/crash/loading
markers to a unique timestamped `cache-backups/reset-*/` directory and lets the
runtime create a fresh archive. Marker-only failures are reset too.
It does not delete archives or reset saves, mods, settings or other shader caches.
The same process guard and private-profile checks apply.

To bypass the optional native pipeline archive for one run while preserving all
existing cache files, use:

```sh
tools/playtest_macos.sh --no-pipeline-archive
```

This forwards `LARECOMP_NO_PIPELINE_ARCHIVE=1` through the clean launch environment.
Startup shader warmup and Apple's own caches remain available. It does not move
or reset the active archive; `--reset-pipeline-cache` remains a separate option.

`--no-shader-warm` explicitly skips the existing startup shader-cache warmup for
one run. It can shorten that phase but shifts compilation toward first use;
it does not skip pipeline-archive loading, shader preparation, or game loading.
The runtime default is unchanged without this flag. `--help` does not copy or
launch anything. The script requires macOS Python 3 for exclusive atomic profile
publication. It refuses a running app/test copy and never modifies installed
app files. Existing profiles containing links are refused rather than followed.

Run the launcher regression with synthetic profiles and a mock executable only:

```sh
python3 tools/test_playtest_macos.py
```

The test never launches the game, reads a real app profile, or probes a controller.
