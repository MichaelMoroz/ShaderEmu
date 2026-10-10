#!/usr/bin/env bash
# Every compiled stage in a harness shader cache folder: through dxbc-spirv (assertions on) and the validator.
export PATH=$HOME/vrc/pkg/usr/bin:$PATH
T=~/vrc/dxbc-spirv/build/tools/dxbc_compiler
dir=$1; ok=0; bad=0
for f in "$dir"/*.cso; do
    if ! $T "$f" --spv /tmp/all.spv > /dev/null 2> /tmp/all.err; then
        echo "TRANSLATOR FAILS: $(basename $f) ($(stat -c %s $f) bytes): $(grep -v '^\s' /tmp/all.err | head -1 | cut -c1-160)"; bad=$((bad+1)); continue
    fi
    if ! spirv-val --target-env vulkan1.3 /tmp/all.spv > /tmp/all.val 2>&1; then
        echo "INVALID SPIR-V: $(basename $f) ($(stat -c %s $f) bytes): $(head -2 /tmp/all.val | tr '\n' ' ' | cut -c1-160)"; bad=$((bad+1)); continue
    fi
    ok=$((ok+1))
done
echo "$dir: $ok stages translate to valid SPIR-V, $bad do not"
