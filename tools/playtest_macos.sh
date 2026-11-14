#!/bin/bash
# Local playtest only. This script contains no game files and does not replace the installed app.
set -Eeuo pipefail
umask 077

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd -P)
REPO=$(cd -- "$SCRIPT_DIR/.." && pwd -P)
BUILD="$REPO/build/native-enhancements"
SOURCE="$HOME/Library/Application Support/com.monzamac.app"
PROFILE="$REPO/build/playtest-profile"
NO_SHADER_WARM=0
RESET_PIPELINE_CACHE=0
STAGE=""
WORK=""
LOCK=""
HAVE_LOCK=0

usage() {
    cat <<'HELP'
Usage: tools/playtest_macos.sh [options]
  --build-dir DIR       Directory with matching monzamac, lasgame.dylib and .id
                        (default: repo/build/native-enhancements)
  --source-profile DIR  Existing app profile containing extracted/ and saves
                        (default: ~/Library/Application Support/com.monzamac.app)
  --profile DIR         Separate persistent test profile
                        (default: repo/build/playtest-profile)
  --reset-pipeline-cache
                        Back up the test profile's active native archive this run
  --no-shader-warm      Skip existing startup shader-cache warmup for this run
  -h, --help            Show this help without copying or launching anything
Save and quit every MonZaMac instance before launching. First use copies saves,
settings, mods, cache and fonts; the imported pipeline archive is kept as an
inactive backup. Later runs keep the test profile and do not re-import anything.
HELP
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir|--source-profile|--profile)
            [[ $# -ge 2 && -n "$2" ]] || { printf 'Missing value for %s\n' "$1" >&2; exit 2; }
            case "$1" in
                --build-dir) BUILD=$2;;
                --source-profile) SOURCE=$2;;
                --profile) PROFILE=$2;;
            esac
            shift 2;;
        --reset-pipeline-cache) RESET_PIPELINE_CACHE=1; shift;;
        --no-shader-warm) NO_SHADER_WARM=1; shift;;
        -h|--help) usage; exit 0;;
        *) printf 'Unknown option: %s\n' "$1" >&2; usage >&2; exit 2;;
    esac
done

pause_terminal() {
    if [[ -t 0 ]]; then
        printf '\nPress Return to close this launcher. '
        IFS= read -r _ || true
    fi
}
cleanup() {
    if [[ -n "$STAGE" && "$STAGE" == "$WORK"/.monzamac-playtest-stage.* ]]; then
        /bin/rm -rf -- "$STAGE"
    fi
    if [[ "$HAVE_LOCK" == 1 ]]; then
        /bin/rm -f -- "$LOCK/pid"
        /bin/rmdir -- "$LOCK" 2>/dev/null || true
    fi
}
fail() {
    printf '\nERROR: %s\n' "$*" >&2
    pause_terminal
    exit 1
}
unexpected_error() {
    local status=$?
    trap - ERR
    printf '\nERROR: launcher stopped (status %s, line %s).\n' "$status" "$1" >&2
    printf 'Your installed app profile has not been changed.\n' >&2
    pause_terminal
    exit "$status"
}
trap cleanup EXIT
trap 'unexpected_error "$LINENO"' ERR
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

require_private_tree() {
    local linked
    linked=$(/usr/bin/find "$1" \( -type l -o \( -type f -links +1 \) \) -print -quit) ||
        fail "Cannot verify the copied profile: $1"
    [[ -z "$linked" ]] || fail "The copied profile contains a symbolic or hard link: $linked. Replace it with a private copy."
}

