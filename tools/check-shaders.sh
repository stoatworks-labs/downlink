#!/usr/bin/env bash
#
# Every shader the plugin compiles, through a real GLSL compiler, before a host
# has to find out. Called by tools/verify.sh AND by CI, so the two cannot
# drift: a GitHub macOS runner cannot create an accelerated GL context, so in
# CI this is the only thing that looks at the GLSL at all. It is not a
# substitute for a driver -- only a real driver catches the class of bug where
# Apple's Metal-backed GL disagrees with the compiler -- and that is checked on
# the dev Mac by every dltest GL check, and nowhere else.
#
#   tools/check-shaders.sh [path/to/dltest]
#
# The shaders are NOT scraped out of the source with a regex. The link and its
# probe are assembled at compile time from one macro, which a regex cannot
# expand; `dltest --dump-shaders DIR` writes the very strings the plugin hands
# the driver, and prints how many.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules, not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#
set -uo pipefail
cd "$(dirname "$0")/.."

DLTEST="${1:-build/dltest}"
EXPECTED=12

if ! command -v glslc >/dev/null 2>&1; then
	printf '   skipped: glslc not installed (brew install shaderc)\n'
	exit 0
fi
if [ ! -x "$DLTEST" ]; then
	printf '   %s is not built\n' "$DLTEST"
	exit 1
fi

dir="$(mktemp -d)"
trap 'rm -rf "$dir"' EXIT

count=$("$DLTEST" --dump-shaders "$dir")
n=0
bad=0
for shader in "$dir"/*.vert "$dir"/*.frag; do
	[ -e "$shader" ] || continue
	n=$(( n + 1 ))
	if ! glslc --target-env=opengl4.5 -fauto-map-locations "$shader" -o /dev/null 2>"$dir/err"; then
		printf '   %s does not compile\n' "$(basename "$shader")"
		sed "s|$dir/||; s|^|      |" "$dir/err"
		bad=$(( bad + 1 ))
	fi
done

# The count is asserted, not counted up to: a check that silently looks at
# fewer shaders than exist is worse than no check.
if [ "$n" -ne "$EXPECTED" ] || [ "$count" != "$EXPECTED" ]; then
	printf '   %d shaders written (dltest says %s), expected %d -- update EXPECTED with the list\n' "$n" "$count" "$EXPECTED"
	bad=$(( bad + 1 ))
fi
if [ "$bad" -eq 0 ]; then
	printf '   %d shaders, all compile\n' "$n"
fi
exit "$bad"
