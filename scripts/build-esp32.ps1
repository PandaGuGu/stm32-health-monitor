# ============================================================
# 小组嵌入式 - ESP32 信息节点 编译/烧录脚本
# 依赖: arduino-cli + esp32 core（见 firmware/esp32/README.md）
#
# 用法:
#   powershell -ExecutionPolicy Bypass -File scripts\build-esp32.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\build-esp32.ps1 -Upload -Port COM12
#   powershell -ExecutionPolicy Bypass -File scripts\build-esp32.ps1 -Define ENABLE_BLE=1
#   powershell -ExecutionPolicy Bypass -File scripts\build-esp32.ps1 -Define ENABLE_BLE=1
#
# 退出码: 0=成功 1=环境缺失 2=编译失败
#
# 关于路径：仓库父目录含中文（心率血），xtensa-g++ 在中文路径下会静默崩溃
# （只报 "Error during build: exit status 1"，没有任何错误行）。因此先把
# sketch 复制到 %TEMP%\esp32build 这个纯英文目录再编译，与 app-vitals 的
# build-apk.sh 用同一套办法。
# ============================================================
param(
  [string]$Port  = "",
  [switch]$Upload,
  [string]$Fqbn  = "esp32:esp32:esp32:PartitionScheme=huge_app",
  [string]$Cli   = "",
  [string[]]$Define = @()
)

$ErrorActionPreference = "Stop"

# 控制台/重定向统一按 UTF-8 输出，否则中文提示在 GBK 环境下会变成乱码
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

$sketchDir = Join-Path $PSScriptRoot "..\firmware\esp32\esp32_info_node"
$workRoot  = Join-Path $env:TEMP "esp32build"
$sketchOut = Join-Path $workRoot "esp32_info_node"
$buildDir  = Join-Path $workRoot "build"

# ---- 1. 找 arduino-cli ----
if (-not $Cli) { $Cli = $env:ARDUINO_CLI }
if (-not $Cli) {
  $cmd = Get-Command arduino-cli -ErrorAction SilentlyContinue
  if ($cmd) { $Cli = $cmd.Source }
if (-not $Cli) {
  # 常见安装位置兜底，省得每次都要配 PATH
  $guess = @(
    (Join-Path $env:USERPROFILE "tools\arduino-cli\arduino-cli.exe"),
    "C:\Program Files\Arduino CLI\arduino-cli.exe",
    (Join-Path $env:LOCALAPPDATA "Arduino15\arduino-cli.exe")
  )
  foreach ($g in $guess) {
    if (Test-Path $g) { $Cli = $g; break }
  }
}
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

# ---- 2. 复制到纯英文路径 ----
if (Test-Path $sketchOut) { Remove-Item $sketchOut -Recurse -Force }
New-Item -ItemType Directory -Path $sketchOut -Force | Out-Null
Copy-Item (Join-Path $sketchDir "*") $sketchOut -Recurse -Force
Write-Host "=== 编译源: $sketchOut ==="

# ---- 3. 编译 ----
Write-Host "FQBN: $Fqbn"
if ($Define.Count -gt 0) { Write-Host "宏: $($Define -join ', ')" }

$extra = @("--build-path", $buildDir, "--warnings", "default")
if ($Define.Count -gt 0) {
  $flags = ($Define | ForEach-Object { "-D$_" }) -join " "
  $extra += "--build-property"
  $extra += "compiler.cpp.extra_flags=$flags"
}

& $Cli compile --fqbn $Fqbn @extra $sketchOut
if ($LASTEXITCODE -ne 0) {
  Write-Host "编译失败 (exit $LASTEXITCODE)" -ForegroundColor Red
  Write-Host "  若报 'text section exceeds available space'：BLE 栈让固件超过 1.3MB，" -ForegroundColor Yellow
  Write-Host "  需要 -Fqbn esp32:esp32:esp32:PartitionScheme=huge_app（默认已是）" -ForegroundColor Yellow
  exit 2
}

$bin = Get-ChildItem $buildDir -Filter "esp32_info_node.ino.bin" -ErrorAction SilentlyContinue |
       Select-Object -First 1
if ($bin) {
  $kb = [math]::Round($bin.Length / 1024, 1)
  Write-Host ""
  Write-Host "产物: $($bin.FullName) ($kb KB)" -ForegroundColor Green
  Write-Host "分区: $Fqbn"
}

# ---- 4. 烧录（可选）----
if ($Upload) {
  if (-not $Port) {
    Write-Host "需要 -Port COMx 才能烧录" -ForegroundColor Red
    exit 1
  }
  Write-Host ""
  Write-Host "=== 烧录到 $Port ==="
  & $Cli upload -p $Port --fqbn $Fqbn --input-dir $buildDir $sketchOut
  if ($LASTEXITCODE -ne 0) {
    Write-Host "烧录失败：检查 BOOT 按键/驱动/端口占用（串口监视器要先关）" -ForegroundColor Red
    exit 2
  }
  Write-Host "烧录完成。用 host/gateway 连接该串口即可看到 hello/telemetry 行。" -ForegroundColor Green
}

exit 0
