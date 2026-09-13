<#
.SYNOPSIS
  Run the sanitizer validation, or report it honestly as unsupported.

.DESCRIPTION
  AddressSanitizer for MSVC x64 requires the "C++ AddressSanitizer" component of
  the Visual Studio C++ workload. This script probes for that runtime, builds and
  runs the whole suite under it when present, and otherwise runs the strongest
  available equivalent — the Debug configuration, which enables iterator
  debugging, the debug heap and /RTC1 runtime checks — and reports the gap.

.EXAMPLE
  pwsh -File scripts/validate-sanitizers.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

. (Join-Path $PSScriptRoot 'msvc-env.ps1') | Out-Null

$msvcLib = Join-Path (Split-Path -Parent (Split-Path -Parent (Get-Command cl.exe).Source)) 'lib\x64'
$asanRuntime = Get-ChildItem -LiteralPath $msvcLib -Filter 'clang_rt.asan_dynamic-x86_64.lib' -ErrorAction SilentlyContinue

if ($asanRuntime) {
    Write-Host "AddressSanitizer runtime found: $($asanRuntime.FullName)"
    & (Join-Path $PSScriptRoot 'build.ps1') -Configuration Release -Sanitizers -Fresh
    if ($LASTEXITCODE -ne 0) { throw 'sanitizer build failed' }
    $dir = Join-Path $root 'build-release-asan'
    & ctest --test-dir $dir --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'sanitizer test run failed' }
    Write-Host 'AddressSanitizer validation: REAL instrumented run, PASS'
    exit 0
}

Write-Host 'AddressSanitizer x64 runtime: UNSUPPORTED in this environment'
Write-Host "  probed: $msvcLib\clang_rt.asan_dynamic-x86_64.lib"
Write-Host '  Install the "C++ AddressSanitizer" component of the Visual Studio C++ workload to enable it.'
Write-Host '  Running the strongest available equivalent: Debug (iterator debugging, debug heap, /RTC1).'

& (Join-Path $PSScriptRoot 'build.ps1') -Configuration Debug -Fresh
if ($LASTEXITCODE -ne 0) { throw 'debug build failed' }
$debugDir = Join-Path $root 'build-debug'
& ctest --test-dir $debugDir --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'debug test run failed' }
Write-Host 'Debug-instrumented validation: PASS (AddressSanitizer: UNSUPPORTED)'
