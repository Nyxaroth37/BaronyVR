Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()
$ErrorActionPreference = 'Stop'
try {
    . (Join-Path $PSScriptRoot 'Common.ps1')
    Check-Prerequisites
    $null = Check-Payload $PSScriptRoot
} catch {
    [Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Barony VR setup', 'OK', 'Error') | Out-Null
    exit 1
}
$script:InstalledRoot = Join-Path $env:LOCALAPPDATA 'BaronyVR\GA21-PublicBeta1'
$form = New-Object Windows.Forms.Form
$form.Text = 'Barony VR | Beta setup'
$form.ClientSize = New-Object Drawing.Size(650, 470)
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.Font = New-Object Drawing.Font('Segoe UI', 10)
$form.BackColor = [Drawing.Color]::FromArgb(27, 30, 36)
$form.ForeColor = [Drawing.Color]::FromArgb(235, 237, 240)
function Label([string]$Text, [int]$X, [int]$Y, [int]$Width, [int]$Height) {
    $label = New-Object Windows.Forms.Label
    $label.Text = $Text
    $label.Location = New-Object Drawing.Point($X, $Y)
    $label.Size = New-Object Drawing.Size($Width, $Height)
    $form.Controls.Add($label)
    return $label
}
$title = Label 'BARONY VR' 28 24 580 42
$title.Font = New-Object Drawing.Font('Segoe UI', 23, [Drawing.FontStyle]::Bold)
$null = Label 'GA21  /  PUBLIC BETA 1' 30 73 580 25
$null = Label 'Play your retail copy of Barony with native OpenXR VR.' 30 113 590 28
$null = Label "Requires Windows x64, Steam Barony build 22630456, and a PC VR connection.`r`nTested headset/runtime: Quest 3 with SteamVR. Other headsets are unverified." 30 145 590 53
$null = Label 'Steam Barony folder' 30 210 580 24
$pathBox = New-Object Windows.Forms.TextBox
$pathBox.Location = New-Object Drawing.Point(30, 239)
$pathBox.Size = New-Object Drawing.Size(475, 28)
$pathBox.Text = Find-Retail
$form.Controls.Add($pathBox)
$browse = New-Object Windows.Forms.Button
$browse.Text = 'Browse...'
$browse.Location = New-Object Drawing.Point(518, 237)
$browse.Size = New-Object Drawing.Size(100, 31)
$form.Controls.Add($browse)
$browse.Add_Click({
    $dialog = New-Object Windows.Forms.FolderBrowserDialog
    $dialog.Description = 'Select the retail Barony folder containing barony.exe.'
    $dialog.ShowNewFolderButton = $false
    if ($dialog.ShowDialog() -eq 'OK') { $pathBox.Text = $dialog.SelectedPath }
    $dialog.Dispose()
})
$null = Label "Existing calibration is preserved. Missing VR settings receive Mike's defaults.`r`nDefaults use seated mode and a right draw hand; adjust them in calibration.`r`nThis beta uses the retail game's settings and saves. It does not replace its EXE." 30 286 590 72
$status = Label 'Ready to install. No source build or developer tools required.' 30 366 590 28
$status.ForeColor = [Drawing.Color]::LightSteelBlue
$help = New-Object Windows.Forms.Button
$help.Text = 'Read beta notes'
$help.Location = New-Object Drawing.Point(30, 412)
$help.Size = New-Object Drawing.Size(150, 34)
$form.Controls.Add($help)
$help.Add_Click({ Start-Process -FilePath 'notepad.exe' -ArgumentList ('"' + (Join-Path $PSScriptRoot 'README.txt') + '"') })
$install = New-Object Windows.Forms.Button
$install.Text = 'Install beta'
$install.Location = New-Object Drawing.Point(450, 408)
$install.Size = New-Object Drawing.Size(168, 40)
$install.BackColor = [Drawing.Color]::FromArgb(60, 100, 150)
$install.ForeColor = [Drawing.Color]::White
$install.FlatStyle = 'Flat'
$form.Controls.Add($install)
$install.Add_Click({
    if ($script:SetupFinished) {
        Start-Process -FilePath (Join-Path $script:InstalledRoot 'BaronyVR.exe')
        $form.Close()
        return
    }
    $install.Enabled = $false
    try {
        $retail = [IO.Path]::GetFullPath($pathBox.Text.Trim()).TrimEnd('\')
        Check-Retail $retail
        if (Get-Process -Name barony -ErrorAction SilentlyContinue) { throw 'Close Barony before installing.' }
        $status.Text = 'Verifying and installing the GA21 runtime...'
        $form.Refresh()
        $installed = $script:InstalledRoot
        if (Test-Path -LiteralPath $installed) {
            $null = Check-Payload $installed
            if ((Get-FileHash -LiteralPath (Join-Path $installed 'payload.json')).Hash -ne
                (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'payload.json')).Hash) {
                throw 'An installation with different contents already exists. It was left unchanged.'
            }
        } else {
            $parent = Split-Path -Parent $installed
            if (!(Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent | Out-Null }
            $stage = Join-Path $parent ('staging-' + [Guid]::NewGuid().ToString('N'))
            New-Item -ItemType Directory -Path $stage | Out-Null
            $manifest = Check-Payload $PSScriptRoot
            foreach ($entry in $manifest.files) {
                $destination = Join-Path $stage $entry.path
                $directory = Split-Path -Parent $destination
                if (!(Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
                Copy-Item -LiteralPath (Join-Path $PSScriptRoot $entry.path) -Destination $destination
            }
            Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'payload.json') -Destination (Join-Path $stage 'payload.json')
            $null = Check-Payload $stage
            [IO.Directory]::Move($stage, $installed)
        }
        $settingsPath = Join-Path $installed 'installation.json'
        if (Test-Path -LiteralPath $settingsPath) {
            $old = Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json
            if ($old.retail -ne $retail) { throw 'This beta is already configured for another retail folder. Its configuration was preserved.' }
        } else {
            @{ retail = [string]::new($retail.ToCharArray()); version = 'GA21-PublicBeta1' } |
                ConvertTo-Json -Depth 2 | Set-Content -LiteralPath $settingsPath -Encoding UTF8
        }
        $created = Seed-Defaults $installed $retail
        $shell = New-Object -ComObject WScript.Shell
        $shortcutName = 'Barony VR - GA21 Beta.lnk'
        $powershell = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
        $launcherExe = Join-Path $installed 'BaronyVR.exe'
        $legacyRoot = Join-Path $env:LOCALAPPDATA 'BaronyVR\GA21-Beta1'
        $legacyArguments = '-NoProfile -ExecutionPolicy Bypass -STA -WindowStyle Hidden -File "' + (Join-Path $legacyRoot 'Launch.ps1') + '"'
        $previousExe = Join-Path $env:LOCALAPPDATA 'BaronyVR\GA21-Beta1R1\BaronyVR.exe'
        foreach ($directory in @([Environment]::GetFolderPath('Desktop'), [Environment]::GetFolderPath('Programs'))) {
            if (!$directory) { continue }
            $shortcutPath = Join-Path $directory $shortcutName
            $shortcut = $shell.CreateShortcut($shortcutPath)
            if (Test-Path -LiteralPath $shortcutPath) {
                if ($shortcut.TargetPath -eq $launcherExe -and !$shortcut.Arguments) { continue }
                $knownPowerShellShortcut = $shortcut.TargetPath -eq $powershell -and $shortcut.Arguments -eq $legacyArguments
                $knownExeShortcut = $shortcut.TargetPath -eq $previousExe -and !$shortcut.Arguments
                if (!$knownPowerShellShortcut -and !$knownExeShortcut) {
                    throw "An unrelated shortcut already uses this name; it was preserved: $shortcutPath"
                }
                $backupDirectory = Join-Path $installed 'ShortcutBackups'
                if (!(Test-Path -LiteralPath $backupDirectory)) { New-Item -ItemType Directory -Path $backupDirectory | Out-Null }
                $backup = Join-Path $backupDirectory ($shortcutName + '.' + [Guid]::NewGuid().ToString('N') + '.bak')
                Copy-Item -LiteralPath $shortcutPath -Destination $backup
            }
            $shortcut.TargetPath = $launcherExe
            $shortcut.Arguments = ''
            $shortcut.WorkingDirectory = $retail
            $shortcut.Description = 'Play Barony in VR using the GA21 beta.'
            $shortcut.IconLocation = (Join-Path $installed 'Runtime\barony.exe') + ',0'
            $shortcut.Save()
        }
        $script:SetupFinished = $true
        $pathBox.Enabled = $false
        $browse.Enabled = $false
        $status.Text = "Installed. Added $created missing VR default files; existing files preserved."
        $install.Text = 'Play in VR'
        [Windows.Forms.MessageBox]::Show("Setup complete.`r`n`r`nConnect your headset to your PC, start Steam and your OpenXR runtime, then use Play in VR or the Barony VR shortcut.`r`n`r`nYour retail EXE and existing calibration were preserved.", 'Barony VR installed', 'OK', 'Information') | Out-Null
    } catch {
        $status.Text = 'Setup stopped. Review the message and retry when ready.'
        [Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Barony VR setup', 'OK', 'Error') | Out-Null
    } finally { $install.Enabled = $true }
})
$form.AcceptButton = $install
[void]$form.ShowDialog()
$form.Dispose()
