<#
.SYNOPSIS
    Installs the PhoneMic virtual microphone driver ("PhoneMic Microphone").

.DESCRIPTION
    PhoneMic's own driver, so no third-party virtual cable is needed. Windows 11
    22H2 or newer.

    The driver isn't signed by Microsoft yet (that needs an EV certificate and a
    Partner Center account), so Windows only loads it in test mode. This script:
      1. explains test mode and asks before turning it on (Secure Boot must be off);
      2. creates a code-signing certificate that exists only on this PC, and
         trusts it for driver installs here;
      3. signs the driver package with it;
      4. creates the PhoneMic device and installs the driver.
    uninstall-driver.ps1 undoes all of it.

    Exit codes: 0 installed and running, 3010 restart Windows to finish, 1 failed.

.PARAMETER Yes
    Don't ask before turning on test mode.
#>
param(
    [switch]$Yes,
    [switch]$FromApp  # launched by the PhoneMic app: wait for Enter before closing
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$CertSubject = 'CN=PhoneMic Local Driver Signing'
$Stage = Join-Path $env:ProgramData 'PhoneMic\driver'

function Finish([int]$code, [string]$message, [string]$color = 'Green') {
    Write-Host ''
    Write-Host $message -ForegroundColor $color
    if ($FromApp) { [void](Read-Host 'Press Enter to close this window') }
    exit $code
}
function Fail([string]$message) { Finish 1 $message 'Red' }

Write-Host 'PhoneMic driver setup' -ForegroundColor Cyan
Write-Host '====================='

# --- administrator ------------------------------------------------------------------
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($Yes) { $argList += '-Yes' }
    if ($FromApp) { $argList += '-FromApp' }
    $p = Start-Process powershell.exe -Verb RunAs -ArgumentList $argList -PassThru -Wait
    exit $p.ExitCode
}

# --- Windows version ---------------------------------------------------------------------
$build = [Environment]::OSVersion.Version.Build
if ($build -lt 22621) {
    Fail "The PhoneMic driver needs Windows 11, version 22H2 or newer (this PC runs build $build). Update Windows, then run this again."
}

# --- package -------------------------------------------------------------------------------
foreach ($f in 'phonemic.sys', 'phonemic.inf', 'phonemic.cat', 'phonemic-driver.exe') {
    if (-not (Test-Path (Join-Path $here $f))) { Fail "Missing $f next to this script. Re-download PhoneMic." }
}

# --- test mode ------------------------------------------------------------------------------
$restart = $false
$bcd = (bcdedit /enum '{current}') | Out-String
if ($bcd -notmatch 'testsigning\s+Yes') {
    Write-Host ''
    Write-Host 'The PhoneMic driver is not signed by Microsoft yet, so Windows only loads it'
    Write-Host 'in TEST MODE. In test mode:'
    Write-Host '  - a "Test Mode" watermark shows on the desktop;'
    Write-Host '  - some games with anti-cheat refuse to start;'
    Write-Host '  - Secure Boot must be off (firmware settings).'
    Write-Host 'uninstall-driver.ps1 turns it off again.'
    Write-Host ''
    if (-not $Yes) {
        $answer = Read-Host 'Turn on test mode now? [y/N]'
        if ($answer -notmatch '^(y|yes)$') { Fail 'Cancelled. Nothing was changed.' }
    }
    bcdedit /set testsigning on | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Fail "Windows refused to turn on test mode. Secure Boot is probably on: turn it off in your PC's firmware settings, then run this again."
    }
    Write-Host 'Test mode turned on (takes effect after a restart).'
    $restart = $true
}

# --- signing certificate (this PC only) ---------------------------------------------------
$cert = Get-ChildItem Cert:\LocalMachine\My -CodeSigningCert |
    Where-Object { $_.Subject -eq $CertSubject -and $_.NotAfter -gt (Get-Date).AddDays(30) } |
    Select-Object -First 1
if (-not $cert) {
    Write-Host 'Creating a code-signing certificate for this PC...'
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject `
        -CertStoreLocation Cert:\LocalMachine\My -KeyExportPolicy NonExportable `
        -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(10)
}
# Trust it for driver installs on this PC (public part only).
$public = [Security.Cryptography.X509Certificates.X509Certificate2]::new($cert.RawData)
foreach ($storeName in 'Root', 'TrustedPublisher') {
    $store = [Security.Cryptography.X509Certificates.X509Store]::new($storeName, 'LocalMachine')
    $store.Open('ReadWrite')
    $store.Add($public)
    $store.Close()
}

# --- sign a staged copy of the package -------------------------------------------------------
New-Item -ItemType Directory -Force $Stage | Out-Null
foreach ($f in 'phonemic.sys', 'phonemic.inf', 'phonemic.cat') {
    Copy-Item (Join-Path $here $f) (Join-Path $Stage $f) -Force
}
foreach ($f in 'phonemic.sys', 'phonemic.cat') {
    $r = Set-AuthenticodeSignature -FilePath (Join-Path $Stage $f) -Certificate $cert -HashAlgorithm SHA256
    if ($r.Status -ne 'Valid') { Fail "Signing $f failed: $($r.StatusMessage)" }
}
Write-Host 'Driver package signed.'

# --- device + driver -------------------------------------------------------------------------
& (Join-Path $here 'phonemic-driver.exe') install (Join-Path $Stage 'phonemic.inf')
$code = $LASTEXITCODE
if ($code -eq 3010) { $restart = $true }
elseif ($code -ne 0) { Fail "Installing the driver failed (code $code). See the messages above." }

if ($restart) {
    Finish 3010 "Installed. Restart Windows to finish, then open PhoneMic and pick 'PhoneMic Microphone' in your apps."
}
& (Join-Path $here 'phonemic-driver.exe') status | Out-Null
if ($LASTEXITCODE -eq 0) {
    Finish 0 "Done. In Discord / Zoom / OBS pick the microphone 'PhoneMic Microphone'."
}
Finish 3010 'Installed, but the driver is not running yet. Restart Windows to finish.' 'Yellow'
