param([string]$ManagedRoot, [string]$OutputFile)
try {
    . (Join-Path $PSScriptRoot 'UpdateCore.ps1')
    $state=Read-Json (Join-Path $ManagedRoot 'active.json')
    $scratch=New-ManagedDirectory $ManagedRoot 'Checks'
    $candidate=Find-Update ([int]$state.active.sequence) $scratch
    Atomic-Json $OutputFile @{ok=$true;candidate=$candidate}
} catch {
    @{ok=$false;error=[string]$_.Exception.Message} | ConvertTo-Json -Depth 3 |
        Set-Content -LiteralPath $OutputFile -Encoding UTF8
    exit 1
} finally { if ($scratch -and (Test-Path -LiteralPath $scratch)) { try { Remove-ManagedDirectory $ManagedRoot 'Checks' $scratch } catch {} } }
