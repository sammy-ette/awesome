#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the Skia dependency used by the top-level Makefile.  All downloads and
# generated files live under AWESOME_SKIA_BUILD_ROOT (normally build/_deps).

set -Eeuo pipefail

usage() {
    cat <<'USAGE'
Usage: tools/build-skia-vulkan.sh [--update] [--clean]

Environment overrides:
  AWESOME_SKIA_BUILD_ROOT  Dependency directory (default: build/_deps)
  SKIA_REF                 Skia branch, tag, or commit (default: main)
  JOBS                     Parallel build jobs
USAGE
}

update_skia=0
clean=0
while (($#)); do
    case "$1" in
        --update) update_skia=1 ;;
        --clean) clean=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_root="${AWESOME_SKIA_BUILD_ROOT:-$repo_root/build/_deps}"
depot_tools_dir="$build_root/depot_tools"
skia_dir="$build_root/skia"
skia_out_rel="out/awesome-vulkan"
skia_out_dir="$skia_dir/$skia_out_rel"
skia_ref="${SKIA_REF:-main}"

if [[ "$clean" == 1 ]]; then
    rm -rf -- "$build_root"
    echo "Removed $build_root"
    exit 0
fi

for command in git python3; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "Missing required command: $command" >&2
        exit 1
    }
done

if command -v nproc >/dev/null 2>&1; then
    default_jobs="$(nproc)"
else
    default_jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '4')"
fi
jobs="${JOBS:-$default_jobs}"

mkdir -p "$build_root"

if [[ ! -d "$depot_tools_dir/.git" ]]; then
    echo "Cloning depot_tools..."
    git clone --depth=1 https://chromium.googlesource.com/chromium/tools/depot_tools.git "$depot_tools_dir"
elif [[ "$update_skia" == 1 ]]; then
    git -C "$depot_tools_dir" pull --ff-only
fi
export PATH="$depot_tools_dir:$PATH"

if [[ ! -d "$skia_dir/.git" ]]; then
    echo "Cloning Skia..."
    git clone --filter=blob:none --no-checkout https://skia.googlesource.com/skia.git "$skia_dir"
    update_skia=1
fi
if [[ "$update_skia" == 1 ]] || ! git -C "$skia_dir" rev-parse --verify HEAD >/dev/null 2>&1; then
    echo "Fetching Skia ref: $skia_ref"
    git -C "$skia_dir" fetch --depth=1 origin "$skia_ref"
    git -C "$skia_dir" checkout --detach FETCH_HEAD
fi

(
    cd "$skia_dir"
    python3 tools/git-sync-deps
    if ! command -v ninja >/dev/null 2>&1; then
        bin/fetch-ninja
        export PATH="$skia_dir/bin:$PATH"
    fi
    bin/gn gen "$skia_out_rel" --args="is_official_build=true is_component_build=false skia_use_vulkan=true skia_use_gl=false skia_enable_svg=true"
    # SVG is not folded into the `skia` Ninja target. Build it explicitly so
    # Awesome can link SkSVGDOM, the shaper, resources, and Unicode archives.
    ninja -C "$skia_out_rel" -j "$jobs" skia libsvg.a
)

for library in libskia.a libsvg.a libskshaper.a libskresources.a \
    libskunicode_icu.a libskunicode_core.a; do
    test -f "$skia_out_dir/$library" || {
        echo "Skia build did not produce required library: $skia_out_dir/$library" >&2
        exit 1
    }
done
