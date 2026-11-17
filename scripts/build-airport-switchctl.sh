#!/bin/sh
# Build the curated A1470/NetBSD 6 ARM tools; never connects to the device.
# A compatible, caller-supplied NetBSD6 ARM SDK and Zig are required.
set -eu

if [ "${AIRPORT_UNSAFE_RESEARCH:-}" != "YES" ]; then
    echo "BLOCKED: set AIRPORT_UNSAFE_RESEARCH=YES only in a controlled lab" >&2
    exit 1
fi
: "${AIRPORT_NETBSD_SDK:?Set AIRPORT_NETBSD_SDK to a compatible NetBSD6 ARM SDK}"
SDK=$AIRPORT_NETBSD_SDK
ZIG=${AIRPORT_ZIG:-zig}
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TOOL_DEFINES=
case ${AIRPORT_TOOL_MODE:-read-only} in
    three-vlan-service)
        SOURCE="$ROOT/src/airport-dot1q-test.c"
        DEFAULT_OUTPUT="$ROOT/build/airport-three-vlan.netbsd"
        TOOL_DEFINES=-DAIRPORT_THREE_VLAN_SERVICE
        ;;
    read-only)
        SOURCE="$ROOT/src/airport-switchctl.c"
        DEFAULT_OUTPUT="$ROOT/build/airport-switchctl.netbsd"
        ;;
    vlan-table-query)
        SOURCE="$ROOT/src/airport-vlan-table-reader.c"
        DEFAULT_OUTPUT="$ROOT/build/airport-vlan-table-reader.netbsd"
        ;;
    *)
        echo "Invalid AIRPORT_TOOL_MODE (read-only, vlan-table-query, three-vlan-service)" >&2
        exit 2
        ;;
esac
OUT=${1:-"$DEFAULT_OUTPUT"}

mkdir -p "$(dirname "$OUT")"
BUILD_TMP=$(mktemp -d)
trap 'rm -rf "$BUILD_TMP"' EXIT HUP INT TERM

"$ZIG" cc -target arm-freestanding-eabi -fno-pie -fno-PIE \
    -DAIRPORT_EXPLICIT_UNSAFE_RESEARCH $TOOL_DEFINES -nostdinc \
    -I "$SDK/usr/include" -c "$SOURCE" \
    -o "$BUILD_TMP/airport-switchctl.o"

"$ZIG" ld.lld -static -e _start -o "$OUT" \
    "$SDK/usr/lib/crt0.o" "$SDK/usr/lib/crti.o" \
    "$SDK/usr/lib/crtbegin.o" "$BUILD_TMP/airport-switchctl.o" \
    -L"$SDK/usr/lib" -lc -lgcc \
    "$SDK/usr/lib/crtend.o" "$SDK/usr/lib/crtn.o"

file "$OUT"
