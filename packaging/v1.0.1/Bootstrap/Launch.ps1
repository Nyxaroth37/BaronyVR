Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()
$ErrorActionPreference='Stop'
try {
    $bootstrap=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'bootstrap-manifest.json') -Raw | ConvertFrom-Json
    foreach ($entry in $bootstrap.files) {
        if ([string]$entry.name -notmatch '^[A-Za-z0-9_.-]+$' -or
            (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $entry.name) -Algorithm SHA256).Hash -ne $entry.sha256) {
            throw 'Launcher files changed or are incomplete. Run the beta installer again.'
        }
    }
    . (Join-Path $PSScriptRoot 'UpdateCore.ps1')
    . (Join-Path $PSScriptRoot 'UiLoop.ps1')
    $script:ManagedRoot=Split-Path -Parent $PSScriptRoot
    $script:State=Read-Json (Join-Path $script:ManagedRoot 'active.json')
    $null=Verify-Record $script:ManagedRoot $script:State.active
} catch {
    [Windows.Forms.MessageBox]::Show($_.Exception.Message,'Barony VR','OK','Error') | Out-Null
    exit 1
}
$script:Worker=$null; $script:WorkerMode=''; $script:Candidate=$null; $script:Game=$null
$script:Mutex=New-Object Threading.Mutex($false, 'Local\BaronyVR-LauncherV1')
try { $script:OwnMutex=$script:Mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $script:OwnMutex=$true }
if (!$script:OwnMutex) {
    [Windows.Forms.MessageBox]::Show('Barony VR launcher is already open.','Barony VR','OK','Information') | Out-Null
    $script:Mutex.Dispose()
    exit 0
}
$form=New-Object Windows.Forms.Form
$form.Text='Barony VR | Launcher'
$form.ClientSize=New-Object Drawing.Size(620,355)
$form.StartPosition='CenterScreen'; $form.FormBorderStyle='FixedDialog'; $form.MaximizeBox=$false
$form.Font=New-Object Drawing.Font('Segoe UI',10)
$form.BackColor=[Drawing.Color]::FromArgb(27,30,36); $form.ForeColor=[Drawing.Color]::WhiteSmoke
function Ui-Label([string]$Text,[int]$Y,[int]$Height) {
    $label=New-Object Windows.Forms.Label; $label.Text=$Text
    $label.Location=New-Object Drawing.Point(28,$Y); $label.Size=New-Object Drawing.Size(564,$Height)
    $form.Controls.Add($label); return $label
}
$title=Ui-Label 'BARONY VR' 24 42; $title.Font=New-Object Drawing.Font('Segoe UI',23,[Drawing.FontStyle]::Bold)
$versionLabel=Ui-Label ([string]$script:State.active.version) 80 26
$status=Ui-Label 'Checking GitHub for published beta updates...' 120 58
$status.ForeColor=[Drawing.Color]::LightSteelBlue
$null=Ui-Label 'Calibration and saves stay in your retail Barony folder.' 183 26
function Ui-Button([string]$Text,[int]$X,[int]$Y,[int]$Width) {
    $button=New-Object Windows.Forms.Button; $button.Text=$Text
    $button.Location=New-Object Drawing.Point($X,$Y);$button.Size=New-Object Drawing.Size($Width,38)
    $form.Controls.Add($button);return $button
}
$play=Ui-Button 'Play without updating' 28 232 210
$update=Ui-Button 'Check for updates' 255 232 337
$rollback=Ui-Button 'Use previous version' 28 291 210
$releaseLink=Ui-Button 'Release notes' 255 291 155
$folderButton=Ui-Button 'Open game folder' 425 291 167
$script:ResultFile='';$script:RequestFile=''
$script:WorkDirectory=''
function Refresh-State {
    $script:State=Read-Json (Join-Path $script:ManagedRoot 'active.json')
    $versionLabel.Text=[string]$script:State.active.version
    $rollback.Enabled=($null -ne $script:State.previous)
}
function Stop-Worker {
    if ($script:Worker) {
        if (!$script:Worker.HasExited) { $script:Worker.Kill(); $script:Worker.WaitForExit() }
        $script:Worker.Dispose(); $script:Worker=$null
    }
    if ($script:WorkDirectory -and (Test-Path -LiteralPath $script:WorkDirectory)) {
        try { Remove-ManagedDirectory $script:ManagedRoot 'Work' $script:WorkDirectory } catch {}
        $script:WorkDirectory=''
    }
}
function Start-Worker([string]$Mode) {
    Stop-Worker
    $directory=New-ManagedDirectory $script:ManagedRoot 'Work';$script:WorkDirectory=$directory
    $script:ResultFile=Join-Path $directory ([Guid]::NewGuid().ToString('N')+'.json')
    $info=New-Object Diagnostics.ProcessStartInfo
    $info.FileName=Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $workerScript='CheckUpdates.ps1';$extra=''
    if ($Mode -eq 'apply') {
        $workerScript='ApplyUpdate.ps1'
        $script:RequestFile=Join-Path $directory ([Guid]::NewGuid().ToString('N')+'.json')
        Atomic-Json $script:RequestFile $script:Candidate.update
        $extra=' -RequestFile "'+$script:RequestFile+'"'
    }
    $info.Arguments='-NoProfile -ExecutionPolicy Bypass -File "'+(Join-Path $PSScriptRoot $workerScript)+'" -ManagedRoot "'+$script:ManagedRoot+'" -OutputFile "'+$script:ResultFile+'"'+$extra
    $info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $script:WorkerMode=$Mode;$script:Worker=[Diagnostics.Process]::Start($info)
    $update.Enabled=$false
    if ($Mode -eq 'apply') { $play.Enabled=$false;$rollback.Enabled=$false;$status.Text='Downloading and verifying the update. Your installed version remains available.' }
    else { $status.Text='Checking published releases... You can play now without waiting.' }
}
function Play-Game {
    Stop-Worker
    try {
        Refresh-State
        $root=Verify-Record $script:ManagedRoot $script:State.active
        if (Get-Process -Name barony -ErrorAction SilentlyContinue) { throw 'Barony is already running.' }
        $gameScript='LaunchGame.ps1';if ($script:State.active.sequence -eq 1) { $gameScript='Launch.ps1' }
        $info=New-Object Diagnostics.ProcessStartInfo
        $info.FileName=Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
        $info.Arguments='-NoProfile -ExecutionPolicy Bypass -STA -File "'+(Join-Path $root $gameScript)+'"'
        $info.WorkingDirectory=$root;$info.UseShellExecute=$false;$info.CreateNoWindow=$true
        $script:Game=[Diagnostics.Process]::Start($info)
        $form.Hide()
    } catch {
        $status.Text=$_.Exception.Message;$update.Enabled=$true
        [Windows.Forms.MessageBox]::Show($_.Exception.Message,'Barony VR','OK','Error') | Out-Null
    }
}
$play.Add_Click({ Play-Game })
$update.Add_Click({
    if ($script:Candidate) {
        if ($script:Candidate.update.min_launcher -gt 1) { Start-Process 'https://github.com/Nyxaroth37/BaronyVR/releases';return }
        Start-Worker 'apply'
    } else { Start-Worker 'check' }
})
$rollback.Add_Click({
    try {
        if (Get-Process -Name barony -ErrorAction SilentlyContinue) { throw 'Close Barony before switching versions.' }
        Stop-Worker
        Rollback-Version $script:ManagedRoot
        Refresh-State;$script:Candidate=$null;$update.Enabled=$true;$update.Text='Check for updates'
        $status.Text='Switched to the previous version. Calibration and saves were preserved.'
    } catch { [Windows.Forms.MessageBox]::Show($_.Exception.Message,'Barony VR','OK','Error') | Out-Null }
})
$releaseLink.Add_Click({ Start-Process 'https://github.com/Nyxaroth37/BaronyVR/releases' })
$folderButton.Add_Click({ $configuration=Read-Json (Join-Path $script:ManagedRoot 'configuration.json');Start-Process -FilePath 'explorer.exe' -ArgumentList ('"'+$configuration.retail+'"') })
$timer=New-Object Windows.Forms.Timer;$timer.Interval=350
$timer.Add_Tick({
    if ($script:Game -and $script:Game.HasExited) {
        $code=$script:Game.ExitCode;$script:Game.Dispose();$script:Game=$null
        try { Atomic-Json (Join-Path $script:ManagedRoot 'last-game-exit.json') @{version=[string]$script:State.active.version;exit_code=$code;time=(Get-Date -Format o)} } catch {}
        if ($code -eq 0) {
            try {
                $cleanup=@(Clean-LegacyInstallation $script:ManagedRoot)+@(Clean-PreviousLauncher $script:ManagedRoot)+@(Clean-ManagedFiles $script:ManagedRoot)
                [IO.File]::WriteAllLines((Join-Path $script:ManagedRoot 'cleanup-status.txt'),[string[]]$cleanup)
            } catch { [IO.File]::WriteAllText((Join-Path $script:ManagedRoot 'cleanup-status.txt'),$_.Exception.Message) }
            $form.Close();return
        }
        $form.Show();$status.Text='Game launch failed. You can retry or use the previous version.'
        $update.Enabled=$true;Refresh-State
    }
    if ($script:Worker -and $script:Worker.HasExited) {
        $mode=$script:WorkerMode
        $script:Worker.Dispose();$script:Worker=$null
        $update.Enabled=$true;$play.Enabled=$true
        try {
            if (!(Test-Path -LiteralPath $script:ResultFile)) { throw 'The update check did not complete. You can still play.' }
            $result=Read-Json $script:ResultFile
            if (!$result.ok) { throw [string]$result.error }
            Refresh-State
            if ($mode -eq 'apply') {
                $script:Candidate=$null;$update.Text='Check for updates'
                $status.Text='Update verified and installed. Starting Barony VR...'
                Play-Game
            } else {
                $script:Candidate=$result.candidate
                if ($script:Candidate) {
                    $status.Text='Available: '+$script:Candidate.update.version+'. Your calibration will be preserved.'
                    $update.Text='Update and play'
                    if ($script:Candidate.update.min_launcher -gt 1) { $update.Text='Get newer launcher';$status.Text='This release needs a newer launcher. Download its installer from Releases.' }
                } else { $status.Text='No newer compatible update was found.';$update.Text='Check for updates' }
            }
        } catch {
            $script:Candidate=$null;$update.Text='Retry update check';Refresh-State
            $status.Text='Updates unavailable: '+$_.Exception.Message+' You can still play.'
        } finally {
            if ($script:WorkDirectory -and (Test-Path -LiteralPath $script:WorkDirectory)) {
                try { Remove-ManagedDirectory $script:ManagedRoot 'Work' $script:WorkDirectory } catch {}
                $script:WorkDirectory=''
            }
        }
    }
})
$form.Add_Shown({
    Refresh-State
    $cleanup=@(Clean-ManagedFiles $script:ManagedRoot)
    [IO.File]::WriteAllLines((Join-Path $script:ManagedRoot 'cleanup-status.txt'),[string[]]$cleanup)
    Start-Worker 'check';$timer.Start()
})
$form.Add_FormClosed({
    $timer.Stop();Stop-Worker
    if ($script:OwnMutex) { $script:Mutex.ReleaseMutex() }
    $script:Mutex.Dispose()
})
$form.AcceptButton=$play
Run-LauncherLoop $form
$timer.Dispose();$form.Dispose()
