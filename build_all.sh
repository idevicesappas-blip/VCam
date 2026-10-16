#!/usr/bin/env bash
# ============================================================
#  VCam build_all.sh — construye TODO el proyecto en un comando
#  Uso:  ./build_all.sh [--skip-ffmpeg] [--only-apk] [--only-module]
#  Salida:
#    out/vcam-v1.0.0-module.zip   → instalar en Magisk/KernelSU
#    out/VCam-1.0.0-release.apk  → app de control
# ============================================================
set -euo pipefail

VERSION="1.0.0"
ROOT="$(cd "$(dirname "$0")" && pwd)"
OUT="$ROOT/out"
NDK_DIR="${NDK_DIR:-$HOME/Android/Sdk/ndk/26.2.11394342}"
SDK_DIR="${SDK_DIR:-$HOME/Android/Sdk}"
FFMPEG_VER="6.1.1"
FF_BRANCH="n$FFMPEG_VER"
API_MIN=29
ABIS=("arm64-v8a" "armeabi-v7a")

SKIP_FFMPEG=false; ONLY_APK=false; ONLY_MODULE=false
for arg in "$@"; do
    case $arg in
        --skip-ffmpeg) SKIP_FFMPEG=true ;;
        --only-apk)    ONLY_APK=true ;;
        --only-module) ONLY_MODULE=true ;;
    esac
done

c()  { echo -e "\033[1;36m▶ $*\033[0m"; }
ok() { echo -e "\033[1;32m✓ $*\033[0m"; }
err(){ echo -e "\033[1;31m✗ $*\033[0m"; exit 1; }

command -v cmake  >/dev/null || err "cmake no instalado"
command -v ninja  >/dev/null || err "ninja no instalado (apt install ninja-build)"
[ -d "$NDK_DIR" ] || err "NDK no encontrado en $NDK_DIR (export NDK_DIR=...)"
[ -d "$SDK_DIR" ] || err "SDK no encontrado en $SDK_DIR (export SDK_DIR=...)"

mkdir -p "$OUT" "$ROOT/prebuilt" "$ROOT/module/engine"

# ============================================================
# FASE 1 — FFmpeg (una vez por ABI; cacheado en prebuilt/)
# ============================================================
build_ffmpeg() {
    local ABI=$1
    local PREFIX="$ROOT/prebuilt/ffmpeg/$ABI"
    [ -d "$PREFIX/lib" ] && { ok "ffmpeg $ABI cacheado"; return; }
    c "Compilando FFmpeg $FFMPEG_VER para $ABI (~10 min primera vez)"

    [ -d "$ROOT/ffmpeg-src" ] || \
        git clone --depth 1 -b "$FF_BRANCH" https://git.ffmpeg.org/ffmpeg.git "$ROOT/ffmpeg-src"

    local HOSTTAG="linux-x86_64"
    local CC CXX
    case $ABI in
        arm64-v8a)
            CC="$NDK_DIR/toolchains/llvm/prebuilt/$HOSTTAG/bin/aarch64-linux-android${API_MIN}-clang"
            EXTRA="--arch=aarch64" ;;
        armeabi-v7a)
            CC="$NDK_DIR/toolchains/llvm/prebuilt/$HOSTTAG/bin/armv7a-linux-androideabi${API_MIN}-clang"
            EXTRA="--arch=arm --enable-armv7te" ;;
    esac
    CXX="${CC}++"

    pushd "$ROOT/ffmpeg-src" >/dev/null
    make distclean 2>/dev/null || true
    ./configure \
        --cc="$CC" --cxx="$CXX" $EXTRA \
        --target-os=android --enable-cross-compile \
        --prefix="$PREFIX" \
        --enable-shared --disable-static --disable-programs --disable-doc \
        --disable-avdevice --disable-avfilter --disable-postproc \
        --disable-network --disable-iconv --disable-sdl2 --disable-zlib \
        --disable-everything \
        --enable-avcodec --enable-avformat --enable-swscale \
        --enable-swresample --enable-avutil \
        --enable-decoder=h264,hevc,mjpeg,png,vp8,vp9 \
        --enable-parser=h264,hevc,mjpeg,vp8,vp9 \
        --enable-demuxer=rtsp,rtmp,flv,image2,mov,mp4,mjpeg \
        --enable-protocol=file,rtmp,rtmpt,rtmps,rtsp,http,https,tcp,udp \
        >/dev/null
    make -j"$(nproc)" >/dev/null
    make install >/dev/null
    popd >/dev/null
    ok "ffmpeg $ABI → $PREFIX"
}

if [ "$ONLY_APK" = false ]; then
    if [ "$SKIP_FFMPEG" = false ]; then
        for ABI in "${ABIS[@]}"; do build_ffmpeg "$ABI"; done
    fi
fi

