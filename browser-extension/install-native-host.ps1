param(
    [string]$ExtensionId
)
$ErrorActionPreference = "Stop"
$ExtensionRoot = $PSScriptRoot
$RepoRoot = Split-Path -Parent $ExtensionRoot

Write-Host ""
Write-Host "NextGen Chess - local engine setup" -ForegroundColor Cyan
Write-Host "Repository: $RepoRoot"

if (-not $ExtensionId) {
    $ExtensionId = Read-Host "Paste the extension ID shown at chrome://extensions"
}
if ($ExtensionId -notmatch "^[a-p]{32}$") {
    throw "The extension ID must be 32 lowercase letters from a to p. Load the unpacked extension first, then copy its ID."
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "CMake was not found. Install CMake and a C++20 compiler (Visual Studio 2022 Build Tools with Desktop development with C++), then rerun this script."
}

$BuildDir = Join-Path $RepoRoot "build"
Write-Host "Building the NextGen C++ engine..." -ForegroundColor Yellow
cmake -S $RepoRoot -B $BuildDir
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }
cmake --build $BuildDir --config Release --target chess_engine
if ($LASTEXITCODE -ne 0) { throw "NextGen engine build failed." }

$EngineCandidates = @(
    (Join-Path $BuildDir "Release\chess_engine.exe"),
    (Join-Path $BuildDir "chess_engine.exe"),
    (Join-Path $BuildDir "Debug\chess_engine.exe")
)
$Engine = $EngineCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $Engine) { throw "Build completed, but chess_engine.exe was not found in the expected build paths." }

$CscCandidates = @(
    "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe",
    "$env:WINDIR\Microsoft.NET\Framework\v4.0.30319\csc.exe"
)
$Csc = $CscCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $Csc) {
    throw "The .NET Framework C# compiler (csc.exe) was not found. Install Visual Studio Build Tools / .NET Framework developer tools, then rerun."
}

$HostDir = Join-Path $ExtensionRoot "native-host"
$HostExe = Join-Path $HostDir "NextGenNativeHost.exe"
$HostSource = Join-Path $HostDir "NextGenNativeHost.cs"
Write-Host "Compiling Chrome Native Messaging bridge..." -ForegroundColor Yellow
& $Csc /nologo /target:exe /reference:System.Web.Extensions.dll ("/out:" + $HostExe) $HostSource
if ($LASTEXITCODE -ne 0) { throw "Could not compile the native messaging host." }

$ManifestPath = Join-Path $HostDir "com.nextgen.chess_engine.json"
$NativeManifest = @{
    name = "com.nextgen.chess_engine"
    description = "Local NextGen Chess Engine bridge"
    path = $HostExe
    type = "stdio"
    allowed_origins = @("chrome-extension://$ExtensionId/")
}
$ManifestJson = $NativeManifest | ConvertTo-Json -Depth 5
[System.IO.File]::WriteAllText($ManifestPath, $ManifestJson, (New-Object System.Text.UTF8Encoding($false)))

$RegistryPath = "HKCU:\Software\Google\Chrome\NativeMessagingHosts\com.nextgen.chess_engine"
New-Item -Path $RegistryPath -Force | Out-Null
Set-Item -Path $RegistryPath -Value $ManifestPath

Write-Host ""
Write-Host "Setup complete." -ForegroundColor Green
Write-Host "Engine: $Engine"
Write-Host "Native host: $HostExe"
Write-Host "Open the extension and analyse a manually entered FEN."
Write-Host "The experimental NNUE option is off by default."
