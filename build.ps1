$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$VcpkgRoot = $env:VCPKG_ROOT

if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    $VcpkgRoot = Join-Path $ProjectRoot ".deps\vcpkg"
    if (-not (Test-Path (Join-Path $VcpkgRoot ".git"))) {
        Write-Host "VCPKG_ROOT is not set; cloning vcpkg into .deps..."
        New-Item -ItemType Directory -Force (Split-Path -Parent $VcpkgRoot) | Out-Null
        git clone https://github.com/microsoft/vcpkg.git $VcpkgRoot
    }
}

$VcpkgExe = Join-Path $VcpkgRoot "vcpkg.exe"
if (-not (Test-Path $VcpkgExe)) {
    & (Join-Path $VcpkgRoot "bootstrap-vcpkg.bat") -disableMetrics
}

$BuildDir = Join-Path $ProjectRoot "build"
$Toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"

cmake -S $ProjectRoot -B $BuildDir -G "Visual Studio 17 2022" -A x64 `
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" `
    "-DVCPKG_TARGET_TRIPLET=x64-windows-static-md"

cmake --build $BuildDir --config Release --parallel

$DllPath = Join-Path $ProjectRoot "dist\NiNodeWatch\SKSE\Plugins\NiNodeWatch.dll"
if (-not (Test-Path $DllPath)) {
    throw "Build finished, but NiNodeWatch.dll was not produced."
}

$ZipPath = Join-Path $ProjectRoot "NiNodeWatch-0.1.1.zip"
Compress-Archive -Path (Join-Path $ProjectRoot "dist\NiNodeWatch\*") `
    -DestinationPath $ZipPath -Force

Write-Host ""
Write-Host "Ready: $DllPath" -ForegroundColor Green
Write-Host "MO2 archive: $ZipPath" -ForegroundColor Green
