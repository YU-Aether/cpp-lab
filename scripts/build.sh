#!/usr/bin/env bash
# 一键构建：默认常规构建 → build/；--tsan 构建 ThreadSanitizer 版 → build-tsan/
set -euo pipefail

if [[ "${1:-}" == "--tsan" ]]; then
    BUILD_DIR=build-tsan
    # TSan 需要编译与链接都带 -fsanitize=thread，-g 保留符号便于读报告
    TSAN_FLAGS='-fsanitize=thread -g'
    cmake -S . -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="$TSAN_FLAGS" \
        -DCMAKE_EXE_LINKER_FLAGS="$TSAN_FLAGS"
else
    BUILD_DIR=build
    cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
fi

cmake --build "$BUILD_DIR" --parallel "$(nproc)"
echo "构建完成：$BUILD_DIR"
