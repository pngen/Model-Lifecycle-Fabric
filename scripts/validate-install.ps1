<#
.SYNOPSIS
  Validate the installed CMake package with an independent downstream consumer.

.DESCRIPTION
  Installs the library to a clean prefix outside the source tree, then configures,
  builds and runs examples/downstream-consumer against that prefix through
  find_package(ModelLifecycleFabric CONFIG REQUIRED).

.EXAMPLE
  pwsh -File scripts/validate-install.ps1 -Configuration Release
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [string]$BuildDir,

    [string]$Prefix,

    [string]$ConsumerDir
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not $BuildDir) {
    $BuildDir = Join-Path $root ("build-" + $Configuration.ToLowerInvariant())
}
if (-not $Prefix) {
    $Prefix = Join-Path $root ("install-" + $Configuration.ToLowerInvariant())
}
if (-not $ConsumerDir) {
    $ConsumerDir = Join-Path $root ("build-consumer-" + $Configuration.ToLowerInvariant())
}

. (Join-Path $PSScriptRoot 'msvc-env.ps1') | Out-Null

if (Test-Path -LiteralPath $Prefix) { Remove-Item -Recurse -Force -LiteralPath $Prefix }
if (Test-Path -LiteralPath $ConsumerDir) { Remove-Item -Recurse -Force -LiteralPath $ConsumerDir }

Write-Host "installing $Configuration to $Prefix"
& cmake --install $BuildDir --prefix $Prefix --config $Configuration
if ($LASTEXITCODE -ne 0) { throw "cmake --install failed with exit code $LASTEXITCODE" }

$packageConfig = Join-Path $Prefix 'lib/cmake/ModelLifecycleFabric/ModelLifecycleFabricConfig.cmake'
if (-not (Test-Path -LiteralPath $packageConfig)) {
    throw "the installed package config was not found at $packageConfig"
}

Write-Host "configuring the downstream consumer in $ConsumerDir"
& cmake -S (Join-Path $root 'examples/downstream-consumer') -B $ConsumerDir -G Ninja `
    ("-DCMAKE_BUILD_TYPE=" + $Configuration) `
    ("-DCMAKE_PREFIX_PATH=" + $Prefix) `
    '-DCMAKE_C_COMPILER=cl' '-DCMAKE_CXX_COMPILER=cl'
if ($LASTEXITCODE -ne 0) { throw "downstream configure failed with exit code $LASTEXITCODE" }

& cmake --build $ConsumerDir
if ($LASTEXITCODE -ne 0) { throw "downstream build failed with exit code $LASTEXITCODE" }

$consumer = Join-Path $ConsumerDir 'mlf_downstream_consumer.exe'
if (-not (Test-Path -LiteralPath $consumer)) { $consumer = Join-Path $ConsumerDir 'mlf_downstream_consumer' }
Write-Host "running $consumer"
& $consumer
if ($LASTEXITCODE -ne 0) { throw "downstream consumer reported failure (exit $LASTEXITCODE)" }

Write-Host 'install validation: PASS'
