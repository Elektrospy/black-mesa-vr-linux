#!/bin/bash
cd "$(dirname "$0")"; rm -f work/bmvr_linux_log.txt work/frames.json
printf '# comment\ntarget=name:small\nflip=1\n' > work/bmvr_target.txt
python3 mock_helper.py work work/frames.json > work/helper.out 2>&1 & HP=$!; sleep 0.5
LD_PRELOAD=$PWD/bmvr_preload64.so ./work/bms_linux normal 800 >/dev/null 2>&1; wait $HP
grep -E "xfer: target" work/bmvr_linux_log.txt
python3 -c "
import json; fr=json.load(open('work/frames.json')); print('frames:', [(f['name'], f['flags']&1) for f in fr])"
rm -f work/bmvr_target.txt
