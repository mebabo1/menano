#!/bin/bash
set -e

echo "🚀 Turnip (Mesa) Termux/Android 최적화 빌드 시작..."

WORK_DIR="$(pwd)/turnip_workdir"
INSTALL_DIR="$WORK_DIR/install"
SOURCE_DIR="$WORK_DIR/mesa-src"
NDK_VER="android-ndk-r29"

mkdir -p "$WORK_DIR"
cd "$WORK_DIR"

# 1. NDK 다운로드 및 경로 설정
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

echo "📂 NDK 경로: $ANDROID_NDK_HOME"

# 2. 소스 코드 가져오기
if [ ! -d "$SOURCE_DIR" ]; then
    echo "📥 Mesa 소스 클론 중..."
    git clone --depth 1 https://github.com/lfdevs/mesa-for-android-container.git "$SOURCE_DIR"
fi

cd "$SOURCE_DIR"

# 3. [중요] Meson 크로스 컴파일 설정 파일 생성
# Meson이 NDK의 컴파일러를 인식하게 만드는 핵심 단계입니다.
NDK_BIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
NDK_SYS="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/sysroot"
API=34  # Android API Level

cat <<EOF > android-cross.txt
[binaries]
ar = '$NDK_BIN/llvm-ar'
c = ['$NDK_BIN/aarch64-linux-android$API-clang', '--sysroot=$NDK_SYS']
cpp = ['$NDK_BIN/aarch64-linux-android$API-clang++', '--sysroot=$NDK_SYS']
c_ld = 'lld'
cpp_ld = 'lld'
strip = '$NDK_BIN/llvm-strip'
pkg-config = 'false'

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
EOF

# 4. 소스 코드 패치
echo "🩹 메모리 핸들러 패치 적용 중..."
sed -i 's/typedef const native_handle_t\* buffer_handle_t;/typedef void\* buffer_handle_t;/g' include/android_stub/cutils/native_handle.h || true
sed -i 's/, hnd->handle/, (void \*)hnd->handle/g' src/util/u_gralloc/u_gralloc_fallback.c || true

# 5. 의존성 (SPIRV) 준비
mkdir -p subprojects && cd subprojects
git clone --depth=1 https://github.com/KhronosGroup/SPIRV-Tools.git spirv-tools || true
git clone --depth=1 https://github.com/KhronosGroup/SPIRV-Headers.git spirv-headers || true
cd ..

# 6. Meson 설정 (크로스 파일 적용)
OPTIM_FLAGS="-march=armv8-a"

echo "⚙️ Meson 설정 시작 (Cross-file 사용)..."
rm -rf builddir # 이전 실패한 설정이 있으면 삭제
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
    --wrap-mode=forcefallback \
    --force-fallback-for=spirv-tools,spirv-headers

# 7. 컴파일 및 설치
echo "🏗️ 컴파일 시작..."
ninja -C builddir -j$(nproc)

echo "📦 결과물 정리 중..."
DESTDIR="$INSTALL_DIR" ninja -C builddir install
cp builddir/src/freedreno/vulkan/libvulkan_freedreno.so "$INSTALL_DIR/vulkan.adreno.so"

echo "✅ 모든 빌드 작업이 완료되었습니다!"
echo "📍 결과물 위치: $INSTALL_DIR/vulkan.adreno.so"
