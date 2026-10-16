#!/usr/bin/env bash
# Build VkWlfApp.apk (host-side compositor app) without Gradle/Soong.
#
#   tools/build_apk.sh [--abi x86_64|arm64-v8a] [--out DIR] [--install SERIAL]
#
# Toolchain discovery (first match wins):
#   ANDROID_NDK_ROOT   NDK with build/cmake/android.toolchain.cmake   (preferred)
#   ANDROID_SDK_ROOT   needs aapt2, d8, apksigner, zipalign, android.jar
#   ANDROID_JAR        explicit path to android.jar (optional)
#   VKWLF_WAYLAND_SRC  libwayland source tree   (NDK build compiles it from source)
#   VKWLF_LIBFFI_SRC   libffi source tree       (ditto; needs generated ffi.h for the ABI)
#
# Set ANDROID_SDK_ROOT / ANDROID_NDK_ROOT (see README).
# 
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ABI=x86_64; OUT="$ROOT/out/apk"; INSTALL_SERIAL=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --abi) ABI="$2"; shift 2;;
    --out) OUT="$2"; shift 2;;
    --install) INSTALL_SERIAL="$2"; shift 2;;
    *) echo "unknown arg $1"; exit 2;;
  esac
done
case "$ABI" in
  x86_64)    TRIPLE=x86_64-none-linux-android34; NDK_ARCH=x86_64-linux-android;;
  arm64-v8a) TRIPLE=aarch64-none-linux-android34; NDK_ARCH=aarch64-linux-android;;
  *) echo "unsupported --abi $ABI"; exit 2;;
esac

SDK="${ANDROID_SDK_ROOT:?set ANDROID_SDK_ROOT}"
NDK="${ANDROID_NDK_ROOT:?set ANDROID_NDK_ROOT}"
PT="$SDK/platform-tools"
BT="$(ls -d "$SDK"/build-tools/* 2>/dev/null | sort -V | tail -1 || true)"
tool() { # name -> path (build-tools, then platform-tools, then PATH)
  for d in "$BT" "$PT" "$SDK/tools"; do [[ -n "$d" && -x "$d/$1" ]] && { echo "$d/$1"; return; }; done
  command -v "$1" || { echo "missing tool: $1" >&2; exit 1; }
}
AAPT2="$(tool aapt2)"; D8="$(tool d8)"; APKSIGNER="$(tool apksigner)"
ZIPALIGN="$( [[ -x "$SDK/tools/zipalign.static" ]] && echo "$SDK/tools/zipalign.static" || tool zipalign)"
if [[ -z "${ANDROID_JAR:-}" ]]; then
  for c in "$SDK"/platforms/android-35/android.jar "$SDK"/platforms/android-34/android.jar "$SDK"/platforms/stable/_android.jar; do
    [[ -f "$c" ]] && { ANDROID_JAR="$c"; break; }
  done
fi
[[ -f "${ANDROID_JAR:-}" ]] || { echo "android.jar not found; set ANDROID_JAR" >&2; exit 1; }

echo "== native (ABI=$ABI)"
NBUILD="$OUT/native-$ABI"; mkdir -p "$NBUILD"
CMAKE_ARGS=(-S "$ROOT" -B "$NBUILD" -DCMAKE_BUILD_TYPE=Release -DANDROID=ON)
if [[ -f "$NDK/build/cmake/android.toolchain.cmake" ]]; then
  CMAKE_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI="$ABI" -DANDROID_PLATFORM=android-34)
else
  # Fallback when the NDK has no CMake toolchain file: drive clang with sysroot flags.
  CLANG="${CC:-$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang}"
  SYSROOT="$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot"
  CF="--target=$TRIPLE --sysroot=$SYSROOT"
  LF="$CF -L$SYSROOT/usr/lib/$NDK_ARCH/34 -L$SYSROOT/usr/lib/$NDK_ARCH -B$SYSROOT/usr/lib/$NDK_ARCH/34 -fuse-ld=lld -static-libstdc++"
  CMAKE_ARGS+=(-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_C_COMPILER="$CLANG" -DCMAKE_CXX_COMPILER="$CLANG++" -DCMAKE_ASM_COMPILER="$CLANG"
               -DCMAKE_C_FLAGS="$CF" -DCMAKE_ASM_FLAGS="$CF"
               -DCMAKE_CXX_FLAGS="$CF -stdlib=libc++ -I$SYSROOT/usr/include/c++/v1"
               -DCMAKE_SHARED_LINKER_FLAGS="$LF" -DCMAKE_EXE_LINKER_FLAGS="$LF")
fi
cmake "${CMAKE_ARGS[@]}" > "$NBUILD/configure.log" 2>&1 || { tail -30 "$NBUILD/configure.log"; exit 1; }
cmake --build "$NBUILD" --target vkwlf_jni -j"$(nproc)" > "$NBUILD/build.log" 2>&1 || { grep -E 'error' "$NBUILD/build.log" | head -20; exit 1; }

echo "== resources"
PK="$OUT/pkg"; rm -rf "$PK"; mkdir -p "$PK/res" "$PK/gen" "$PK/classes" "$PK/lib/$ABI"
"$AAPT2" compile --dir "$ROOT/res" -o "$PK/res.zip"
"$AAPT2" link -o "$PK/unaligned.apk" -I "$ANDROID_JAR" --manifest "$ROOT/AndroidManifest.xml" \
  -A "$ROOT/assets" --java "$PK/gen" --auto-add-overlay "$PK/res.zip"

echo "== java"
javac --release 11 -classpath "$ANDROID_JAR" -d "$PK/classes" \
  $(find "$PK/gen" "$ROOT/java" -name '*.java')
"$D8" --release --min-api 34 --output "$PK" $(find "$PK/classes" -name '*.class')

echo "== package"
cp "$NBUILD/libvkwlf_jni.so" "$PK/lib/$ABI/"
( cd "$PK" && zip -q -u -0 unaligned.apk classes.dex "lib/$ABI/libvkwlf_jni.so" )
"$ZIPALIGN" -f 4 "$PK/unaligned.apk" "$PK/aligned.apk"
KS="$OUT/debug.keystore"
[[ -f "$KS" ]] || keytool -genkeypair -v -keystore "$KS" -storepass android -keypass android -alias androiddebugkey \
  -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
"$APKSIGNER" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android --out "$OUT/VkWlfApp.apk" "$PK/aligned.apk"
rm -f "$OUT/VkWlfApp.apk.idsig"
echo "APK: $OUT/VkWlfApp.apk"

if [[ -n "$INSTALL_SERIAL" ]]; then
  ADB="$(command -v adb || echo "$PT/adb")"
  "$ADB" -s "$INSTALL_SERIAL" install -r "$OUT/VkWlfApp.apk"
fi
