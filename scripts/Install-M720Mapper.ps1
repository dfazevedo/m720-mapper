[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$Executable = (Join-Path (Split-Path $PSScriptRoot -Parent) 'M720Mapper.exe'),
    [switch]$RequireActive
)

$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this installer from a non-elevated PowerShell session.'
}
$source = (Resolve-Path -LiteralPath $Executable).ProviderPath
$folder = Join-Path $env:LOCALAPPDATA 'M720Mapper'
$installed = Join-Path $folder 'M720Mapper.exe'
$legacyLauncher = Join-Path $folder 'M720Mapper-Launcher.exe'
$legacyStop = Join-Path $folder 'Stop-M720Mapper.exe'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$runValue = '"{0}"' -f $installed
$legacyValue = '"{0}"' -f $legacyLauncher
$previousRun = (Get-ItemProperty -LiteralPath $runKey -Name M720Mapper -ErrorAction SilentlyContinue).M720Mapper
if ($previousRun -and $previousRun -notin @($runValue, $legacyValue)) {
    throw "An unrelated M720Mapper startup entry already exists: $previousRun"
}
if ($source -eq $installed) { throw 'Use the release executable outside the installation folder.' }
if (!$PSCmdlet.ShouldProcess($folder, 'Install native M720 Mapper and enable startup')) { return }

$oldFiles = @(@($installed, $legacyLauncher, $legacyStop) | Where-Object { Test-Path -LiteralPath $_ })
foreach ($path in @($folder) + @($oldFiles)) {
    if (!(Test-Path -LiteralPath $path)) { continue }
    $item = Get-Item -LiteralPath $path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
        ($item.PSIsContainer -ne ($path -eq $folder))) {
        throw "Unexpected installation path: $path"
    }
}

$current = @(Get-CimInstance Win32_Process -Filter "Name='M720Mapper.exe'" |
    Where-Object { $_.SessionId -eq (Get-Process -Id $PID).SessionId })
if ($current.Count -gt 1 -or ($current.Count -eq 1 -and $current[0].ExecutablePath -ne $installed)) {
    throw 'Stop the other M720 Mapper instance gracefully before installing.'
}
if ($current.Count -eq 1 -and !$previousRun) {
    throw 'An existing Mapper is running without a recognized startup entry. Stop it before installing.'
}

function Invoke-MapperCommand([string]$Path, [string]$Argument) {
    $parameters = @{ FilePath = $Path; Wait = $true; PassThru = $true; NoNewWindow = $true }
    if ($Argument) { $parameters.ArgumentList = $Argument }
    (Start-Process @parameters).ExitCode
}

function Wait-OldProcess([Diagnostics.Process]$Process) {
    if ($Process -and !$Process.WaitForExit(10000)) { throw 'Mapper has not exited; its files were preserved.' }
}

if ($current.Count -eq 1) {
    $previousProcess = Get-Process -Id $current[0].ProcessId
    $previousLauncher = $null
    if ($previousRun -eq $legacyValue) {
        $parentId = $current[0].ParentProcessId
        $parent = Get-CimInstance Win32_Process -Filter "ProcessId=$parentId"
        if ($parent.ExecutablePath -eq $legacyLauncher) {
            $previousLauncher = Get-Process -Id $parentId -ErrorAction SilentlyContinue
        }
        if (!(Test-Path -LiteralPath $legacyStop)) { throw 'The legacy graceful-stop helper is missing.' }
        $exit = Invoke-MapperCommand $legacyStop ''
    } else {
        $exit = Invoke-MapperCommand $installed '--stop'
    }
    if ($exit -ne 0) { throw "Graceful stop failed with code $exit. Installation not changed." }
    Wait-OldProcess $previousProcess
    Wait-OldProcess $previousLauncher
}

$backup = $null
$backupComplete = $false
$installChanged = $false
$newProcess = $null
try {
    New-Item -Path $folder -ItemType Directory -Force | Out-Null
    if ($oldFiles -or $previousRun) {
        $backup = Join-Path $folder ('backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
        New-Item -Path $backup -ItemType Directory | Out-Null
        foreach ($file in $oldFiles) {
            Copy-Item -LiteralPath $file -Destination $backup
            $copy = Join-Path $backup (Split-Path $file -Leaf)
            if ((Get-FileHash -LiteralPath $copy).Hash -ne (Get-FileHash -LiteralPath $file).Hash) {
                throw "The backup hash differs for $file."
            }
        }
        @{ RunValue = $previousRun; WasRunning = ($current.Count -eq 1) } |
            ConvertTo-Json | Set-Content -LiteralPath (Join-Path $backup 'startup.json') -Encoding UTF8
    }
    $backupComplete = $true
    $installChanged = $true
    Copy-Item -LiteralPath $source -Destination $installed -Force
    if ((Get-FileHash -LiteralPath $source).Hash -ne (Get-FileHash -LiteralPath $installed).Hash) {
        throw 'The installed executable hash differs from the release.'
    }
    $newProcess = Start-Process -FilePath $installed -WorkingDirectory $folder -WindowStyle Hidden -PassThru
    $ready = $false
    $startup = [Diagnostics.Stopwatch]::StartNew()
    while (!$ready -and $startup.Elapsed.TotalSeconds -lt 20) {
        if ($newProcess.WaitForExit(300)) { throw "The new Mapper exited with code $($newProcess.ExitCode). See Mapper.log." }
        $status = Invoke-MapperCommand $installed '--status'
        if ($status -eq 2) { throw 'The native Mapper status command failed.' }
        $ready = $status -eq 0 -or (!$RequireActive -and $status -eq 1)
    }
    if (!$ready) { throw 'The native Mapper did not reach the requested startup state. See Mapper.log.' }
    foreach ($file in @($legacyLauncher, $legacyStop)) {
        if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file }
    }
    New-ItemProperty -LiteralPath $runKey -Name M720Mapper -PropertyType String -Value $runValue -Force | Out-Null
    "Installed at $installed. PID $($newProcess.Id)."
    if ($backup) { "Previous installation backed up in $backup." }
}
catch {
    $failure = $_
    if ($newProcess -and !$newProcess.HasExited) {
        if ((Invoke-MapperCommand $installed '--stop') -ne 0 -or !$newProcess.WaitForExit(10000)) {
            throw "Install failed and the new process could not be stopped. Backup preserved at $backup. $failure"
        }
    }
    if ($backupComplete -and $installChanged) {
        foreach ($file in @($installed, $legacyLauncher, $legacyStop)) {
            $copy = if ($backup) { Join-Path $backup (Split-Path $file -Leaf) }
            if ($copy -and (Test-Path -LiteralPath $copy)) { Copy-Item -LiteralPath $copy -Destination $file -Force }
            elseif (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file }
        }
        if ($previousRun) {
            New-ItemProperty -LiteralPath $runKey -Name M720Mapper -PropertyType String -Value $previousRun -Force | Out-Null
        } else {
            Remove-ItemProperty -LiteralPath $runKey -Name M720Mapper -ErrorAction SilentlyContinue
        }
    }
    if ($current.Count -eq 1) {
        $restore = if ($previousRun -eq $legacyValue) { $legacyLauncher } else { $installed }
        Start-Process -FilePath $restore -WorkingDirectory $folder -WindowStyle Hidden
    }
    throw $failure
}
