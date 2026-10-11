<#
.SYNOPSIS
    Removes the PhoneMic virtual microphone driver and undoes what
    install-driver.ps1 set up: the device, the driver package, the local signing
    certificate and (if you agree) test mode.
#>
param([switch]$Yes)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$CertSubject = 'CN=PhoneMic Local Driver Signing'

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($Yes) { $argList += '-Yes' }
    $p = Start-Process powershell.exe -Verb RunAs -ArgumentList $argList -PassThru -Wait
    exit $p.ExitCode
}

# Device
$tool = Join-Path $here 'phonemic-driver.exe'
if (Test-Path $tool) { & $tool remove }

# Driver package(s) in the driver store
Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like '*\phonemic.inf' } | ForEach-Object {
    Write-Host "Removing driver package $($_.Driver)"
    pnputil /delete-driver $_.Driver /uninstall /force | Out-Null
}

# Certificate (from every store install-driver.ps1 put it in)
foreach ($storeName in 'My', 'Root', 'TrustedPublisher') {
    Get-ChildItem "Cert:\LocalMachine\$storeName" | Where-Object { $_.Subject -eq $CertSubject } |
        Remove-Item -Force -ErrorAction SilentlyContinue
}
Remove-Item (Join-Path $env:ProgramData 'PhoneMic\driver') -Recurse -Force -ErrorAction SilentlyContinue

# Test mode
$bcd = (bcdedit /enum '{current}') | Out-String
if ($bcd -match 'testsigning\s+Yes') {
    $off = $Yes
    if (-not $Yes) {
        $answer = Read-Host 'Turn test mode off? (Say no if another test-signed driver needs it.) [Y/n]'
        $off = $answer -notmatch '^(n|no)$'
    }
    if ($off) {
        bcdedit /set testsigning off | Out-Null
        Write-Host 'Test mode turned off. Restart Windows to finish.'
    }
}
Write-Host 'PhoneMic driver removed.' -ForegroundColor Green
