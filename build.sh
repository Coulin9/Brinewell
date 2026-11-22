#!/bin/bash
# Brinewell build entry point.
#
#   ./build.sh                          incremental build (rebuild only components whose inputs changed)
#   ./build.sh --clean                  full clean rebuild
#   ./build.sh --component NAME [--clean]   build one component (incremental unless --clean)
#   ./build.sh --reuse kernel,layer     reuse only the named artifacts
#   ./build.sh --reuse all              reuse every native/guest artifact
#   ./build.sh --list-components
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=scripts/lib/common.sh
source "$ROOT/scripts/lib/common.sh"

COMPONENTS=(
    kernel
    libepoxy
    virglrenderer
    d3dmetal
    mullion
    libkrun
    kosmicrisp
    moltenvk
    gvproxy
    macmon
    initramfs
    mesa
    agent
    uu-runtime
    fex-eac
    mangoapp
    guest-integration
    neptune-probe
    neptune
    layer
    swift
)

REUSE=""
ONLY=""
CLEAN=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean) CLEAN=1; shift ;;
        --reuse)
            [[ $# -ge 2 ]] || die "--reuse requires a list or 'all'"
            REUSE="$2"; shift 2 ;;
        --reuse=*) REUSE="${1#*=}"; shift ;;
        --component)
            [[ $# -ge 2 ]] || die "--component requires a name"
            ONLY="$2"; shift 2 ;;
        --list-components) printf '%s\n' "${COMPONENTS[@]}"; exit 0 ;;
        -h|--help) sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

if [[ -n "$REUSE" ]]; then
    [[ $CLEAN -eq 0 ]] || die "--clean cannot be combined with --reuse"
    if [[ "$REUSE" != "all" ]]; then
        IFS=',' read -r -a requested <<< "$REUSE"
        for name in "${requested[@]}"; do
            found=0
            for known in "${COMPONENTS[@]}"; do [[ "$name" == "$known" ]] && found=1; done
            [[ $found -eq 1 ]] || die "unknown component in --reuse: $name"
        done
    fi
    export BRINEWELL_REUSE="$REUSE"
    log "explicit reuse: $REUSE"
else
    if [[ $CLEAN -eq 1 ]]; then
        export BRINEWELL_REUSE=""
        log "full build"
        if [[ -z "$ONLY" ]]; then
            rm -rf "$OUT"
            mkdir -p "$OUT"
        fi
    else
        export BRINEWELL_REUSE=auto
        log "incremental build"
    fi
fi

run_component() {
    local name="$1"
    local script="$ROOT/scripts/components/$name.sh"
    [[ -f "$script" ]] || die "missing component script: ${script#"$ROOT"/}"
    log "component: $name"
    # Run the component in its own process so a reuse path's `exit 0` does not
    # terminate the build loop before packaging.
    bash "$script"
}

if [[ -n "$ONLY" ]]; then
    found=0
    for known in "${COMPONENTS[@]}"; do [[ "$ONLY" == "$known" ]] && found=1; done
    [[ $found -eq 1 ]] || die "unknown component: $ONLY"
    run_component "$ONLY"
    exit 0
fi

START=$(date +%s)
for name in "${COMPONENTS[@]}"; do
    run_component "$name"
done
"$ROOT/scripts/package.sh"
log "build complete in $(( $(date +%s) - START ))s: ${APP#"$ROOT"}"
