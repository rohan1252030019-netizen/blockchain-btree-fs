#!/bin/bash
# ══════════════════════════════════════════════════════════
# install_raylib.sh — Install Raylib 4.5 on Kali / Debian
# Run with: chmod +x INSTALL_RAYLIB.sh && ./INSTALL_RAYLIB.sh
# ══════════════════════════════════════════════════════════
set -e

echo "=== Step 1: System dependencies ==="
sudo apt-get update -qq
sudo apt-get install -y \
    gcc make git \
    libssl-dev \
    libasound2-dev \
    libx11-dev libxrandr-dev libxi-dev \
    libxcursor-dev libxinerama-dev \
    libxkbcommon-dev \
    libgl1-mesa-dev

echo ""
echo "=== Step 2: Try apt libraylib-dev first ==="
if apt-cache show libraylib-dev &>/dev/null; then
    echo "libraylib-dev found in apt — installing..."
    sudo apt-get install -y libraylib-dev
    echo "✓ Done via apt. Run: make"
    exit 0
fi

echo "Not in apt. Building from source (Raylib 4.5.0)..."

echo ""
echo "=== Step 3: Clone Raylib ==="
rm -rf /tmp/raylib_src
git clone --depth 1 --branch 4.5.0 \
    https://github.com/raysan5/raylib.git /tmp/raylib_src

echo ""
echo "=== Step 4: Build ==="
make -C /tmp/raylib_src/src \
    PLATFORM=PLATFORM_DESKTOP \
    RAYLIB_LIBTYPE=SHARED \
    -j"$(nproc)"

echo ""
echo "=== Step 5: Install to /usr/local ==="
sudo make -C /tmp/raylib_src/src install \
    PLATFORM=PLATFORM_DESKTOP \
    RAYLIB_LIBTYPE=SHARED
sudo ldconfig

echo ""
echo "=== Step 6: Verify ==="
if ldconfig -p | grep -q raylib; then
    echo "✓ libraylib.so found in ldconfig"
    ls -la /usr/local/lib/libraylib* 2>/dev/null || true
    ls -la /usr/local/include/raylib.h 2>/dev/null || true
    echo ""
    echo "✓ Raylib installed successfully!"
    echo "  Now run:  make"
else
    echo "⚠ ldconfig doesn't see it yet. Try: sudo ldconfig && make"
fi
