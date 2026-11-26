#!/bin/sh
# Instructions a 128-frame block takes, counted by callgrind in Melodee's profiler image (its build image plus
# valgrind), for each bench mode. A block at 48 kHz is 2,667 us; on the MPC's 1.8 GHz Cortex-A55 that is about
# 4.8 million cycles, so the share printed assumes about one instruction a cycle (the A55 is in-order).
#   tools/bench.sh [SECONDS]
set -eu
cd "$(dirname "$0")/.."
CONTEXT=${DOCKER_CONTEXT:-colima-melodee}
SECONDS_RUN=${1:-4}
FRAMES=500 # page frames: 8 s of animation at 16 ms
docker --context "$CONTEXT" run --rm --platform linux/arm64 -v "$PWD:/work" -w /work melodee-profiler sh -c "
    make -s build/bench >/dev/null
    for mode in worst typical stopped; do
        build/bench \$mode $SECONDS_RUN
        refs=\$(valgrind --tool=callgrind --callgrind-out-file=build/callgrind.\$mode build/bench \$mode $SECONDS_RUN 2>&1 | grep -o 'refs: *[0-9,]*' | tr -d 'refs: ,')
        base=\$(valgrind --tool=callgrind --callgrind-out-file=/dev/null build/bench \$mode 0 2>&1 | grep -o 'refs: *[0-9,]*' | tr -d 'refs: ,')
        blocks=\$(awk -v s=$SECONDS_RUN 'BEGIN { printf \"%d\", s * 48000 / 128 }')
        awk -v r=\$refs -v b=\$base -v n=\$blocks -v m=\$mode 'BEGIN { per = (r - b) / n; printf \"%-8s %9.0f instructions a block, %5.2f %% of a block on one A55 core at 1 IPC\\n\", m, per, per / 4800000 * 100 }'
    done
    # The Orbital Impact page, a frame 16 ms apart (on Melodee's screen thread, not the audio one): at rest, through a
    # hit's shock every 400 ms, and with four accented shocks ringing at once.
    make -s build/page >/dev/null
    for mode in rest hit roll; do
        build/page bench $FRAMES \$mode
        refs=\$(valgrind --tool=callgrind --callgrind-out-file=/dev/null build/page bench $FRAMES \$mode 2>&1 | grep -o 'refs: *[0-9,]*' | tr -d 'refs: ,')
        base=\$(valgrind --tool=callgrind --callgrind-out-file=/dev/null build/page bench 0 \$mode 2>&1 | grep -o 'refs: *[0-9,]*' | tr -d 'refs: ,')
        awk -v r=\$refs -v b=\$base -v n=$FRAMES -v m=\$mode 'BEGIN { printf \"page %-5s %9.0f instructions a frame\\n\", m, (r - b) / n }'
    done"
