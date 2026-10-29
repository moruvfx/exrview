# build.ps1 — build ExrView trên máy Windows của bạn.
# Cần: Visual Studio 2022 (workload "Desktop development with C++"), CMake, Qt 6 (MSVC 2022 64-bit),
#      vcpkg (git clone https://github.com/microsoft/vcpkg && .\vcpkg\bootstrap-vcpkg.bat),
#      Inno Setup 6 (tuỳ chọn, để tạo file cài đặt).
#
#   .\build.ps1 -QtDir C:\Qt\6.8.0\msvc2022_64 -Vcpkg C:\vcpkg
param(
    [Parameter(Mandatory = $true)][string]$QtDir,
    [Parameter(Mandatory = $true)][string]$Vcpkg,
    [string]$Triplet = "x64-windows-static-md",
    [switch]$NoInstaller
)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE="$Vcpkg\scripts\buildsystems\vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=$Triplet `
    -DCMAKE_PREFIX_PATH="$QtDir" `
    -DEXRVIEW_BUILD_SHELLEXT=ON
if ($LASTEXITCODE) { throw "configure lỗi" }

cmake --build build --config Release --parallel
if ($LASTEXITCODE) { throw "build lỗi" }

if (Test-Path dist) { Remove-Item dist -Recurse -Force }
cmake --install build --config Release --prefix dist
& "$QtDir\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler dist\ExrView.exe
Write-Host "Đã build xong: $PSScriptRoot\dist" -ForegroundColor Green

if (-not $NoInstaller) {
    $iscc = (Get-Command iscc -ErrorAction SilentlyContinue).Source
    if (-not $iscc) { $iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" }
    if (Test-Path $iscc) {
        & $iscc installer\ExrView.iss
        Write-Host "File cài đặt: $PSScriptRoot\installer\Output" -ForegroundColor Green
    } else {
        Write-Host "Không thấy Inno Setup — bỏ qua bước tạo installer." -ForegroundColor Yellow
        Write-Host "Có thể đăng ký preview thủ công (PowerShell Admin): regsvr32 `"$PSScriptRoot\dist\ExrViewShell.dll`""
    }
}
