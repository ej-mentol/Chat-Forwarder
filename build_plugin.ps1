# Скрипт сборки и упаковки ChatForwarder (Автономная версия)
# Использование:
# .\build_plugin.ps1                  - Просто собрать (Release + AVX2)
# .\build_plugin.ps1 -Package         - Собрать и создать ZIP архивы с версией
# .\build_plugin.ps1 -CopyTo "C:\Path" - Собрать и скопировать в игру

param (
    [switch]$Package = $false,
    [string]$CopyTo = ""
)

$ErrorActionPreference = "Stop"
$projectDir = $PSScriptRoot
$buildsDir = "$projectDir\Builds"

# Calculate SolutionDir (Assuming structure: Root -> Plugins -> ChatForwarder)
# We need to go up two levels from ChatForwarder to reach Root
$solutionDir = Resolve-Path "$projectDir\..\..\"
# Ensure it ends with a backslash for MSBuild
$solutionDirStr = "$solutionDir\"

# === 0. Поиск версии в коде ===
$version = "unknown"
$versionFile = "$projectDir\plugins.cpp"
if (Test-Path $versionFile) {
    $content = Get-Content $versionFile -Raw
    if ($content -match 'return\s+\"([0-9\.]+)\"\s*;') {
        $version = $matches[1]
        Write-Host "Detected Plugin Version: $version" -ForegroundColor Cyan
    }
}

# === 1. Поиск MSBuild ===
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Write-Error "Visual Studio Installer not found!" }
$msbuildPath = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1
if (-not $msbuildPath) { Write-Error "MSBuild.exe not found!" }

# === 2. Сборка ===
$projectFile = "$projectDir\ChatForwarder.vcxproj"
$platform = "Win32"
$configurations = @("Release", "Release_AVX2")

foreach ($config in $configurations) {
    Write-Host "`n=== Building $config ($platform) ===" -ForegroundColor Yellow
    
    # We explicitly pass SolutionDir so imports work correctly without a .sln file
    $argsBuilder = @(
        $projectFile,
        "/t:Rebuild",
        "/p:Configuration=$config",
        "/p:Platform=$platform",
        "/p:SolutionDir=$solutionDirStr",
        "/m",
        "/v:minimal",
        "/nologo"
    )
    & $msbuildPath $argsBuilder
    if ($LASTEXITCODE -ne 0) { Write-Error "Build failed for $config" }
}

# === 3. Автокопирование ===
if (-not [string]::IsNullOrWhiteSpace($CopyTo)) {
    if (Test-Path $CopyTo) {
        Write-Host "`nCopying DLLs to: $CopyTo" -ForegroundColor Magenta
        $dlls = @("Release\ChatForwarder.dll", "Release_AVX2\ChatForwarder_AVX2.dll")
        foreach ($dll in $dlls) {
            $src = "$projectDir\$dll"
            if (Test-Path $src) { Copy-Item $src -Destination $CopyTo -Force; Write-Host "Copied: $(Split-Path $src -Leaf)" }
        }
    }
}

# === 4. Упаковка ===
if ($Package) {
    Write-Host "`nPackaging Release v$version..." -ForegroundColor Cyan
    if (-not (Test-Path $buildsDir)) { New-Item -ItemType Directory -Path $buildsDir | Out-Null }
    $zipPath = "$buildsDir\ChatForwarder_v$version.zip"
    $tempZipDir = "$buildsDir\temp_pack"
    if (Test-Path $tempZipDir) { Remove-Item $tempZipDir -Recurse -Force }
    New-Item -ItemType Directory -Path $tempZipDir | Out-Null
    
    Get-ChildItem -Path "$projectDir\Release", "$projectDir\Release_AVX2" -Filter "*.dll" | Copy-Item -Destination $tempZipDir
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    Compress-Archive -Path "$tempZipDir\*" -DestinationPath $zipPath
    Remove-Item $tempZipDir -Recurse -Force
    Write-Host "Package created: $zipPath" -ForegroundColor Green
}