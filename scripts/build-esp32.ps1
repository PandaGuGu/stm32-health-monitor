# ============================================================
# 小组嵌入式 - ESP32 信息节点 编译/烧录脚本
# 依赖: arduino-cli + esp32 core（见 firmware/esp32/README.md）
#
# 用法:
#   powershell -ExecutionPolicy Bypass -File scripts\build-esp32.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\build-esp32.ps1 -Upload -Port COM3
#
# 退出码: 0=成功 1=环境缺失 2=编译失败
# ============================================================
param(
  [string]$Port  = "",
  [switch]$Upload,
  [string]$Fqbn  = "esp32:esp32:esp32",
  [string]$Cli   = ""
)

$ErrorActionPreference = "Stop"

$sketchDir = Join-Path $PSScriptRoot "..\firmware\esp32\esp32_info_node"
$buildDir  = Join-Path $PSScriptRoot "..\firmware\esp32\build"

# ---- 1. 找 arduino-cli ----
if (-not $Cli) { $Cli = $env:ARDUINO_CLI }
if (-not $Cli) {
  $cmd = Get-Command arduino-cli -ErrorAction SilentlyContinue
  if ($cmd) { $Cli = $cmd.Source }
}
if (-not $Cli -or -not (Test-Path $Cli)) {
  Write-Host "未找到 arduino-cli。" -ForegroundColor Red
  Write-Host "  · 下载: https://github.com/arduino/arduino-cli/releases"
  Write-Host "  · 或设置环境变量 ARDUINO_CLI 指向 arduino-cli.exe"
  Write-Host "  · 首次还需: arduino-cli core install esp32:esp32 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json"
  exit 1
}

if (-not (Test-Path $sketchDir)) {
  Write-Host "未找到 sketch: $sketchDir" -ForegroundColor Red
  exit 1
}

# ---- 2. 编译 ----
Write-Host "=== 编译 $sketchDir ==="
Write-Host "FQBN: $Fqbn"
& $Cli compile --fqbn $Fqbn --build-path $buildDir --warnings default $sketchDir
if ($LASTEXITCODE -ne 0) {
  Write-Host "编译失败 (exit $LASTEXITCODE)" -ForegroundColor Red
  exit 2
}

$bin = Get-ChildItem $buildDir -Filter "esp32_info_node.ino.bin" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($bin) {
  $kb = [math]::Round($bin.Length / 1024, 1)
  Write-Host ""
  Write-Host "产物: $($bin.FullName) ($kb KB)" -ForegroundColor Green
  Write-Host "分区余量: 4MB Flash / default 分区表（APP 约 1.3MB 可用）"
}

# ---- 3. 烧录（可选）----
if ($Upload) {
  if (-not $Port) {
    Write-Host "需要 -Port COMx 才能烧录" -ForegroundColor Red
    exit 1
  }
  Write-Host ""
  Write-Host "=== 烧录到 $Port ==="
  & $Cli upload -p $Port --fqbn $Fqbn --input-dir $buildDir $sketchDir
  if ($LASTEXITCODE -ne 0) {
    Write-Host "烧录失败：检查 BOOT 按键/驱动/端口占用（串口监视器要先关）" -ForegroundColor Red
    exit 2
  }
  Write-Host "烧录完成。用 host/gateway 连接该串口即可看到 hello/telemetry 行。" -ForegroundColor Green
}

exit 0
