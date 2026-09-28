# Builds build\Release\truefont.dll with CMake and the Ashita SDK at -Sdk. This is always the release build: TF_DEV is
# never defined here, and the check below stops the run if the DLL is not the release one.
param([Parameter(Mandatory = $true)][string]$Sdk)
$ErrorActionPreference = 'Stop'
Push-Location (Join-Path $PSScriptRoot '../..')
try {
    $env:ASHITA4_SDK_PATH = $Sdk
    cmake -S . -B build -G 'Visual Studio 17 2022' -A Win32 -DCMAKE_BUILD_TYPE=Release | Out-Host
    if ($LASTEXITCODE) { throw "CMake could not configure the build (exit $LASTEXITCODE)." }
    cmake --build build --config Release | Out-Host
    if ($LASTEXITCODE) { throw "The build failed (exit $LASTEXITCODE)." }
    # Only the release build carries this reply; a TF_DEV build does not.
    $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes((Resolve-Path 'build/Release/truefont.dll').Path))
    if (-not $text.Contains('the selftest is in the DEV build only.')) { throw 'build/Release/truefont.dll is not the release build (TF_DEV was defined).' }
} finally { Pop-Location }
