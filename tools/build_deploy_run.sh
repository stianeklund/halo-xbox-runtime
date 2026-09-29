#!/usr/bin/env bash
# Build the standalone XBE, upload it over XBDM and launch it — the standalone
# counterpart of halo-clean-main/tools/xbox/build_deploy_run.sh.
#
#   tools/build_deploy_run.sh [--xbox HOST] [--native-xbdm] [--watch SECONDS]
#                             [--no-launch] [-- BUILD_ARGS...]
#
#   --xbox HOST      XBDM host (default: $XBOX_HOST or 127.0.0.1)
#   --native-xbdm    bridged xemu: keep XBDM traffic native (HALO_NATIVE_XBDM=1)
#   --watch SECONDS  launch through xbdm_boot_watch.py and stream debug output
#   --no-launch      upload only
#   BUILD_ARGS       passed to build.sh (seed|standalone|nogpu, --fresh,
#                    --inventory); -q/--quiet hides build output unless it fails
#
# Overridable environment: XBOX_HOST, XBOX_TITLE, PYTHON, plus build.sh's.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export HALO_ROOT="${HALO_ROOT:-$(cd "$here/.." && pwd)/halo-clean-main}"
PYTHON="${PYTHON:-/usr/bin/python3}"
xbox_host="${XBOX_HOST:-127.0.0.1}"
xbox_title="${XBOX_TITLE:-E:\\GAMES\\halo-patched\\standalone_seed.xbe}"
build_args=()
quiet=0 launch=1 watch=0 variant=seed

log() {
  printf '[build_deploy_run][%s] %s\n' "$(date '+%H:%M:%S')" "$*"
}

while (($#)); do
  case "$1" in
    --xbox)
      if (($# < 2)); then
        printf 'error: --xbox requires a host or IP\n' >&2
        exit 2
      fi
      xbox_host="$2"
      shift 2
      ;;
    --xemu-bridged|--native-xbdm)
      export HALO_NATIVE_XBDM=1
      shift
      ;;
    --watch)
      if (($# < 2)); then
        printf 'error: --watch requires a number of seconds\n' >&2
        exit 2
      fi
      watch="$2"
      shift 2
      ;;
    --no-launch)
      launch=0
      shift
      ;;
    -h|--help)
      sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    --)
      shift
      build_args+=("$@")
      break
      ;;
    *)
      build_args+=("$1")
      shift
      ;;
  esac
done

# -q/--quiet is ours; everything else goes to build.sh.
filtered=()
for arg in ${build_args[@]+"${build_args[@]}"}; do
  case "$arg" in
    -q|--quiet) quiet=1 ;;
    seed|standalone|nogpu) variant="$arg"; filtered+=("$arg") ;;
    *) filtered+=("$arg") ;;
  esac
done
case "$variant" in
  seed) xbe="$here/build-seed/default.xbe" ;;
  nogpu) xbe="$here/build-nogpu/default.xbe" ;;
  standalone) xbe="$here/build/default.xbe" ;;
esac

# Every XBDM call from WSL must re-exec through Windows (or run native when
# HALO_NATIVE_XBDM=1 for a bridged guest).
export HALO_WINDOWS_REEXEC=1
rdcp() {
  "$PYTHON" "$HALO_ROOT/tools/xbox/xbdm_rdcp.py" --host "$xbox_host" "$@"
}

log "starting (xbox host: ${xbox_host}, variant: ${variant})"
log "build args: ${filtered[*]:-<none>}"

log "running build"
if ((quiet)); then
  build_log="$(mktemp /tmp/halo-nxdk-build.XXXXXX)"
  if ! "$here/build.sh" ${filtered[@]+"${filtered[@]}"} >"$build_log" 2>&1; then
    grep -E 'error|Error|ERROR|undefined' "$build_log" | head -40 || tail -40 "$build_log"
    log "build failed (full log: ${build_log}); deployment skipped"
    exit 1
  fi
  grep -iE 'warning:' "$build_log" | grep -v 'CMake Warning' | head -20 || true
  rm -f "$build_log"
else
  if ! "$here/build.sh" ${filtered[@]+"${filtered[@]}"}; then
    log "build failed; deployment skipped"
    exit 1
  fi
fi

log "uploading ${xbe} -> ${xbox_title}"
if ! rdcp --timeout 60 --sendfile "$xbe" "$xbox_title"; then
  log "upload failed"
  exit 1
fi

# The stack walker reads d:\cachebeta.map, i.e. next to the XBE.
map="$(dirname "$xbe")/cachebeta.map"
map_title="${xbox_title%\\*}\\cachebeta.map"
if [[ -f "$map" ]]; then
  log "uploading ${map} -> ${map_title}"
  rdcp --timeout 60 --sendfile "$map" "$map_title" || log "map upload failed (stack traces stay unsymbolized)"
fi

if ((!launch)); then
  log "done (not launched)"
  exit 0
fi

if [[ "$watch" != 0 ]]; then
  log "launching and watching for ${watch}s"
  "$PYTHON" "$here/tools/xbdm_boot_watch.py" --host "$xbox_host" \
    --title "$xbox_title" --seconds "$watch"
  log "done"
  exit 0
fi

log "launching ${xbox_title}"
if ! rdcp "magicboot title=${xbox_title}"; then
  log "launch failed"
  exit 1
fi
log "done (use --watch SECONDS to stream the title's debug output)"
