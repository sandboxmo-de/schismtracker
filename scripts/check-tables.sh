#!/bin/sh
#
# Check that the generated lookup tables in include/player are current:
# build each generator with the build machine's compiler, run it, and
# compare the output with the committed header.
#
#   check-tables.sh SRCDIR
#
# Set CC to pick the compiler (default cc); it has to make programs that run
# here. To regenerate a header after changing its generator, run the
# generator the same way and commit its output. The command for each one is
# in the list below.

set -e

if [ $# -ne 1 ]; then
	echo "usage: $0 SRCDIR" >&2
	exit 2
fi

src=$1
: "${CC:=cc}"

tmp=$(mktemp -d "${TMPDIR:-/tmp}/check-tables.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

$CC -O2 -o "$tmp/lutgen" "$src/scripts/lutgen.c" -lm
$CC -O2 -o "$tmp/gen-opl-tables" "$src/scripts/gen-opl-tables.c" -lm

status=0

# header, then the command that makes it
check() {
	header=$1
	shift
	"$@" > "$tmp/out.h"
	if cmp -s "$tmp/out.h" "$src/$header"; then
		echo "ok: $header"
	else
		echo "FAIL: $header doesn't match what '$(basename "$1") $2' makes now"
		status=1
	fi
}

check include/player/precomp_lut.h "$tmp/lutgen"
check include/player/fmopl2_tables.h "$tmp/gen-opl-tables" opl2
check include/player/fmopl3_tables.h "$tmp/gen-opl-tables" opl3

exit $status