require_stopped() {
    local processes running
    processes=$(/bin/ps -axo pid=,comm=) || fail 'Cannot inspect running apps. No copying or game launch will proceed.'
    running=$(printf '%s\n' "$processes" | /usr/bin/awk '
        { executable=$0; sub(/^[[:space:]]*[0-9]+[[:space:]]+/, "", executable);
          count=split(executable, parts, "/"); name=tolower(parts[count]);
          if (name == "monzamac" || tolower(executable) ~ /monzamac[.]app\/contents\/macos\//) print; }')
    if [[ -n "$running" ]]; then
        printf '\nMonZaMac is already running:\n%s\n' "$running" >&2
        fail 'Save your game, fully quit MonZaMac (including any test copy), then run this launcher again.'
    fi
}

[[ "$(/usr/bin/uname -s)" == Darwin ]] || fail 'This launcher requires macOS.'
[[ -x /usr/bin/python3 ]] || fail 'macOS Python 3 is required for safe profile staging.'
[[ -d "$BUILD" ]] || fail "Build directory not found: $BUILD"
[[ -d "$SOURCE" ]] || fail "Source app profile not found: $SOURCE"
BUILD=$(cd -- "$BUILD" && pwd -P)
SOURCE=$(cd -- "$SOURCE" && pwd -P)
# Resolve the parent even for a new path; keep the final component so links are refused.
PROFILE=$(/usr/bin/python3 - "$PROFILE" <<'PY_PATH'
import os
import sys
p = os.path.abspath(sys.argv[1])
print(os.path.join(os.path.realpath(os.path.dirname(p)), os.path.basename(p)))
PY_PATH
)
[[ ! -L "$PROFILE" ]] || fail 'The test profile must not be a symbolic link.'
if [[ "$PROFILE" == "$SOURCE" || "$PROFILE" == "$SOURCE/"* || "$SOURCE" == "$PROFILE/"* ]]; then
    fail 'Source and test profiles must be separate, with neither inside the other.'
fi
WORK=$(dirname -- "$PROFILE")
LOCK="$PROFILE.launch-lock"
BINARY="$BUILD/monzamac"
GAME_LIB="$BUILD/lasgame.dylib"
ASSETS="$SOURCE/extracted"
EXTRA_ENV=()
if [[ "$NO_SHADER_WARM" == 1 ]]; then EXTRA_ENV+=(LARECOMP_NO_SHADER_WARM=1); fi

printf 'MonZaMac — isolated local playtest\n\n'
printf 'Before continuing: SAVE in your installed app, then QUIT it completely.\n'
printf 'On the first successful launch only, your saves, settings, mods and cache are copied.\n'
printf 'Later launches keep the test profile and its progress; they do not re-copy your saves.\n'
printf 'Source profile: %s\nTest profile:   %s\n\n' "$SOURCE" "$PROFILE"

[[ -x "$BINARY" ]] || fail "Test runtime is missing or not executable: $BINARY"
[[ -f "$GAME_LIB" && -s "$GAME_LIB" && -f "$GAME_LIB.id" && -s "$GAME_LIB.id" ]] ||
    fail "The compatible local game library and its ID file are not ready: $GAME_LIB"
library_bytes=$(/usr/bin/stat -f '%z' "$GAME_LIB")
[[ "$library_bytes" =~ ^[0-9]+$ && "$library_bytes" -gt 1048576 ]] ||
    fail 'The compatible game library is still a small placeholder. Wait for the local build to finish, then retry.'
[[ -d "$ASSETS" && -f "$ASSETS/exefs/main" ]] || fail "Existing game files were not found at: $ASSETS"
require_stopped
/bin/mkdir -p -- "$WORK"
[[ ! -L "$WORK" ]] || fail "The work folder must be a real directory: $WORK"
if ! /bin/mkdir -- "$LOCK" 2>/dev/null; then
    if [[ ! -L "$LOCK" && -f "$LOCK/pid" && ! -L "$LOCK/pid" ]]; then
        owner=$(<"$LOCK/pid")
        if [[ "$owner" =~ ^[0-9]+$ ]] && ! kill -0 "$owner" 2>/dev/null; then
            /bin/rm -f -- "$LOCK/pid"
            /bin/rmdir -- "$LOCK" 2>/dev/null || fail "Another launcher owns: $LOCK"
            /bin/mkdir -- "$LOCK" || fail 'Another test launcher started. Close it and try again.'
        else
            fail 'Another test launcher is open. Close it before starting a second copy.'
        fi
    else
        fail "A launcher lock already exists: $LOCK. If no launcher is open, remove that empty lock folder and retry."
    fi
fi
HAVE_LOCK=1
printf '%s\n' "$$" > "$LOCK/pid"
require_stopped

if [[ -e "$PROFILE" || -L "$PROFILE" ]]; then
    [[ -d "$PROFILE" && ! -L "$PROFILE" && -f "$PROFILE/.isolated-playtest-profile" ]] ||
        fail "An existing folder is not a recognized test profile; it will not be overwritten: $PROFILE"
    printf 'Using your existing test profile; no files are re-copied.\n'
else
    printf 'Preparing private copies. A large shader cache or mods folder may take a moment...\n'
    STAGE=$(/usr/bin/mktemp -d "$WORK/.monzamac-playtest-stage.XXXXXX")
    for item in save saves larecomp.ini mods cache fonts; do
        if [[ -e "$SOURCE/$item" || -L "$SOURCE/$item" ]]; then
            printf '  Copying %s\n' "$item"
            # -L dereferences links; ordinary copies never share a hard-linked inode.
            /bin/cp -pRL -- "$SOURCE/$item" "$STAGE/$item" || fail "Could not copy $item. The incomplete staging copy will be removed."
        fi
    done
    if [[ ! -d "$STAGE/save" && ! -d "$STAGE/saves" ]]; then
        fail 'No save folders were found. The test profile was not created.'
    fi
    require_private_tree "$STAGE"
    # Native archives can take minutes to parse and may belong to another runtime/device.
    # Retain the imported bytes without making that archive active in this build.
    for archive in pipelines.bin pipelines.saving pipelines.crashed; do
        if [[ -e "$STAGE/cache/$archive" ]]; then
            /bin/mkdir -p -- "$STAGE/cache-backups/imported"
            /bin/mv -- "$STAGE/cache/$archive" "$STAGE/cache-backups/imported/$archive"
            printf '  Kept imported %s inactive in cache-backups/imported.\n' "$archive"
        fi
    done
    /bin/mkdir -p -- "$STAGE/logs"
    printf 'Private first-run copy made %s\n' "$(/bin/date '+%Y-%m-%d %H:%M:%S %Z')" > "$STAGE/.isolated-playtest-profile"
    /bin/chmod 700 "$STAGE"
    require_stopped
    # Darwin's exclusive rename publishes the complete directory atomically and
    # refuses an existing destination, even if another process creates it now.
    /usr/bin/python3 - "$STAGE" "$PROFILE" <<'PY'
import ctypes
import os
import sys
libc = ctypes.CDLL(None, use_errno=True)
rename = libc.renamex_np
rename.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
rename.restype = ctypes.c_int
if rename(os.fsencode(sys.argv[1]), os.fsencode(sys.argv[2]), 0x00000004):  # RENAME_EXCL
    error = ctypes.get_errno()
    raise SystemExit('Cannot publish the test profile without replacing anything: ' + os.strerror(error))
PY
    STAGE=""
    printf 'Test profile created. Your installed profile remains separate.\n'
fi

# Reject later links too, so writes cannot be redirected into the installed profile.
require_private_tree "$PROFILE"
if [[ "$RESET_PIPELINE_CACHE" == 1 ]]; then
    require_stopped
    BACKUP=""
    for archive in pipelines.bin pipelines.saving pipelines.crashed; do
        if [[ -e "$PROFILE/cache/$archive" ]]; then
            if [[ -z "$BACKUP" ]]; then
                BACKUP="$PROFILE/cache-backups/reset-$(/bin/date '+%Y%m%d-%H%M%S')-$$"
                /bin/mkdir -p -- "$PROFILE/cache-backups"
                /bin/mkdir -- "$BACKUP"
            fi
            /bin/mv -- "$PROFILE/cache/$archive" "$BACKUP/$archive"
        fi
    done
    if [[ -n "$BACKUP" ]]; then
        printf 'Preserved the test native archive/markers at: %s\n' "$BACKUP"
    else
        printf 'No active test pipeline archive or markers to reset.\n'
    fi
fi
/bin/mkdir -p -- "$PROFILE/logs"
LOG="$PROFILE/logs/play-$(/bin/date '+%Y%m%d-%H%M%S')-$$.log"
DEVLOG="${LOG%.log}-framegen.txt"
(set -o noclobber; : > "$LOG") || fail "Cannot create a new log without replacing one: $LOG"
(set -o noclobber; : > "$DEVLOG") || fail "Cannot create a new diagnostic log without replacing one: $DEVLOG"
require_stopped

printf '\nChoose your copied save on the start screen, then Continue (offline).\n'
if [[ "$NO_SHADER_WARM" == 1 ]]; then printf 'Startup shader warmup skipped for this run; first-use shaders can hitch.\n'; fi
printf 'F1: settings/menu.  F3: FPS and resolution (Fn+F1/F3 if needed).\n'
printf 'Use the same scene and settings for comparison; first-time shaders may still hitch.\n'
printf 'Save and quit the test normally when finished. Test progress stays in the test profile.\n'
printf 'Keep this Terminal window open while playing.\nLog: %s\nFrame generation and motion diagnostics: %s\n\n' "$LOG" "$DEVLOG"
cd -- "$REPO"
# A clean environment prevents inherited automation, replay, validation or library-injection hooks.
# Enable framegen and native-motion diagnostics for this playtest; each run keeps its own file.
# The reactive-mask experiment stays at its default (off); copied UI settings remain in charge.
if /usr/bin/env -i \
    HOME="$HOME" USER="$(/usr/bin/id -un)" LOGNAME="$(/usr/bin/id -un)" \
    PATH="/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin" \
    TMPDIR="${TMPDIR:-/tmp}" LANG="${LANG:-en_US.UTF-8}" \
    LARECOMP_DATA_ROOT="$PROFILE" LARECOMP_GAME_LIB="$GAME_LIB" \
    LARECOMP_DEVLOG=framegen,motion LARECOMP_DEVLOG_FILE="$DEVLOG" \
    ${EXTRA_ENV[@]+"${EXTRA_ENV[@]}"} \
    "$BINARY" "$ASSETS" >> "$LOG" 2>&1; then
    status=0
else
    status=$?
fi
if [[ "$status" != 0 ]]; then
    printf '\nThe test exited with status %s. Last log lines:\n' "$status" >&2
    /usr/bin/tail -n 35 "$LOG" >&2 || true
else
    printf '\nTest closed. Copied progress and settings are preserved.\n'
fi
printf 'Log: %s\nFrame generation and motion diagnostics: %s\n' "$LOG" "$DEVLOG"
pause_terminal
exit "$status"
