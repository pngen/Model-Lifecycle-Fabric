<#
.SYNOPSIS
  Fresh-clone closure: clone the repository into a clean path and prove the whole
  documented workflow from scratch.

.DESCRIPTION
  Clones the committed state into a directory outside the source tree, configures
  from nothing, builds Release and runs the whole suite, builds Debug and runs the
  whole suite, installs to a clean prefix, builds and runs the independent
  downstream consumer, runs every example, and runs the benchmarks.

.EXAMPLE
  pwsh -File scripts/validate-fresh-clone.ps1
#>
[CmdletBinding()]
param(
    [string]$CloneRoot,

    [string]$Source = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'

if (-not $CloneRoot) {
    $CloneRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('mlf-fresh-clone-' + [System.Guid]::NewGuid().ToString('N').Substring(0, 8))
}

. (Join-Path $PSScriptRoot 'msvc-env.ps1') | Out-Null

Write-Host "cloning $Source into $CloneRoot"
if (Test-Path -LiteralPath $CloneRoot) { Remove-Item -Recurse -Force -LiteralPath $CloneRoot }
& git clone --quiet --local $Source $CloneRoot
if ($LASTEXITCODE -ne 0) { throw "git clone failed with exit code $LASTEXITCODE" }

Push-Location $CloneRoot
try {
    $head = (& git rev-parse HEAD).Trim()
    Write-Host "fresh clone at $head"

    foreach ($configuration in @('Release', 'Debug')) {
        Write-Host "--- $configuration build"
        & pwsh -NoProfile -File (Join-Path $CloneRoot 'scripts/build.ps1') -Configuration $configuration -Fresh
        if ($LASTEXITCODE -ne 0) { throw "$configuration build failed" }

        Write-Host "--- $configuration tests"
        & ctest --test-dir (Join-Path $CloneRoot ("build-" + $configuration.ToLowerInvariant())) --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw "$configuration tests failed" }
    }

    Write-Host '--- install and downstream consumer'
    & pwsh -NoProfile -File (Join-Path $CloneRoot 'scripts/validate-install.ps1') -Configuration Release
    if ($LASTEXITCODE -ne 0) { throw 'install validation failed' }

    Write-Host '--- examples'
    $failures = 0
    foreach ($example in Get-ChildItem (Join-Path $CloneRoot 'build-release/mlf_example_*.exe') | Sort-Object Name) {
        & $example.FullName | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "example failed: $($example.Name)"
            $failures++
        }
    }
    if ($failures -ne 0) { throw "$failures example(s) failed" }

    Write-Host '--- benchmarks'
    & (Join-Path $CloneRoot 'build-release/mlf_benchmarks.exe') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'benchmarks failed' }

    Write-Host '--- sanitizer probe'
    & pwsh -NoProfile -File (Join-Path $CloneRoot 'scripts/validate-sanitizers.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'sanitizer validation failed' }

    $dirty = (& git status --porcelain | Measure-Object -Line).Lines
    if ($dirty -ne 0) { throw "fresh clone working tree is not clean ($dirty entries)" }

    Write-Host 'fresh-clone closure: PASS'
}
finally {
    Pop-Location
}
