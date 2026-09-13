<#
.SYNOPSIS
  Import the MSVC x64 developer environment into the current PowerShell process.

.DESCRIPTION
  Locates vcvars64.bat for the installed Visual Studio build tools, runs it, and
  copies the resulting environment (PATH, INCLUDE, LIB, and the SDK tool paths
  including rc.exe and mt.exe) into this process. Dot-source this script:

      . .\scripts\msvc-env.ps1

  Nothing is written to disk and no state outside the process is modified.
#>
[CmdletBinding()]
param(
    [string]$Vcvars
)

$ErrorActionPreference = 'Stop'

if (-not $Vcvars) {
    $candidates = @(
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat'),
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat'),
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'),
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'),
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat')
    )
    $Vcvars = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}

if (-not $Vcvars -or -not (Test-Path -LiteralPath $Vcvars)) {
    throw 'vcvars64.bat was not found. Install the MSVC v143 C++ build tools and the Windows SDK.'
}

$lines = & cmd.exe /c "call `"$Vcvars`" >nul 2>&1 && set"
if ($LASTEXITCODE -ne 0) {
    throw "vcvars64.bat failed with exit code $LASTEXITCODE"
}

foreach ($line in $lines) {
    $separator = $line.IndexOf('=')
    if ($separator -lt 1) { continue }
    $name = $line.Substring(0, $separator)
    $value = $line.Substring($separator + 1)
    if ($name -ieq 'Path') {
        Set-Item -Path 'env:Path' -Value $value
    } else {
        Set-Item -Path "env:$name" -Value $value
    }
}

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw 'cl.exe is still not on PATH after importing the MSVC environment.'
}
if (-not (Get-Command rc.exe -ErrorAction SilentlyContinue)) {
    throw 'rc.exe is still not on PATH after importing the MSVC environment.'
}
