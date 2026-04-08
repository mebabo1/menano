#!/bin/bash
set -e  # 에러 발생 시 즉시 중단

echo "🚀 Turnip (Mesa) glibc 성능 최적화 빌드 시작..."

# 1. 작업 디렉토리 설정
WORK_DIR=$(pwd)/turnip_workdir
INSTALL_DIR=$WORK_DIR/install
SOURCE_DIR=$WORK_DIR/mesa-src

mkdir -p $WORK_DIR
mkdir -p $INSTALL_DIR

# 2. Mesa 소스 코드 클론
if [ ! -d "$SOURCE_DIR" ]; then
    echo "📥 Mesa 소스 클론 중..."
    # 최신 Adreno 지원을 위해 업스트림 메인 브랜치 사용
    git clone --depth 1 https://gitlab.freedesktop.org/mesa/mesa.git $SOURCE_DIR
fi

cd $SOURCE_DIR

# --- [성능 향상을 위한 추가 섹션] ---
# 2-1. 최적화 패치 적용 (MR 37802: Adreno 성능 관련 최신 패치)
echo "🩹 성능 최적화 패치(MR 37802) 적용 중..."
curl -sL "https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/37802.patch" -o 37802.patch
patch -p1 --fuzz=4 < 37802.patch || echo "⚠️ 패치 적용 일부 실패 (이미 반영되었을 수 있음)"

# 2-2. 의존성 서브프로젝트 다운로드 (Vulkan 최신 기능 보장)
mkdir -p subprojects
cd subprojects
git clone --depth=1 https://github.com/KhronosGroup/SPIRV-Tools.git spirv-tools || true
git clone --depth=1 https://github.com/KhronosGroup/SPIRV-Headers.git spirv-headers || true
cd ..
# --------------------------------

echo "⚙️  Meson 구성 중 (최적화 옵션 포함)..."
# 성능 향상을 위해 -Doptimization=3 및 -Db_lto=true 추가
meson setup builddir \
    --prefix=/data/data/com.termux/files/usr/glibc \
    --libdir=lib \
    -Dbuildtype=release \
    -Doptimization=3 \
    -Db_lto=true \
    -Dplatforms=x11,wayland \
    -Dgallium-drivers=freedreno,zink,llvmpipe \
    -Dvulkan-drivers=freedreno \
    -Dfreedreno-kmds=kgsl \
    -Dvulkan-beta=true \
    -Degl=enabled \
    -Dgles1=enabled \
    -Dgles2=enabled \
    -Dopengl=true \
    -Dgbm=enabled \
    -Dglx=dri \
    -Dglvnd=enabled \
    -Dllvm=enabled \
    -Dshared-llvm=enabled \
    -Dvideo-codecs=all \
    -Dgallium-extra-hud=true \
    --force-fallback-for=spirv-tools,spirv-headers

# 4. 컴파일 및 설치
echo "🏗️  컴파일 시작 (nproc 사용)..."
ninja -C builddir -j$(nproc)

echo "📦 임시 디렉토리에 설치 중..."
DESTDIR=$INSTALL_DIR ninja -C builddir install

echo "✅ 빌드 완료!"
