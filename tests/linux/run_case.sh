#!/bin/bash
# needs env B (build dir with fake_game, libtogl.so, libSDL2-2.0.so.0, mock_helper) and SHIM (32-bit shim .so)
# usage: run_case.sh <name> <helper args...> -- <config file content or ""> -- <env assignments...>
# Builds /tmp/h3a/cases/<name>, runs mock helper + fake game with the shim preloaded.
set -u
name=$1; shift
helper_args=(); while [ "${1:-}" != "--" ]; do helper_args+=("$1"); shift; done; shift
cfg=$1; shift; shift
envs=("$@")
D=$B/cases/$name; rm -rf "$D"; mkdir -p "$D"
cp "$B/fake_game" "$B/libtogl.so" "$B/libSDL2-2.0.so.0" "$D/"
[ -n "${PRE_FLAG:-}" ] && : > "$D/bmvr_in_stereo.flag"   # T29: a previous launch died with stereo active
[ -n "$cfg" ] && printf '%b' "$cfg" > "$D/bmvr_target.txt"
export MOCK_TRACE="$D/trace.txt"; : > "$MOCK_TRACE"
( cd "$D" && "$B/mock_helper" "$D" "${helper_args[@]}" > helper.out 2>&1 ) &
HP=$!
for i in $(seq 1 50); do grep -q listening "$D/helper.out" 2>/dev/null && break; sleep 0.1; done
( cd "$D" && env "${envs[@]}" LD_LIBRARY_PATH="$D" LD_PRELOAD="$SHIM" timeout 90 ./fake_game ${GAME_ARGS:-300 500} > game.out 2> game.err ); echo "game exit=$?" > "$D/exit.txt"
kill $HP 2>/dev/null; wait $HP 2>/dev/null
exit 0
