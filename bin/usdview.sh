#!/bin/bash
# Launch the installed RigExec viewer with the Blender file format and runtime.
set -euo pipefail
BLENDER_RIG_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$BLENDER_RIG_ROOT/../usdRig/bin/_env.sh"
BLENDER_RIG_PREFIX="$BLENDER_RIG_ROOT/build/install"
RIG_SDK="$BLENDER_RIG_ROOT/../usdRig/build/install"
if [ -f "$BLENDER_RIG_ROOT/build/rigexec-prefix" ]; then
    read -r RIG_SDK < "$BLENDER_RIG_ROOT/build/rigexec-prefix"
fi
RIG_SDK="${USDBLENDERRIG_RIGEXEC_PREFIX:-$RIG_SDK}"
export PYTHONPATH="$BLENDER_RIG_PREFIX/lib/python:$RIG_SDK/lib/python:$RIG_SDK/lib/python/rigExecUsdview:$PY_SITE${PYTHONPATH:+:$PYTHONPATH}"
export PXR_PLUGINPATH_NAME="$BLENDER_RIG_PREFIX/lib/usd/usdBlenderRig/resources:$RIG_SDK/lib/usd/rigExecSchema/resources:$RIG_SDK/lib/usd/rigExecImaging/resources:$RIG_SDK/lib/python/rigExecUsdview"
export RIGEXEC_IMAGING_DLL="$RIG_SDK/lib/librigExecImaging.dylib"
if [ "$(uname -s)" = Darwin ]; then
    export DYLD_LIBRARY_PATH="$BLENDER_RIG_PREFIX/lib:$RIG_SDK/lib:$USD/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
else
    export LD_LIBRARY_PATH="$BLENDER_RIG_PREFIX/lib:$RIG_SDK/lib:$USD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export RIGEXEC_IMAGING_DLL="$RIG_SDK/lib/librigExecImaging.so"
fi
exec "$PY" "${USDBLENDERRIG_USDVIEW:-$USDVIEW}" "$@"