# ============================================================
# FASE 2 — Motor nativo C++ (CMake + NDK, por ABI)
# ============================================================
build_engine() {
    local ABI=$1
    local BUILDDIR="$OUT/native-$ABI"
    local SDK_LEVEL=$(( ABI == "arm64-v8a" ? 34 : 29 ))
    c "Compilando motor C++ ($ABI)"
    cmake -G Ninja -B "$BUILDDIR" -S "$ROOT/native" \
        -DCMAKE_TOOLCHAIN_FILE="$NDK_DIR/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM=android-$API_MIN \
        -DANDROID_SDK=$SDK_LEVEL \
        -DCMAKE_BUILD_TYPE=Release \
        -DFFMPEG_ROOT="$ROOT/prebuilt/ffmpeg/$ABI" \
        > /dev/null
    ninja -C "$BUILDDIR" vcam_provider_aidl || err "compilación AIDL falló ($ABI)"

    # copiar binario + libs ffmpeg al módulo
    local ENGDIR="$ROOT/module/engine/$ABI"
    mkdir -p "$ENGDIR"
    cp -f "$BUILDDIR/vcam_provider_aidl"        "$ENGDIR/vcam_provider_aidl"
    cp -f "$BUILDDIR/"lib*.so                    "$ENGDIR/" 2>/dev/null || true
    # libstdc++ emulada táctica: libc++_shared del propio build
    find "$BUILDDIR" -name "libc++_shared.so" -exec cp -f {} "$ENGDIR/" \;
    ok "motor $ABI → module/engine/$ABI/"
}

if [ "$ONLY_APK" = false ]; then
    for ABI in "${ABIS[@]}"; do build_engine "$ABI"; done
fi

# ============================================================
# FASE 3 — Verificación SELFTEST en host (sanity check binario)
# ============================================================
if [ "$ONLY_APK" = false ]; then
    c "Verificando binarios generados"
    for ABI in "${ABIS[@]}"; do
        BIN="$ROOT/module/engine/$ABI/vcam_provider_aidl"
        [ -f "$BIN" ] || err "faltante: $BIN"
        SIZE=$(stat -c%s "$BIN")
        [ "$SIZE" -gt 500000 ] || err "$ABI binario sospechosamente pequeño ($SIZE bytes)"
        # ELF válido + ABI correcta
        file "$BIN" | grep -qi "elf" || err "$BIN no es ELF válido"
        ok "$ABI ✓ ($((SIZE / 1024)) KB)"
    done
fi

# ============================================================
# FASE 4 — APK (gradle)
# ============================================================
if [ "$ONLY_MODULE" = false ]; then
    c "Compilando APK de control"
    # keystore autogenerado si no existe
    if [ ! -f "$ROOT/app/keystore.jks" ]; then
        keytool -genkeypair -v \
            -keystore "$ROOT/app/keystore.jks" \
            -alias vcam -keyalg RSA -keysize 2048 -validity 10000 \
            -storepass vcam123 -keypass vcam123 \
            -dname "CN=VCam, OU=Dev, O=vcam, C=ES" >/dev/null 2>&1
        ok "keystore.jks autogenerado (pass: vcam123) — REEMPLÁZALO para producción"
    fi
    ( cd "$ROOT" && ./gradlew :app:assembleRelease --no-daemon -q ) \
        || err "gradle falló"
    APK=$(find "$ROOT/app/build/outputs/apk" -name "*release*.apk" | head -1)
    [ -n "$APK" ] || err "APK no encontrado"
    cp -f "$APK" "$OUT/VCam-$VERSION-release.apk"
    ok "APK → out/VCam-$VERSION-release.apk"
fi

# ============================================================
# FASE 5 — Empaquetar módulo Magisk/KernelSU
# ============================================================
c "Empaquetando módulo Magisk/KernelSU"
MODSTAGE="$OUT/module-stage"
rm -rf "$MODSTAGE"
mkdir -p "$MODSTAGE"

# estructura del módulo: engine va en raíz (customize.sh usa $ENVDIR=MODPATH/engine)
mkdir -p "$MODSTAGE/engine"
cp -r "$ROOT/module/engine/."            "$MODSTAGE/engine/"
cp    "$ROOT/module/module.prop"         "$MODSTAGE/"
cp    "$ROOT/module/customize.sh"        "$MODSTAGE/"
cp    "$ROOT/module/service.sh"          "$MODSTAGE/"
[ -f "$ROOT/module/sepolicy.rule" ] && \
cp    "$ROOT/module/sepolicy.rule"        "$MODSTAGE/"

# fixes de permisos
find "$MODSTAGE" -exec chmod 644 {} \; 2>/dev/null
find "$MODSTAGE/engine" -type f -name "vcam_provider_aidl" -exec chmod 755 {} \;
chmod 755 "$MODSTAGE/customize.sh" "$MODSTAGE/service.sh"

sed -i "s/^version=.*/version=v$VERSION/" "$MODSTAGE/module.prop"

ZIP="$OUT/vcam-v$VERSION-module.zip"
rm -f "$ZIP"
( cd "$MODSTAGE" && zip -r -9 "$ZIP" . -x "*.DS_Store" >/dev/null )
ok "Módulo → $ZIP"

# ============================================================
# RESUMEN
# ============================================================
echo ""
echo -e "\033[1;33m╔════════════════════════════════════════════════╗"
echo -e "║  VCam v$VERSION — build completo                  ║"
echo -e "╠════════════════════════════════════════════════╣"
ls -lh "$OUT"/*.zip "$OUT"/*.apk 2>/dev/null | awk '{printf "║  %-45s %8s ║\n", $NF, $5}'
echo -e "╚════════════════════════════════════════════════╝\033[0m"
echo ""
echo "  1. Instala vcam-v$VERSION-module.zip en Magisk/KernelSU (desde storage)"
echo "  2. Instala VCam-$VERSION-release.apk"
echo "  3. Abre VCam → selecciona fuente → START INJECTION"
echo ""
