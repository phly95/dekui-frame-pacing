#!/usr/bin/env bash
# Build Dekui in the upstream CI container (same flags as tools/mm3d/deck/build-ci.sh).
set -euo pipefail
cd "$(dirname "$0")"
podman run --rm --userns=keep-id -v "$PWD:/workspace:Z" -w /workspace \
  -e CCACHE_DIR=/workspace/.ccache -e CCACHE_BASEDIR=/workspace -e CCACHE_MAXSIZE=4G \
  -e EXTRA_QT_PLUGINS=waylandcompositor -e "EXTRA_PLATFORM_PLUGINS=libqwayland-egl.so;libqwayland-generic.so" \
  docker.io/opensauce04/azahar-build-environment:latest bash -c '
set -euo pipefail
test -f build-deck/build.ninja || cmake -S . -B build-deck -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_LINKER=/etc/bin/ld.lld \
  -DENABLE_MM3D_PRESENTATION=ON -DENABLE_MM3D_GAME_FOLDER=ON \
  -DENABLE_BUILTIN_KEYBLOB=OFF -DENABLE_QT=ON -DENABLE_VULKAN=ON \
  -DENABLE_LIBRETRO=OFF -DENABLE_ROOM_STANDALONE=OFF \
  -DENABLE_DISCORD_RPC=OFF -DENABLE_QT_UPDATE_CHECKER=OFF
cmake --build build-deck --parallel 18 '"${1:+--target $1}"'
'
