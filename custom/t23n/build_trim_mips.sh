#!/bin/bash
# Final recommended build: trimmed ncnn + yolo26 for MIPS (Ingenic Xburst, uclibc).
#
# Recipe validated on device 192.168.66.56 (2026-11-23, 100-run averages):
#   -O2 + -ffast-math  -> 964K, 1987 ms/run   (overall best; default)
#   -Os + -ffast-math  -> 842K, 2596 ms/run   (smallest; OPT=-Os)
#   -ffast-math is worth ~22-28% speed on this in-order MIPS core (gemm reassociation).
#   Old baseline for context: Aug-master full lib Release = 1.55M @ 2536 ms.
#
# IMPORTANT build-system facts (hard-won):
#   1) The /opt toolchain dir ships an old cmake without -B support -> use
#      /usr/bin/cmake.
#   2) toolchains/mips32r2-linux-gnu.toolchain.cmake sets CMAKE_C(XX)_FLAGS as a
#      *normal* variable, which shadows -DCMAKE_C(XX)_FLAGS from the command
#      line (the cache entry updates but the flags never reach the compiler).
#      Per-config CMAKE_<LANG>_FLAGS_MINSIZEREL is NOT shadowed, so all size
#      flags are injected there. Verify: grep 'CXX_FLAGS = ' <build>/src/
#      CMakeFiles/ncnn.dir/flags.make
#   3) ncnn forces CMAKE_BUILD_TYPE=release when empty and then adds
#      -Ofast -ffast-math for Release only (src/CMakeLists.txt). MinSizeRel
#      gets no fast-math, hence the explicit -ffast-math below.
#   4) WITH_LAYER_* cache names are lowercase: always tr 'A-Z' 'a-z'.
#   5) Never leave a CMakeCache.txt in the source dir: cmake reads it as cache
#      hints for out-of-source builds and silently overrides -D options.
set -euo pipefail
# Builds against the main ncnn clone (default: /home/wushiwei/work2/ncnn,
# override with NCNN_SRC=/path). Trimming is done purely by the WITH_LAYER_*=OFF
# flags below; the upstream source itself is not modified.
# math_stub.c (uclibc shim for fesetround/fegetround/nearbyintf) lives next to
# this script, not in the ncnn tree.
APP_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC=${NCNN_SRC:-/home/wushiwei/work2/ncnn}
[ -f "$SRC/CMakeLists.txt" ] && [ -f "$SRC/examples/yolo26.cpp" ] && [ -f "$APP_DIR/math_stub.c" ] \
    || { echo "ERROR: ncnn source not found at $SRC (set NCNN_SRC=) or math_stub.c missing next to this script"; exit 1; }
cd "$SRC"

OPT=${OPT:--O2}                     # -O2 (speed/size balance) or -Os (size)
BUILD=${BUILD:-build-mips-trim-fast}
MF="$OPT -DNDEBUG -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -fvisibility=hidden -ffast-math"

export PATH=/opt/mips-gcc540-glibc222-64bit-r3.3.0.smaller/bin:$PATH
STRIP=mips-linux-uclibc-gnu-strip

# ReLU is REQUIRED: ncnn fuses LeakyReLU as activation_type=2 into Convolution
# (`-23310=1,0.01 9=2`) and create_activation_layer() instantiates LayerType::ReLU
# (ReLU carries the negative slope).  Without ReLU compiled in, create_layer_cpu()
# returns NULL and the process segfaults while loading any LeakyReLU-fused model.
KEEP=" Input Convolution ConvolutionDepthWise Gemm Bias Scale Padding Crop Cast Flatten Packing Quantize Dequantize Requantize Swish ReLU Split Slice BinaryOp Concat Reshape MatMul Pooling Softmax Permute MemoryData Interp Sigmoid "
LF=$(grep -oP 'ncnn_add_layer\(\K\w+' src/CMakeLists.txt \
    | grep -vwE '\b(Input|Convolution|ConvolutionDepthWise|Gemm|Bias|Scale|Padding|Crop|Cast|Flatten|Packing|Quantize|Dequantize|Requantize|Swish|ReLU|Split|Slice|BinaryOp|Concat|Reshape|MatMul|Pooling|Softmax|Permute|MemoryData|Interp|Sigmoid)\b' \
    | tr 'A-Z' 'a-z' | sed 's/.*/-DWITH_LAYER_&=OFF/' | tr '\n' ' ')

[ -f CMakeCache.txt ] && { echo "ERROR: in-source CMakeCache.txt would poison flags; remove it first"; exit 1; }

rm -rf "$BUILD"
/usr/bin/cmake -B "$BUILD" -S . \
    -DCMAKE_TOOLCHAIN_FILE=toolchains/mips32r2-linux-gnu.toolchain.cmake \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DCMAKE_C_FLAGS_MINSIZEREL="$MF" \
    -DCMAKE_CXX_FLAGS_MINSIZEREL="$MF" \
    -DCMAKE_INSTALL_PREFIX="$BUILD/install" \
    -DNCNN_VULKAN=OFF -DNCNN_MSA=OFF -DNCNN_RUNTIME_CPU=ON \
    -DNCNN_OPENMP=OFF -DNCNN_BENCHMARK=OFF \
    -DNCNN_BUILD_TOOLS=OFF -DNCNN_BUILD_EXAMPLES=OFF \
    -DNCNN_BUILD_TESTS=OFF -DNCNN_BUILD_BENCHMARK=OFF \
    -DNCNN_SIMPLEOCV=ON \
    $LF > /dev/null

# flags sanity: fast-math + trimming must be visible in the actual compile line
grep -q -- '-ffast-math' "$BUILD/src/CMakeFiles/ncnn.dir/flags.make" \
    || { echo "ERROR: -ffast-math missing from compile flags"; exit 1; }
[ "$(grep -c 'WITH_LAYER.*:BOOL=OFF' "$BUILD/CMakeCache.txt")" -gt 70 ] \
    || { echo "ERROR: layer trimming not applied"; exit 1; }

make -C "$BUILD" -j"$(nproc)" > /dev/null
make -C "$BUILD" install > /dev/null

CC=mips-linux-uclibc-gnu-gcc
CXX=mips-linux-uclibc-gnu-g++
AF="-march=mips32r2 -muclibc $OPT -DNDEBUG -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti"
AF+=" -I$BUILD/install/include/ncnn -DUSE_NCNN_SIMPLEOCV"
${CC} ${AF} -c "$APP_DIR/math_stub.c" -o "$BUILD/math_stub.o"
${CXX} ${AF} examples/yolo26.cpp "$BUILD/math_stub.o" \
    "$BUILD/install/lib/libncnn.a" -o "$BUILD/yolo26" \
    -Wl,--gc-sections -lpthread -ldl -lm
${STRIP} "$BUILD/yolo26"
ls -lh "$BUILD/yolo26" "$BUILD/install/lib/libncnn.a"
