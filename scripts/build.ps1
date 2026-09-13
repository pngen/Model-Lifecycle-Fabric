<#
.SYNOPSIS
  Configure and build Model Lifecycle Fabric.

.EXAMPLE
  pwsh -File scripts/build.ps1 -Configuration Release
  pwsh -File scripts/build.ps1 -Configuration Debug -Sanitizers
  pwsh -File scripts/build.ps1 -Configuration Release -Fresh -Targets mlf_cli
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',

    [string]$BuildDir,

    [switch]$Sanitizers,

    [switch]$Fresh,

    [string[]]$Targets = @(),

    [switch]$ConfigureOnly,

    [string]$InstallPrefix,

    [switch]$BuildTests = $true,

    [switch]$Verbose2
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not $BuildDir) {
    $suffix = if ($Sanitizers) { '-asan' } else { '' }
    $BuildDir = Join-Path $root ("build-" + $Configuration.ToLowerInvariant() + $suffix)
}

. (Join-Path $PSScriptRoot 'msvc-env.ps1') | Out-Null

$arguments = @(
    '-S', $root,
    '-B', $BuildDir,
    '-G', 'Ninja',
    ("-DCMAKE_BUILD_TYPE=" + $Configuration),
    '-DCMAKE_C_COMPILER=cl',
    '-DCMAKE_CXX_COMPILER=cl',
    ("-DMLF_BUILD_TESTS=" + ($(if ($BuildTests) { 'ON' } else { 'OFF' }))),
    '-DMLF_BUILD_EXAMPLES=ON',
    '-DMLF_BUILD_BENCHMARKS=ON',
    '-DMLF_BUILD_CLI=ON',
    '-DMLF_WARNINGS_AS_ERRORS=ON'
)

if ($Sanitizers) { $arguments += '-DMLF_ENABLE_SANITIZERS=ON' }
if ($InstallPrefix) { $arguments += ("-DCMAKE_INSTALL_PREFIX=" + $InstallPrefix) }
if ($Fresh -and (Test-Path -LiteralPath $BuildDir)) {
    Remove-Item -Recurse -Force -LiteralPath $BuildDir
}

& cmake @arguments
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed with exit code $LASTEXITCODE" }

if ($ConfigureOnly) { return }

$buildArguments = @('--build', $BuildDir)
if ($Targets.Count -gt 0) {
    $buildArguments += '--target'
    $buildArguments += $Targets
}
& cmake @buildArguments
if ($LASTEXITCODE -ne 0) { throw "cmake build failed with exit code $LASTEXITCODE" }

if ($Verbose2) {
    Write-Host ("built " + $BuildDir)
}
