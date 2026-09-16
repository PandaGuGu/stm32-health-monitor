# ============================================================
# 小组嵌入式 - STM32 健康监测(心率/血氧/温度/跌倒) 编译脚本
# 调用 Keil MDK UV4 命令行批量编译，产出 OBJ\IIC.hex
# 用法: powershell -ExecutionPolicy Bypass -File scripts\build.ps1
# 退出码: 0=成功(可含警告) 1=有警告 2=有错误 15=工程文件问题(常为uvprojx带BOM)
# ============================================================
param(
  [string]$ProjectDir = "$PSScriptRoot\..\firmware\MAX30102"
)

$ErrorActionPreference = "Stop"
$UV4   = "C:\Keil_v5\UV4\UV4.exe"
$proj  = Join-Path $ProjectDir "USER\IIC.uvprojx"
$log   = Join-Path $ProjectDir "build.log"
$hex   = Join-Path $ProjectDir "OBJ\IIC.hex"

if (-not (Test-Path $UV4))   { Write-Error "未找到 UV4: $UV4";  exit 1 }
if (-not (Test-Path $proj))  { Write-Error "未找到工程: $proj";  exit 1 }

Write-Host "=== 编译工程: $proj ==="
if (Test-Path $hex)   { Remove-Item -Force $hex -ErrorAction SilentlyContinue }   # 用 hex 是否生成判断成功
if (Test-Path $log)   { Remove-Item -Force $log -ErrorAction SilentlyContinue }

# 必须用 Start-Process -Wait，才能可靠捕获 UV4 的退出码
$proc = Start-Process -FilePath $UV4 -ArgumentList "-b", $proj, "-j0", "-o", $log -Wait -PassThru -NoNewWindow
$code = $proc.ExitCode

Write-Host ""
Write-Host "=== UV4 退出码: $code (0成功/1警告/2错误/15工程问题) ==="
if (Test-Path $log) { Get-Content $log | Select-Object -Last 25 }

$hexExists = Test-Path $hex
Write-Host ""
Write-Host ("hex 生成: {0}" -f $hexExists)
if ($hexExists) { Write-Host ("hex 路径: {0}" -f $hex) }

exit $code