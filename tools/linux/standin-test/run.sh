#!/bin/bash
# usage: run.sh <mode> [env assignments...]   -> prints driver output, shim log, helper frames
mode="$1"; shift
cd "$(dirname "$0")"
rm -f work/bmvr_linux_log.txt work/bmvr_target.txt work/frames.json
if [ "$mode" != "nohelper" ]; then python3 mock_helper.py work work/frames.json > work/helper.out 2>&1 & HP=$!; sleep 0.5; fi
env "$@" LD_PRELOAD=$PWD/bmvr_preload64.so timeout 60 ./work/bms_linux "${mode/nohelper/normal}" 800 2> work/stderr.txt; rc=$?
[ -n "$HP" ] && { wait $HP 2>/dev/null; }
echo "driver exit code: $rc"; cat work/stderr.txt | grep -v "bmvr_preload: log file" 
true
