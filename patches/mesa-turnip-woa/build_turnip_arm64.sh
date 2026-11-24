#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Felipe00100 & S22 Windows Community Project Contributors
#
# Cross-compiles Mesa 26.1.0 Turnip Vulkan 1.4.348 (vulkan_freedreno.dll) for
# Windows 11 ARM64 on WSL2/Linux using clang-cl + lld-link + Windows SDK/MSVC headers.
set -euo pipefail

PATCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MESA_DIR="${MESA_DIR:-$HOME/s22-turnip-woa}"
BUILD_DIR="${BUILD_DIR:-$MESA_DIR/build-woa-arm64}"
BASE_REPO="${BASE_REPO:-https://github.com/arehnman/virtio-win-mesa.git}"
BASE_COMMIT="${BASE_COMMIT:-76c095b}"
MSVC_DIR="${MSVC_DIR:-/mnt/c/VS2022/VC/Tools/MSVC/14.44.35207}"
WDK_DIR="${WDK_DIR:-/mnt/c/ADK}"
WDK_VER="${WDK_VER:-10.0.26100.0}"

if [[ ! -d "$MSVC_DIR/include" || ! -d "$WDK_DIR/Include/$WDK_VER/um" ]]; then
    echo "ERROR: Set MSVC_DIR and WDK_DIR to your Windows MSVC ARM64 and Windows SDK ($WDK_VER) paths." >&2
    exit 1
fi

if [[ ! -d "$MESA_DIR/.git" ]]; then
    echo "[1/4] Cloning base Mesa tree ($BASE_REPO @ $BASE_COMMIT)..."
    git clone "$BASE_REPO" "$MESA_DIR"
    git -C "$MESA_DIR" checkout "$BASE_COMMIT"
fi

echo "[2/4] Applying Windows ARM64 Turnip patches + WDDM 2.0 backend..."
if ! git -C "$MESA_DIR" apply --check "$PATCH_DIR/mesa-turnip-win-arm64.patch" 2>/dev/null; then
    echo "  (Patch already applied or tree already modified; continuing)"
else
    git -C "$MESA_DIR" apply "$PATCH_DIR/mesa-turnip-win-arm64.patch"
fi
cp "$PATCH_DIR/tu_knl_wddm.cc" "$MESA_DIR/src/freedreno/vulkan/tu_knl_wddm.cc"
cp "$PATCH_DIR/adreno_wddm_escape.h" "$MESA_DIR/src/freedreno/vulkan/adreno_wddm_escape.h"

sed -e "s|@MSVC_DIR@|$MSVC_DIR|g" \
    -e "s|@WDK_DIR@|$WDK_DIR|g" \
    -e "s|@WDK_VER@|$WDK_VER|g" \
    "$PATCH_DIR/cross-win-arm64.txt.in" > "$MESA_DIR/cross-win-arm64.txt"

echo "[3/4] Configuring Meson (Windows ARM64 release)..."
if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
    meson setup "$BUILD_DIR" "$MESA_DIR" \
        --cross-file "$MESA_DIR/cross-win-arm64.txt" \
        --buildtype=release \
        -Dplatforms=windows \
        -Dgallium-drivers= \
        -Dvulkan-drivers=freedreno \
        -Dfreedreno-kmds=wddm \
        -Dopengl=false \
        -Dgles1=disabled \
        -Dgles2=disabled \
        -Degl=disabled \
        -Dglx=disabled \
        -Dzlib=disabled \
        -Dzstd=disabled \
        -Dllvm=disabled \
        -Dshared-llvm=disabled \
        -Dcpp_rtti=false
fi

echo "[4/4] Building vulkan_freedreno.dll..."
ninja -C "$BUILD_DIR" src/freedreno/vulkan/vulkan_freedreno.dll
ls -lh "$BUILD_DIR/src/freedreno/vulkan/vulkan_freedreno.dll"

if [[ "${BUILD_ZINK:-1}" == "1" ]]; then
    ZINK_DIR="${ZINK_DIR:-$MESA_DIR/build-zink-arm64}"
    echo "[5/5] Building Mesa Zink OpenGL 4.6 (opengl32.dll + libgallium_wgl.dll)..."
    if [[ ! -f "$ZINK_DIR/build.ninja" ]]; then
        meson setup "$ZINK_DIR" "$MESA_DIR" \
            --cross-file "$MESA_DIR/cross-win-arm64.txt" \
            --buildtype=release \
            -Dplatforms=windows \
            -Dgallium-drivers=zink \
            -Dvulkan-drivers= \
            -Dopengl=true \
            -Dgles1=disabled \
            -Dgles2=disabled \
            -Degl=disabled \
            -Dglx=disabled \
            -Dzlib=disabled \
            -Dzstd=disabled \
            -Dllvm=disabled \
            -Dshared-llvm=disabled \
            -Dcpp_rtti=false \
            -Dmicrosoft-clc=disabled \
            -Dgallium-mediafoundation=disabled \
            -Dvideo-codecs= \
            -Dxmlconfig=disabled
    fi
    ninja -C "$ZINK_DIR" src/gallium/targets/libgl-gdi/opengl32.dll src/gallium/targets/wgl/libgallium_wgl.dll
    ls -lh "$ZINK_DIR/src/gallium/targets/libgl-gdi/opengl32.dll" "$ZINK_DIR/src/gallium/targets/wgl/libgallium_wgl.dll"
fi
