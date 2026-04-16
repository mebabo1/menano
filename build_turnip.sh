#!/bin/bash
set -e

echo "🚀 Turnip (Mesa) Termux/Android 최적화 빌드 시작..."

WORK_DIR="$(pwd)/turnip_workdir"
INSTALL_DIR="$WORK_DIR/install"
SOURCE_DIR="$WORK_DIR/mesa-src"
NDK_VER="android-ndk-r29"

mkdir -p "$WORK_DIR"
cd "$WORK_DIR"

# 1. NDK 다운로드 및 설정
if [ -z "$ANDROID_NDK_HOME" ] && [ ! -d "$NDK_VER" ]; then
    echo "📥 NDK ($NDK_VER) 다운로드 중..."
    curl -sL "https://dl.google.com/android/repository/${NDK_VER}-linux.zip" -o "${NDK_VER}.zip"
    unzip -q "${NDK_VER}.zip"
    rm "${NDK_VER}.zip"
fi
export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$WORK_DIR/$NDK_VER}"
echo "📂 NDK 경로: $ANDROID_NDK_HOME"

# 2. Mesa 소스 클론
if [ ! -d "$SOURCE_DIR" ]; then
    echo "📥 Mesa 소스 클론 중..."
    git clone --depth 1 https://github.com/lfdevs/mesa-for-android-container.git "$SOURCE_DIR"
fi

# 3. 의존성 (libdrm, SPIRV) 준비 - subprojects 폴더 내부에 위치해야 함
echo "📦 의존성 소스 정리 중..."
SUB_DIR="$SOURCE_DIR/subprojects"
mkdir -p "$SUB_DIR"

# libdrm 클론 (이름은 반드시 'libdrm'이어야 함)
if [ ! -d "$SUB_DIR/libdrm" ]; then
    git clone --depth 1 https://gitlab.freedesktop.org/mesa/drm.git "$SUB_DIR/libdrm"
fi

# SPIRV 도구들 클론
if [ ! -d "$SUB_DIR/spirv-tools" ]; then
    git clone --depth=1 https://github.com/KhronosGroup/SPIRV-Tools.git "$SUB_DIR/spirv-tools"
    git clone --depth=1 https://github.com/KhronosGroup/SPIRV-Headers.git "$SUB_DIR/spirv-headers"
fi

# 4. 소스 코드 패치 (경로 주의)
cd "$SOURCE_DIR"
echo "🩹 메모리 핸들러 패치 적용 중..."
# mesa 소스 내부의 stub 파일 수정
sed -i "s/dep_libdrm = dependency('libdrm'/dep_libdrm = dependency('libdrm', fallback : ['libdrm', 'libdrm_dep'] /g" meson.build
sed -i 's/typedef const native_handle_t\* buffer_handle_t;/typedef void\* buffer_handle_t;/g' include/android_stub/cutils/native_handle.h || true
sed -i 's/, hnd->handle/, (void \*)hnd->handle/g' src/util/u_gralloc/u_gralloc_fallback.c || true

# 5. 크로스 파일 생성 (pkgconfig 차단 강화)
NDK_BIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
NDK_SYS="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/sysroot"
API=34

cat <<EOF > android-cross.txt
[binaries]
ar = '$NDK_BIN/llvm-ar'
c = ['$NDK_BIN/aarch64-linux-android$API-clang', '--sysroot=$NDK_SYS']
cpp = ['$NDK_BIN/aarch64-linux-android$API-clang++', '--sysroot=$NDK_SYS']
c_ld = 'lld'
cpp_ld = 'lld'
strip = '$NDK_BIN/llvm-strip'
# pkgconfig를 아예 찾지 못하도록 설정
pkgconfig = 'false'

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
EOF

# 6. Meson 설정 및 빌드
OPTIM_FLAGS="-march=armv8-a"

echo "⚙️ Meson 설정 시작..."
rm -rf builddir
# wrap-mode=nodownload를 사용하여 이미 준비된 subprojects를 강제로 사용하게 함
meson setup builddir \
    --cross-file android-cross.txt \
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
    --wrap-mode=nodownload \
    --force-fallback-for=spirv-tools,spirv-headers,libdrm

echo "🏗️ 컴파일 시작..."
ninja -C builddir -j$(nproc)

echo "📦 결과물 정리..."
mkdir -p "$INSTALL_DIR"
DESTDIR="$INSTALL_DIR" ninja -C builddir install
cp builddir/src/freedreno/vulkan/libvulkan_freedreno.so "$INSTALL_DIR/vulkan.adreno.so"

echo "✅ 빌드 완료! 결과물: $INSTALL_DIR/vulkan.adreno.so"
