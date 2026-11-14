#!/bin/sh
# Hidden, muted test run: ./run_test.sh <seconds> ; extra RC_* env vars pass through.
cd "$(dirname "$0")"
PATH=/usr/bin:/bin:$PATH
S=${1:-15}
export RC_HIDDEN=1 RC_MUTE=1 RC_QUIT=$((S*1000))
timeout $((S+20)) ./RobotCity.exe
echo "exit $?"
tail -n ${TAIL:-40} robotcity.log
