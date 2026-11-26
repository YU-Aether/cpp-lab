#!/usr/bin/env bash
# 一键测试：bash scripts/test.sh [构建目录]，默认 build/，TSan 版传 build-tsan
set -euo pipefail

BUILD_DIR="${1:-build}"

# Qt 用例无显示环境时走 offscreen（已设置过则尊重现有值）
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"

ctest --test-dir "$BUILD_DIR" --output-on-failure
