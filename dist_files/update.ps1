# Installs a downloaded release over this copy of the game.
# Started by the game (portable_win32.c) after it downloaded update\package.zip;
# waits for the game to exit, replaces the program files, keeps the player's
# data (saves\, rom\, game.ini) and starts the new version.
param(
    [Parameter(Mandatory)] [string] $Zip,
    [Parameter(Mandatory)] [string] $Dir,
    [int] $WaitPid = 0,
    [string] $Relaunch = ""
)
$ErrorActionPreference = 'Stop'
$log = Join-Path $Dir 'update\update.log'
function Log($m) { Add-Content -Path $log -Value ("{0:u}  {1}" -f (Get-Date), $m) }
function Fail($m) {
    Log "FAILED: $m"
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.MessageBox]::Show("The update could not be installed:`n$m`n`nThe previous version is unchanged.", "Update") | Out-Null
    if ($Relaunch) { Start-Process -FilePath $Relaunch -ArgumentList '--no-update' }
    exit 1
}

try {
    Log "update from $Zip into $Dir"
    if ($WaitPid) {
        $p = Get-Process -Id $WaitPid -ErrorAction SilentlyContinue
        if ($p) { $p.WaitForExit(30000) | Out-Null }
    }
    $tmp = Join-Path $Dir 'update\extract'
    if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
    Expand-Archive -Path $Zip -DestinationPath $tmp -Force
    # the zip holds one top-level folder (Name-version\); accept a flat zip too
    $items = @(Get-ChildItem $tmp)
    $src = if ($items.Count -eq 1 -and $items[0].PSIsContainer) { $items[0].FullName } else { $tmp }
    if (-not (Get-ChildItem $src -Filter *.exe)) { Fail "the package contains no program" }
    $keep = @('saves', 'rom', 'game.ini', 'update')
    foreach ($item in Get-ChildItem $src) {
        if ($keep -contains $item.Name) { continue }
        $dest = Join-Path $Dir $item.Name
        # a running copy of update.ps1 can be replaced; retry briefly for locked exes
        for ($try = 0; $try -lt 10; $try++) {
            try { Copy-Item -Path $item.FullName -Destination $dest -Recurse -Force; break }
            catch { if ($try -eq 9) { throw }; Start-Sleep -Milliseconds 500 }
        }
        Log "  updated $($item.Name)"
    }
    Remove-Item $tmp -Recurse -Force
    Remove-Item $Zip -Force
    Log "done"
    if ($Relaunch) { Start-Process -FilePath $Relaunch }
}
catch { Fail $_.Exception.Message }
