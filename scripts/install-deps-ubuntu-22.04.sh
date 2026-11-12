#!/usr/bin/env bash
# 安装 ROCOS-CO 的构建依赖和 SocketCAN/vcan 调试工具。
set -euo pipefail

source /etc/os-release
if [[ ${ID:-} != ubuntu || ${VERSION_ID:-} != 22.04 ]]; then
    echo "此脚本适用于 Ubuntu 22.04，当前系统为 ${PRETTY_NAME:-未知系统}。" >&2
    exit 1
fi

apt=(apt-get)
if (( EUID != 0 )); then
    if ! command -v sudo >/dev/null; then
        echo "安装系统包需要管理员权限，请以 root 身份运行此脚本。" >&2
        exit 1
    fi
    apt=(sudo apt-get)
fi

packages=(
    build-essential cmake ninja-build git python3 doxygen
    libyaml-cpp-dev linux-libc-dev
    iproute2 can-utils
)

"${apt[@]}" update
"${apt[@]}" install -y --no-install-recommends "${packages[@]}"
echo "ROCOS-CO 系统依赖已安装。"
