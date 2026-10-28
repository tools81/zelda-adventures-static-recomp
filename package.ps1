# Builds the distribution:
#   dist\<exe>-<version>\                  the portable folder (staged)
#   dist\<exe>-<version>-portable.zip      portable zip (also the update package)
#   dist\<exe>-<version>-setup.exe         installer (needs Inno Setup 6)
#
#   .\package.ps1                 build, then package
#   .\package.ps1 -SkipBuild      package the programs already built
#   .\package.ps1 -Publish        also create GitHub release v<version> with both
#                                 files (needs the gh CLI; repo = game.json "update_repo")
#
# The version is "version" in game.json. A release's zip must keep the name
# "*-portable.zip": that is the asset the game's update check downloads.
param([switch]$SkipBuild, [switch]$NoInstaller, [switch]$Publish)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
Set-Location $root

$cfg = Get-Content (Join-Path $root 'game.json') -Raw | ConvertFrom-Json
$exe = $cfg.exe_name
$ver = $cfg.version
$title = $cfg.title
if (-not $ver) { throw 'game.json has no "version"' }
Write-Host "Packaging $title $ver"

if (-not $SkipBuild) {
    # build.bat by its full path: some shells do not search the current folder
    & cmd.exe /c "`"$root\build.bat`""
    if ($LASTEXITCODE) { throw 'build failed' }
}
foreach ($e in "$exe.exe", "${exe}_hd.exe") {
    if (-not (Test-Path (Join-Path $root $e))) { throw "$e not found: build first" }
}

# ---- stage the portable folder
$dist = Join-Path $root 'dist'
$name = "$exe-$ver"
$stage = Join-Path $dist $name
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $stage 'rom') | Out-Null
Copy-Item (Join-Path $root "$exe.exe"), (Join-Path $root "${exe}_hd.exe") $stage
Copy-Item (Join-Path $root 'dist_files\update.ps1'), (Join-Path $root 'dist_files\game.ini'), (Join-Path $root 'dist_files\README.txt') $stage
Set-Content -Path (Join-Path $stage 'rom\PUT YOUR DISC IMAGE HERE.txt') -Encoding ascii -Value @(
    "Put your $title disc image (.cue and .bin) in this folder,",
    "or choose it when the game first starts (it is then remembered in game.ini).")
Set-Content -Path (Join-Path $stage 'VERSION.txt') -Encoding ascii -Value $ver
# a portable copy must not carry this machine's data
foreach ($junk in 'saves', 'update') { $p = Join-Path $stage $junk; if (Test-Path $p) { Remove-Item $p -Recurse -Force } }

# ---- portable zip (one top-level folder, so it can be unpacked anywhere)
$zip = Join-Path $dist "$name-portable.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
Write-Host "  $zip"

# ---- installer
$setup = Join-Path $dist "$name-setup.exe"
if (-not $NoInstaller) {
    $iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
              "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
              "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) {
        Write-Warning 'Inno Setup 6 not found (https://jrsoftware.org/isinfo.php): installer skipped'
    } else {
        & $iscc /Q "/DAppVersion=$ver" "/DAppTitle=$title" "/DExeName=$exe" "/DSourceDir=$stage" "/DOutputDir=$dist" (Join-Path $root 'installer\zelda.iss')
        if ($LASTEXITCODE) { throw 'Inno Setup failed' }
        Write-Host "  $setup"
    }
}

# ---- optional: publish as a GitHub release (outward-facing: only with -Publish)
if ($Publish) {
    $repo = $cfg.update_repo
    if (-not $repo) { throw 'game.json "update_repo" is empty: set it to the GitHub owner/name first' }
    $files = @($zip) + @(Get-Item $setup -ErrorAction SilentlyContinue | ForEach-Object FullName)
    & gh release create "v$ver" @files --repo $repo --title "$title $ver" --notes "$title $ver"
    if ($LASTEXITCODE) { throw 'gh release create failed' }
}
Write-Host 'Done.'
