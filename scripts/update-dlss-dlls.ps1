#Requires -Version 5.1
$ErrorActionPreference = 'Stop'

# Fetches the pinned DLSS / NGX DLLs into dlss_override/ at repo root.
# meson.build prefers dlss_override/ over the packman-shipped NGX DLLs when
# this dir is populated, so re-run this after editing scripts/dlss-pins.json.
# [nfs2se] each DLL is fetched on its own from NVIDIA's public DLSS SDK repo and checked against its pinned sha256.

$scriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot    = Split-Path -Parent $scriptDir
$pinFile     = Join-Path $scriptDir 'dlss-pins.json'
$overrideDir = Join-Path $repoRoot  'dlss_override'

$pin = Get-Content -Raw -LiteralPath $pinFile | ConvertFrom-Json
New-Item -ItemType Directory -Force -Path $overrideDir | Out-Null

foreach ($f in $pin.files) {
    $dest     = Join-Path $overrideDir $f.name
    $expected = $f.sha256.ToUpperInvariant()
    if ((Test-Path -LiteralPath $dest) -and
        ((Get-FileHash -Algorithm SHA256 -LiteralPath $dest).Hash.ToUpperInvariant() -eq $expected)) {
        Write-Host "$($f.name) already at $($pin.release_tag)"
        continue
    }
    Write-Host "Fetching $($f.url)"
    $pp = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'
    try {
        Invoke-WebRequest -Uri $f.url -OutFile $dest -UseBasicParsing `
            -MaximumRedirection 5 -UserAgent 'dxvk-remix-dlss-bump/1.0'
    } finally { $ProgressPreference = $pp }
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $dest).Hash.ToUpperInvariant()
    if ($actual -ne $expected) {
        Remove-Item -LiteralPath $dest -Force
        throw "SHA-256 mismatch for $($f.name) ($($pin.release_tag)): expected $expected, got $actual. File deleted."
    }
}

Write-Host "Installed DLSS DLLs $($pin.release_tag) -> $overrideDir"
Write-Host "Re-run meson configure (or rtx-build) to pick up the new DLLs."
