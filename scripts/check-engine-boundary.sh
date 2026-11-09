#!/bin/sh
#
# Check that the engine library doesn't depend on the UI.
#
#   check-engine-boundary.sh LIBRARY [UI_OBJECT ...]
#
# Three checks:
#
# 1. With nm: every symbol LIBRARY leaves undefined is compared with the
#    global symbols the UI objects define. Any overlap means an engine file
#    calls into (or reads a variable from) the UI.
#
# 2. With the compiler: every object in LIBRARY is linked, together with a
#    main() that registers an empty struct engine_host (so every callback is
#    a default), into a throwaway program. The link fails if anything is left
#    undefined that the system libraries don't provide.
#
# 3. With nm again: none of the objects listed in CORE_OBJECTS (a
#    space-separated list from the environment) may refer to the libc heap
#    (malloc, calloc, realloc, free, strdup, strndup, asprintf, vasprintf and
#    friends), to the aborting wrappers in schism/mem.c, or to the helpers in
#    schism/libc-owned.c that hand out malloc() memory. They allocate through
#    include/engine-alloc.h instead. Skipped if CORE_OBJECTS is unset.
#
# The second check needs CC, and takes CPPFLAGS, CFLAGS, LDFLAGS and LIBS
# from the environment; `make check-engine-boundary` passes the ones
# schismplay is built with, and the core object list. Set NM to use something
# other than nm.

set -e

if [ $# -lt 1 ]; then
	echo "usage: $0 LIBRARY [UI_OBJECT ...]" >&2
	exit 2
fi

lib=$1
shift

: "${NM:=nm}"
: "${CC:=cc}"

tmp=$(mktemp -d "${TMPDIR:-/tmp}/engine-boundary.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

# "name type" for every global symbol, from nm's POSIX output
syms() {
	"$NM" -g -P "$@" 2>/dev/null | awk 'NF >= 2 && $2 ~ /^[A-Za-z]$/ { print $1, $2 }'
}

syms "$lib" > "$tmp/lib"
awk '$2 == "U" { print $1 }' "$tmp/lib" | sort -u > "$tmp/lib-undef"
awk '$2 != "U" && $2 != "w" && $2 != "v" { print $1 }' "$tmp/lib" | sort -u > "$tmp/lib-def"
comm -23 "$tmp/lib-undef" "$tmp/lib-def" > "$tmp/lib-needs"

echo "engine library: $(wc -l < "$tmp/lib-def" | tr -d ' ') symbols defined, $(wc -l < "$tmp/lib-needs" | tr -d ' ') needed from outside"

status=0

if [ $# -gt 0 ]; then
	syms "$@" | awk '$2 != "U" && $2 != "w" && $2 != "v" { print $1 }' | sort -u > "$tmp/ui-def"
	comm -12 "$tmp/lib-needs" "$tmp/ui-def" > "$tmp/bad"
	if [ -s "$tmp/bad" ]; then
		echo "FAIL: the engine refers to these UI symbols:"
		sed 's/^/  /' "$tmp/bad"
		status=1
	else
		echo "ok: none of them is defined by the $# UI objects"
	fi
fi

cat > "$tmp/main.c" <<'EOF'
#include "headers.h"
#include "engine-host.h"

int main(void)
{
	struct engine_host host;

	memset(&host, 0, sizeof(host));
	engine_host_register(&host);
	return 0;
}
EOF

case "$(uname -s)" in
Darwin)
	whole="-Wl,-force_load,$lib"
	;;
*)
	whole="-Wl,--whole-archive $lib -Wl,--no-whole-archive"
	;;
esac

# shellcheck disable=SC2086
if $CC $CPPFLAGS $CFLAGS -c "$tmp/main.c" -o "$tmp/main.o" > "$tmp/cc.log" 2>&1 \
	&& $CC $CFLAGS $LDFLAGS -o "$tmp/engine-only" "$tmp/main.o" $whole $LIBS > "$tmp/ld.log" 2>&1; then
	echo "ok: the whole library links on its own with the default callbacks"
else
	echo "FAIL: linking the library on its own:"
	cat "$tmp/cc.log" "$tmp/ld.log" 2>/dev/null | sed 's/^/  /'
	status=1
fi

if [ -n "${CORE_OBJECTS:-}" ]; then
	heap='malloc calloc realloc reallocf free strdup strndup wcsdup asprintf vasprintf
		valloc pvalloc memalign posix_memalign aligned_alloc
		malloc_type_malloc malloc_type_calloc malloc_type_realloc
		mem_alloc mem_calloc mem_realloc str_dup strn_dup
		charset_iconv charset_compose_to_set charset_case_fold_to_set
		disko_memclose str_break str_escape str_unescape str_concat str_realloc'
	: > "$tmp/heap-bad"
	ncore=0
	for obj in $CORE_OBJECTS; do
		ncore=$((ncore + 1))
		# undefined symbols, with the leading underscore some platforms add
		"$NM" -g -P "$obj" 2>/dev/null | awk '$2 == "U" { sub(/^_/, "", $1); print $1 }' > "$tmp/obj-undef"
		for fn in $heap; do
			if grep -qx "$fn" "$tmp/obj-undef"; then
				echo "$obj: $fn" >> "$tmp/heap-bad"
			fi
		done
	done
	if [ -s "$tmp/heap-bad" ]; then
		echo "FAIL: core engine objects call the libc heap directly:"
		sed 's/^/  /' "$tmp/heap-bad"
		status=1
	else
		echo "ok: none of the $ncore core engine objects calls the libc heap"
	fi
fi

exit $status
