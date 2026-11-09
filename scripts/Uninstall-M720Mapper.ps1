[CmdletBinding(SupportsShouldProcess)]
param()

$ErrorActionPreference = 'Stop'
$folder = Join-Path $env:LOCALAPPDATA 'M720Mapper'
$installed = Join-Path $folder 'M720Mapper.exe'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$runValue = '"{0}"' -f $installed
$entry = (Get-ItemProperty -LiteralPath $runKey -Name M720Mapper -ErrorAction SilentlyContinue).M720Mapper
if ($entry -and $entry -ne $runValue) { throw 'The startup entry belongs to a different Mapper installation.' }
if (!$PSCmdlet.ShouldProcess($folder, 'Stop and uninstall the native M720 Mapper')) { return }

if (Test-Path -LiteralPath $installed) {
    $directoryItem = Get-Item -LiteralPath $folder -Force
    $fileItem = Get-Item -LiteralPath $installed -Force
    if (($directoryItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
        ($fileItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $fileItem.PSIsContainer) {
        throw 'The installation paths are unexpected. No files were removed.'
    }
    $status = Start-Process -FilePath $installed -ArgumentList '--status' -NoNewWindow -Wait -PassThru
    if ($status.ExitCode -notin 0,1,3,4) { throw 'This executable did not respond as the native Mapper.' }
    $session = (Get-Process -Id $PID).SessionId
    $running = @(Get-CimInstance Win32_Process -Filter "Name='M720Mapper.exe'" |
        Where-Object { $_.SessionId -eq $session -and $_.ExecutablePath -eq $installed })
    if ($running.Count -gt 1) { throw 'Multiple installed Mapper processes found; files were preserved.' }
    if ($running.Count -eq 1) {
        $target = Get-Process -Id $running[0].ProcessId
        $stop = Start-Process -FilePath $installed -ArgumentList '--stop' -NoNewWindow -Wait -PassThru
        if ($stop.ExitCode -ne 0 -or !$target.WaitForExit(10000)) { throw 'Graceful stop failed; installation preserved.' }
    }
    Remove-Item -LiteralPath $installed
}
if ($entry -eq $runValue) { Remove-ItemProperty -LiteralPath $runKey -Name M720Mapper }
'Native Mapper removed. The log and any migration backups were preserved.'
