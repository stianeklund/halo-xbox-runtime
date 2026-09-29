#!/usr/bin/env bash
# Configure (if needed) and build the standalone XBE.
#
#   ./build.sh [seed|standalone|nogpu] [--fresh] [--inventory] [--deploy]
#
#   seed        (default) seeded bring-up build -> build-seed/default.xbe
#   standalone  no seeded data (does not boot yet) -> build/default.xbe
#   nogpu       seeded, HALO_STANDALONE_GPU_STATE=OFF -> build-nogpu/default.xbe
#   --fresh     reconfigure from scratch (cmake --fresh)
#   --inventory also write <build>/deps_inventory.json
#   --deploy    upload the XBE to XBOX_HOST as XBOX_TITLE over XBDM
#
# HALO_ROOT picks the halo checkout or worktree to build (default ../halo),
# e.g. HALO_ROOT=/mnt/g/dev/halo-wt ./build.sh.  Switching it reuses the same
# build directory and recompiles the game sources from the new tree.
#
# Overridable environment: NXDK_DIR, HALO_ROOT, HALO_SEED_XBE, PYTHON, XBOX_HOST, XBOX_TITLE, JOBS.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export NXDK_DIR="${NXDK_DIR:-/mnt/g/dev/nxdk}"
export HALO_ROOT="${HALO_ROOT:-$(cd "$here/.." && pwd)/halo}"
PYTHON="${PYTHON:-/usr/bin/python3}"
XBOX_HOST="${XBOX_HOST:-127.0.0.1}"
XBOX_TITLE="${XBOX_TITLE:-E:\\GAMES\\halo-patched\\standalone_seed.xbe}"
JOBS="${JOBS:-$(nproc)}"

variant=seed fresh=() inventory=0 deploy=0
for arg in "$@"; do
	case "$arg" in
		seed|standalone|nogpu) variant="$arg" ;;
		--fresh) fresh=(--fresh) ;;
		--inventory) inventory=1 ;;
		--deploy) deploy=1 ;;
		-h|--help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "unknown argument: $arg" >&2; exit 2 ;;
	esac
done

# halo-patched/ is gitignored, so a fresh worktree lacks it: fall back to the
# main checkout's copy (the seed/layout image is the same for every tree).
seed_xbe="${HALO_SEED_XBE:-$HALO_ROOT/halo-patched/cachebeta.xbe}"
if [ ! -f "$seed_xbe" ]; then
	main_tree="$(dirname "$(git -C "$HALO_ROOT" rev-parse --path-format=absolute --git-common-dir)")"
	seed_xbe="$main_tree/halo-patched/cachebeta.xbe"
fi
[ -f "$seed_xbe" ] || { echo "no cachebeta.xbe; set HALO_SEED_XBE" >&2; exit 1; }
case "$variant" in
	seed)       build="$here/build-seed";  opts=(-DHALO_STANDALONE_SEED_XBE="$seed_xbe" -DHALO_STANDALONE_GPU_STATE=ON) ;;
	nogpu)      build="$here/build-nogpu"; opts=(-DHALO_STANDALONE_SEED_XBE="$seed_xbe" -DHALO_STANDALONE_GPU_STATE=OFF) ;;
	standalone) build="$here/build";       opts=(-DHALO_STANDALONE_SEED_XBE= -DHALO_STANDALONE_GPU_STATE=ON) ;;
esac

echo "variant=$variant  NXDK_DIR=$NXDK_DIR  HALO_ROOT=$HALO_ROOT  seed=$seed_xbe"
cmake -S "$here" -B "$build" "${fresh[@]}" \
	-DCMAKE_TOOLCHAIN_FILE="$here/toolchains/llvm.cmake" \
	-DPython3_EXECUTABLE="$PYTHON" \
	-DHALO_ROOT="$HALO_ROOT" \
	-DHALO_STANDALONE_LAYOUT_XBE="$seed_xbe" \
	"${opts[@]}"
cmake --build "$build" -j "$JOBS"
[ "$inventory" = 1 ] && cmake --build "$build" --target standalone_inventory
echo "built $build/default.xbe"

if [ "$deploy" = 1 ]; then
	HALO_WINDOWS_REEXEC=1 "$PYTHON" "$HALO_ROOT/tools/xbox/xbdm_rdcp.py" --host "$XBOX_HOST" \
		--timeout 60 --sendfile "$build/default.xbe" "$XBOX_TITLE"
fi
