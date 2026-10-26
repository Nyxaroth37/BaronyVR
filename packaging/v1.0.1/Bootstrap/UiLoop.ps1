function Run-LauncherLoop([System.Windows.Forms.Form]$Window) {
    # A modeless application loop stays alive when the launcher is hidden
    # during gameplay. Closing the window still ends the launcher normally.
    [System.Windows.Forms.Application]::Run($Window)
}
