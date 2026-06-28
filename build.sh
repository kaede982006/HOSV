#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build/HOSV"
DIST_DIR="${ROOT_DIR}/dist"

die() {
    echo "build.sh: $*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "required command '$1' was not found"
}

preflight() {
    require_command cmake
    require_command go
    require_command c++
    [ -f "${ROOT_DIR}/HOSVMain/CMakeLists.txt" ] || die "missing HOSVMain/CMakeLists.txt"
    [ -f "${ROOT_DIR}/tools/arkbridge/go.mod" ] || die "missing tools/arkbridge/go.mod"
    [ -f "${ROOT_DIR}/tools/arkbridge/cmd/arkbridge/main.go" ] || die "missing tools/arkbridge/cmd/arkbridge/main.go"
}

clean_build() {
    echo "Cleaning build and distribution directories..."
    rm -rf "${ROOT_DIR}/build"
    rm -rf "${DIST_DIR}"
}

# Handle arguments
if [ $# -gt 0 ]; then
    case "$1" in
        clean)
            clean_build
            echo "Clean completed."
            exit 0
            ;;
        --clean|-c)
            clean_build
            ;;
        -h|--help)
            echo "Usage: $0 [clean | --clean | -c | --help]"
            echo "  clean       Remove build artifacts and exit"
            echo "  --clean, -c Clean build artifacts and perform a fresh build"
            echo "  --help, -h  Show this help message"
            exit 0
            ;;
        *)
            echo "Unknown argument: $1"
            echo "Usage: $0 [clean | --clean | -c | --help]"
            exit 1
            ;;
    esac
fi

preflight

cmake -S "${ROOT_DIR}/HOSVMain" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" --config Release --parallel
rm -f "${BUILD_DIR}/hosvmain"

mkdir -p "${DIST_DIR}"
rm -f "${DIST_DIR}/HOSVMain"
cp "${BUILD_DIR}/hosv" "${DIST_DIR}/HOSV"
chmod +x "${DIST_DIR}/HOSV"
if [ -x "${BUILD_DIR}/arkbridge" ]; then
    cp "${BUILD_DIR}/arkbridge" "${DIST_DIR}/arkbridge"
    chmod +x "${DIST_DIR}/arkbridge"
fi
cp "${ROOT_DIR}/logo.png" "${DIST_DIR}/logo.png"
rm -rf "${DIST_DIR}/fonts"
cp -R "${ROOT_DIR}/fonts" "${DIST_DIR}/fonts"

echo "Built ${DIST_DIR}/HOSV"
echo "Runtime linkage:"
ldd "${DIST_DIR}/HOSV" || true
