#!/bin/bash
#Build OF-A generator + test harness against the in-tree opusfile sources and
# run the failed-seek rollback test.
#
#Environment notes (see project memory):
# - No system gcc/ASAN runtime here: use the zig-bundled clang.
# - /tmp is a small shared tmpfs consumed by other sessions: keep TMPDIR under
#    the roomy working copy.
# - MALLOC_CHECK_=3 hardens glibc heap usage during the run.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PFX="${OFA_PREFIX:-$HOME/local/prefix/usr}"
LIB="$PFX/lib/x86_64-linux-gnu"
INC_OPUS="$PFX/include/opus"
INC_OGG="$PFX/include"
WORK="$ROOT/.worktmp"
TMPDIR="$WORK/tmp"
mkdir -p "$TMPDIR" "$WORK/obj" "$WORK/bin"
export TMPDIR

CC=(python3 -m ziglang cc)
WARN=(-Wall -Wextra -Wno-long-long)
#Static-link the dependency libs so the binaries run without LD_LIBRARY_PATH.
LIBS=("$LIB/libopus.a" "$LIB/libogg.a" -lm)

echo "== compiling opusfile library objects =="
for src in opusfile stream info internal; do
  "${CC[@]}" -c -O1 -g "${WARN[@]}" \
    -I"$ROOT/include" -I"$INC_OPUS" -I"$INC_OGG" \
    "$ROOT/src/$src.c" -o "$WORK/obj/$src.o"
done

echo "== building ofa_gen =="
"${CC[@]}" -O1 -g "${WARN[@]}" \
  -I"$INC_OPUS" -I"$INC_OGG" \
  "$HERE/ofa_gen.c" "${LIBS[@]}" -o "$WORK/bin/ofa_gen"

echo "== building ofa_test =="
"${CC[@]}" -O1 -g "${WARN[@]}" \
  -I"$ROOT/include" -I"$INC_OPUS" -I"$INC_OGG" \
  "$HERE/ofa_test.c" \
  "$WORK/obj/opusfile.o" "$WORK/obj/stream.o" "$WORK/obj/info.o" \
  "$WORK/obj/internal.o" \
  "${LIBS[@]}" -o "$WORK/bin/ofa_test"

echo "== generating fixture =="
FIX="$WORK/ofa.oga"
"$WORK/bin/ofa_gen" "$FIX"
ls -l "$FIX"

echo "== running failed-seek rollback test =="
MALLOC_CHECK_=3 "$WORK/bin/ofa_test" "$FIX"
