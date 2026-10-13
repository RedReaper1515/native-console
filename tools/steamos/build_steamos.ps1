<#
.SYNOPSIS
Builds a recompiled game as a Linux / SteamOS executable from Windows.

.DESCRIPTION
Runs tools/steamos/build_in_sdk.sh in Valve's Steam Runtime 3 "sniper" SDK
container through Docker Desktop. The executable targets the same runtime
Steam uses for native Linux games and runs on SteamOS (Steam Deck) directly.
Build caches live in the Docker volume "native-console-steamos".

.EXAMPLE
tools\steamos\build_steamos.ps1 -Rom "D:\roms\Tetris.gb" -Name tetris

.EXAMPLE
tools\steamos\build_steamos.ps1 -Rom "D:\roms\Minish Cap.gba" -Name minish_cap -Trace trace1.txt,trace2.txt

.NOTES
The console follows the ROM's extension unless -Console is given; the
recompiler for each console is in tools/consoles.sh.
#>
param(
    [Parameter(Mandatory = $true)] [string] $Rom,
    [Parameter(Mandatory = $true)] [string] $Name,
    [ValidateSet("", "gb", "gba", "snes", "nes")] [string] $Console = "",
    [string[]] $Trace = @(),
    # Destination for <Name>\<Name> and <Name>-steamos-x86_64.tar.gz.
    [string] $OutDir = "",
    # Extra arguments for the recompiler, e.g. -RecompilerArgs "--compact-resume".
    [string[]] $RecompilerArgs = @(),
    [string] $Image = "registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest"
)
# Not "Stop": Windows PowerShell turns docker's progress output on stderr into
# terminating errors when the caller redirects it. Exit codes are checked instead.
$ErrorActionPreference = "Continue"

$source = (Resolve-Path (Join-Path $PSScriptRoot "..\..") -ErrorAction Stop).Path
if (-not $OutDir) { $OutDir = Join-Path $source "output\steamos" }
New-Item -ItemType Directory -Force $OutDir -ErrorAction Stop | Out-Null
$OutDir = (Resolve-Path $OutDir -ErrorAction Stop).Path
$romItem = Get-Item -LiteralPath $Rom -ErrorAction Stop
# powershell -File passes "a","b" as the single string "a,b".
$Trace = @($Trace | ForEach-Object { $_ -split "," } | Where-Object { $_ })

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    throw "docker not found. Install Docker Desktop and start it."
}
docker info --format "{{.OSType}}" 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Docker is not running. Start Docker Desktop and try again." }
docker image inspect $Image 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "[steamos] pulling $Image (first run only)"
    docker pull $Image
    if ($LASTEXITCODE -ne 0) { throw "docker pull failed" }
}

$mounts = @(
    "-v", "${source}:/src:ro",
    "-v", "native-console-steamos:/work",
    "-v", "${OutDir}:/out",
    "-v", "$($romItem.DirectoryName):/in/rom:ro"
)
$scriptArgs = @("--rom", "/in/rom/$($romItem.Name)", "--name", $Name)
if ($Console) { $scriptArgs += @("--console", $Console) }
for ($i = 0; $i -lt $Trace.Count; $i++) {
    $t = Get-Item -LiteralPath $Trace[$i] -ErrorAction Stop
    $mounts += @("-v", "$($t.DirectoryName):/in/trace${i}:ro")
    $scriptArgs += @("--trace", "/in/trace$i/$($t.Name)")
}
if ($RecompilerArgs.Count -gt 0) { $scriptArgs += @("--") + $RecompilerArgs }

docker run --rm @mounts $Image bash /src/tools/steamos/build_in_sdk.sh @scriptArgs
if ($LASTEXITCODE -ne 0) { throw "SteamOS build failed (exit $LASTEXITCODE)" }
Write-Host "[steamos] output: $(Join-Path $OutDir $Name)"
