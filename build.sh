#!/usr/bin/env bash
# Builds the Voice Bridge server plugin on Linux (32-bit voice-bridge.so).
#
#   ./build.sh                 build + tests, files in dist/server
#   ./build.sh --clean         rebuild from scratch
#   ./build.sh --no-tests
#   ./build.sh --version 1.2.0   set the version (saved in the VERSION file)
#
# Needs: cmake, ninja-build, gcc-multilib, g++-multilib
set -euo pipefail

root="$(cd "$(dirname "$0")" && pwd)"
clean=0
tests=1
version=""

while [[ $# -gt 0 ]]; do
	case "$1" in
		--clean) clean=1 ;;
		--no-tests) tests=0 ;;
		--version) version="$2"; shift ;;
		-h|--help) sed -n '2,9p' "$0"; exit 0 ;;
		*) echo "unknown option: $1" >&2; exit 1 ;;
	esac
	shift
done

cd "$root"
if [[ -n "$version" ]]; then
	if [[ ! "$version" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]] || (( 10#${BASH_REMATCH[1]} > 6 || 10#${BASH_REMATCH[2]} > 99 || 10#${BASH_REMATCH[3]} > 99 )); then
		echo "version must be MAJOR.MINOR.PATCH with MAJOR 0-6, MINOR 0-99, PATCH 0-99 (got '$version')" >&2
		exit 1
	fi
	echo "$version" > VERSION
	sed -i "s/#define VOICE_BRIDGE_INCLUDE_VERSION \"[^\"]*\"/#define VOICE_BRIDGE_INCLUDE_VERSION \"$version\"/" include/voice-bridge.inc
else
	version="$(tr -d '[:space:]' < VERSION)"
fi
echo "Voice Bridge $version"
if [[ $clean -eq 1 ]]; then rm -rf build/linux-server; fi

args=(--preset linux-server --log-level=WARNING "-DVOICE_BRIDGE_VERSION=$version")
cmake "${args[@]}"
cmake --build build/linux-server --parallel
if [[ $tests -eq 1 ]]; then ctest --test-dir build/linux-server --output-on-failure; fi

out=dist/server
rm -rf "$out"
mkdir -p "$out/components" "$out/plugins" "$out/include"
cp build/linux-server/plugins/voice-bridge.so "$out/components/"
cp build/linux-server/plugins/voice-bridge.so "$out/plugins/"
cp build/linux-server/pawno/include/*.inc "$out/include/"
cp packaging/SERVER-README.txt "$out/README.txt"
cp LICENSE "$out/"
echo
echo "Voice Bridge $version: $out/components/voice-bridge.so (open.mp), $out/plugins/voice-bridge.so (SA-MP)"
