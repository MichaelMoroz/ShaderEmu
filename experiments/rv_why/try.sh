#!/bin/bash
# try.sh LOAD TAG GEN_ARGS... : translate, make the tick, run it on the card, print the row. (AOT_RUN: run.py's extra args; MK: mk_tick.py's)
L=$1; T=$2; shift 2
D=C:/Users/micha/agents_isa/aot/$L
python experiments/rv_why/aot.py $D/$L.snap $D $D/g_$T.hlsl "$@" | cut -c1-220
python experiments/rv_why/mk_tick.py aot $D/g_$T.hlsl $MK > /dev/null
python experiments/rv_why/run.py $L rvc_lean_aot --tag $T $AOT_RUN 2>&1 | python experiments/rv_why/row.py
