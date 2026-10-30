#!/usr/bin/env bash
# =============================================================
# 手工构建 app-vitals 的 APK（不依赖 gradle / Android Studio）
#
# 依赖：Android SDK build-tools 34.0.0 + platforms/android-34 + JDK 17+
# 用法：bash build-apk.sh
# 产物：app-vitals.apk（项目根目录）
#
# 说明：工程所在的父目录含中文（心率血），aapt2/d8/javac 在部分 Windows
# 环境下处理非 ASCII 路径会失败，因此统一复制到 $TEMP 下的英文路径构建。
# =============================================================
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ANDROID_HOME="${ANDROID_HOME:-$HOME/tools/android-sdk}"
JDK="${JDK_HOME:-/c/Program Files/Java/jdk-24}"
BT="${BT_DIR:-$ANDROID_HOME/build-tools/35.0.0}"
PLATFORM_JAR="$ANDROID_HOME/platforms/android-34/android.jar"
KS="$HOME/.android/debug.keystore"

[ -d "$BT" ] || { echo "找不到 build-tools: $BT"; exit 1; }
[ -f "$PLATFORM_JAR" ] || { echo "找不到 android.jar: $PLATFORM_JAR"; exit 1; }
[ -f "$KS" ] || { echo "找不到 debug.keystore: $KS"; exit 1; }
[ -x "$JDK/bin/javac.exe" ] || { echo "找不到 javac: $JDK"; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/vitals-apk-XXXXXX")"
mkdir -p "$WORK/gen" "$WORK/classes" "$WORK/dex"
echo "构建目录: $WORK"
cp -r "$HERE/app/src/main/res" "$WORK/res"
cp "$HERE/app/src/main/AndroidManifest.xml" "$WORK/AndroidManifest.xml"
cp -r "$HERE/app/src/main/java" "$WORK/src"

cd "$WORK"

echo "[1/6] aapt2 compile"
"$BT/aapt2.exe" compile --dir res -o res.zip

echo "[2/6] aapt2 link"
"$BT/aapt2.exe" link -o base.apk \
  -I "$PLATFORM_JAR" \
  --manifest AndroidManifest.xml \
  -R res.zip \
  --java gen \
  --min-sdk-version 24 \
  --target-sdk-version 34 \
  --version-code 1 \
  --version-name 1.0.0 \
  --auto-add-overlay

echo "[3/6] javac"
SRCS=$(find src gen -name '*.java')
"$JDK/bin/javac.exe" --release 8 -nowarn \
  -cp "$PLATFORM_JAR" -d classes $SRCS

echo "[4/6] d8"
CLZ=$(find classes -name '*.class')
"$BT/d8.bat" --lib "$PLATFORM_JAR" --min-api 24 --output dex $CLZ

echo "[5/6] 组装 + 对齐"
cp base.apk app-vitals.apk
python - <<'PY'
import zipfile, shutil, os
src = 'dex/classes.dex'
with zipfile.ZipFile('app-vitals.apk', 'a', zipfile.ZIP_DEFLATED) as z:
    z.write(src, 'classes.dex')
print('  classes.dex 已打入 APK')
PY
"$BT/zipalign.exe" -f -p 4 app-vitals.apk aligned.apk

echo "[6/6] apksigner"
"$BT/apksigner.bat" sign \
  --ks "$KS" \
  --ks-pass pass:android \
  --key-pass pass:android \
  --ks-key-alias androiddebugkey \
  --out signed.apk aligned.apk

cp signed.apk "$HERE/app-vitals.apk"
echo
echo "构建完成：$HERE/app-vitals.apk"
ls -la "$HERE/app-vitals.apk"
