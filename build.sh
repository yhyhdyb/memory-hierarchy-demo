#!/usr/bin/env bash
# 一键编译（Linux / macOS）
set -e
cd "$(dirname "$0")"
g++ -O2 -std=c++17 -Wall -Wextra -pthread \
    -o memhier \
    src/main.cpp src/cache_hierarchy.cpp src/ram_vs_disk.cpp src/dma_sim.cpp
echo "编译成功：./memhier"
echo "运行：./memhier [1|2|3] [--disk-mb=N] [--mem-mb=N]"
