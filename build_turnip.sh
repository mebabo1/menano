#!/bin/bash
set -e

echo "🚀 Turnip (Mesa) Termux/Android 최적화 빌드 시작..."

WORK_DIR=$(pwd)/turnip_workdir
INSTALL_DIR=$WORK_DIR/install
SOURCE_DIR=$WORK_DIR/mesa-src
if [ -z "$ANDROID_NDK_HOME" ] && [ ! -d "$NDK_VER" ]; then
    echo "📥 NDK ($NDK_VER) 다운로드 중..."
    curl -sL "https://dl.google.com/android/repository/${NDK_VER}-linux.zip" -o "${NDK_VER}.zip"
    echo "📦 압축 해제 중..."
    unzip -q "${NDK_VER}.zip"
    rm "${NDK_VER}.zip"
    export ANDROID_NDK_HOME="$WORK_DIR/$NDK_VER"
elif [ -d "$NDK_VER" ]; then
    export ANDROID_NDK_HOME="$WORK_DIR/$NDK_VER"
fi

mkdir -p "$WORK_DIR"

if [ ! -d "$SOURCE_DIR" ]; then
    echo "📥 소스 클론 중..."
    git clone --depth 1 https://github.com/lfdevs/mesa-for-android-container.git "$SOURCE_DIR"
fi

cd "$SOURCE_DIR"

# [핵심 수정] 안드로이드 빌드를 위한 메모리 핸들러 패치 (필수)
sed -i 's/typedef const native_handle_t\* buffer_handle_t;/typedef void\* buffer_handle_t;/g' include/android_stub/cutils/native_handle.h || true
sed -i 's/, hnd->handle/, (void \*)hnd->handle/g' src/util/u_gralloc/u_gralloc_fallback.c || true

# 의존성 소스 준비
mkdir -p subprojects && cd subprojects
git clone --depth=1 https://github.com/KhronosGroup/SPIRV-Tools.git spirv-tools || true
git clone --depth=1 https://github.com/KhronosGroup/SPIRV-Headers.git spirv-headers || true
cd ..

OPTIM_FLAGS="-march=armv8-a" # Termux 범용성을 위해 armv8-a 권장 (본인 기기가 고사양이면 armv9-a 유지)

meson setup builddir \
    --prefix=/usr \
    --libdir=lib \
    -Dbuildtype=release \
    -Doptimization=3 \
    -Dplatforms=android \
    -Dandroid-stub=true \
    -Dgallium-drivers=freedreno \
    -Dvulkan-drivers=freedreno \
    -Dfreedreno-kmds=kgsl \
    -Dvulkan-beta=true \
    -Dglx=disabled \
    -Degl=disabled \
    -Dgbm=disabled \
    -Dcpp_args="$OPTIM_FLAGS" \
    -Dc_args="$OPTIM_FLAGS" \
    --force-fallback-for=spirv-tools,spirv-headers

# 컴파일
ninja -C builddir -j$(nproc)

# 설치 및 드라이버 이름 변경 (안드로이드 로더가 인식하도록)
DESTDIR="$INSTALL_DIR" ninja -C builddir install
cp builddir/src/freedreno/vulkan/libvulkan_freedreno.so "$INSTALL_DIR/vulkan.adreno.so"

echo "✅ 빌드 완료! 결과물: $INSTALL_DIR/vulkan.adreno.so"
