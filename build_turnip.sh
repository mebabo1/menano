#!/bin/bash
set -e  # 에러 발생 시 즉시 중단

echo "🚀 Turnip (Mesa) glibc 빌드 시작..."

# 1. 작업 디렉토리 설정
WORK_DIR=$(pwd)/turnip_workdir
INSTALL_DIR=$WORK_DIR/install
SOURCE_DIR=$WORK_DIR/mesa-src

mkdir -p $WORK_DIR
mkdir -p $INSTALL_DIR

# 2. Mesa 소스 코드 클론
if [ ! -d "$SOURCE_DIR" ]; then
    echo "📥 Mesa 소스 클론 중..."
    git clone --depth 1 https://gitlab.freedesktop.org/mesa/mesa.git $SOURCE_DIR
fi

cd $SOURCE_DIR

echo "⚙️  Meson 구성 중..."
meson setup builddir \
    --prefix=/data/data/com.termux/files/usr/glibc \
    --libdir=lib \
    -Dbuildtype=release \
    -Dplatforms=x11,wayland \
    -Dgallium-drivers=freedreno,zink,llvmpipe \
    -Dvulkan-drivers=freedreno \
    -Dfreedreno-kmds=kgsl \
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
    -Dgallium-extra-hud=true

# 4. 컴파일 및 설치
echo "🏗️  컴파일 시작 (nproc 사용)..."
ninja -C builddir -j$(nproc)

echo "📦 임시 디렉토리에 설치 중..."
# 설치 시점에 DESTDIR 환경 변수를 사용하여 빌드 결과물을 $INSTALL_DIR로 보냅니다.
DESTDIR=$INSTALL_DIR ninja -C builddir install

echo "✅ 빌드 완료!"
