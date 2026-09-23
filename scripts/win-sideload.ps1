# Install the Windows MSIX on this PC the way the Store would, and check it
# (docs/build-windows.md, "The Store package").
#
#   powershell -ExecutionPolicy Bypass -File scripts/win-sideload.ps1 [<file.msix>]
#   ... -NoInstall      sign only: everything that needs no administrator
#   ... -Check          after installing, start the installed launcher's
#                       --diagnose with package identity and read back
#                       where it put the library
#   ... -Remove         uninstall the package; the library stays
#
# The Store signs its own uploads, so scripts/package-msix.sh leaves the
# package unsigned, and Windows installs nothing unsigned. This script
# makes a certificate whose subject is the manifest's Publisher (the one
# thing Windows checks the signature against), trusts it on this machine,
# signs a copy of the package and installs it; the app is then in the
# Start menu. With -Check it also runs the installed launcher's
# `--diagnose` *with package identity* and reads back where it put the
# library: outside AppData, or an uninstall would take the user's
# machines (paths::packaged()). Off by default: it starts the app, which
# an install does not need (user). The one step that needs an
# administrator is trusting the certificate (LocalMachine\TrustedPeople),
# and it is done once, through a UAC prompt, in a shell of its own.
#
# The default package is the newest build/win/package/*.msix. Nothing
# here touches the zip's library in %APPDATA%.
param(
  [string]$Msix,
  [switch]$NoInstall,
  [switch]$Remove,
  [switch]$Check
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$out = Join-Path $root 'build\win\package'

if (-not $Msix) {
  $found = Get-ChildItem (Join-Path $out '*.msix') -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -notlike '*-sideload.msix' } | Sort-Object LastWriteTime | Select-Object -Last 1
  if (-not $found) { throw "win-sideload: no .msix in $out (scripts/package-msix.sh)" }
  $Msix = $found.FullName
}
$Msix = (Resolve-Path $Msix).Path

# --- the identity, from the package itself ------------------------------
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($Msix)
try {
  $entry = $zip.GetEntry('AppxManifest.xml')
  if (-not $entry) { throw "win-sideload: $Msix has no AppxManifest.xml" }
  $reader = New-Object IO.StreamReader($entry.Open())
  [xml]$manifest = $reader.ReadToEnd()
  $reader.Close()
} finally { $zip.Dispose() }
$name = $manifest.Package.Identity.Name
$publisher = $manifest.Package.Identity.Publisher
$version = $manifest.Package.Identity.Version
$appId = $manifest.Package.Applications.Application.Id
"package        $name $version, $publisher"

if ($Remove) {
  $pkg = Get-AppxPackage -Name $name
  if (-not $pkg) { "not installed"; exit 0 }
  $pkg | Remove-AppxPackage
  "removed        $($pkg.PackageFullName); the library in $env:USERPROFILE\2ksbox stays"
  exit 0
}

# --- a certificate with the manifest's Publisher as its subject ----------
# Made once, in the user's own store, and reused: a second certificate
# with the same subject would work too, but each would need trusting.
$friendly = '2ksbox sideload'
$cert = Get-ChildItem Cert:\CurrentUser\My |
  Where-Object { $_.Subject -eq $publisher -and $_.FriendlyName -eq $friendly -and $_.HasPrivateKey } |
  Sort-Object NotAfter | Select-Object -Last 1
if (-not $cert) {
  $cert = New-SelfSignedCertificate -Type Custom -Subject $publisher -KeyUsage DigitalSignature `
    -FriendlyName $friendly -CertStoreLocation Cert:\CurrentUser\My `
    -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
  "certificate    made: $($cert.Thumbprint)"
} else {
  "certificate    $($cert.Thumbprint)"
}

# --- sign a copy ---------------------------------------------------------
$signtool = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe' -ErrorAction SilentlyContinue |
  Sort-Object { [version]($_.Directory.Parent.Name) } | Select-Object -Last 1
if (-not $signtool) { throw 'win-sideload: no signtool.exe (the Windows SDK)' }
$signed = Join-Path $out ([IO.Path]::GetFileNameWithoutExtension($Msix) + '-sideload.msix')
Copy-Item $Msix $signed -Force
& $signtool.FullName sign /fd SHA256 /sha1 $cert.Thumbprint $signed | Out-Null
if ($LASTEXITCODE -ne 0) { throw "win-sideload: signtool failed (the certificate's subject must equal the manifest's Publisher, $publisher)" }
$sig = Get-AuthenticodeSignature $signed
if ($sig.SignerCertificate.Thumbprint -ne $cert.Thumbprint) { throw 'win-sideload: the signature is not ours' }
"signed         $signed"
if ($NoInstall) { exit 0 }

# --- trust it, once, as an administrator ---------------------------------
$trusted = Get-ChildItem Cert:\LocalMachine\TrustedPeople -ErrorAction SilentlyContinue |
  Where-Object { $_.Thumbprint -eq $cert.Thumbprint }
if (-not $trusted) {
  $cer = Join-Path $out 'sideload.cer'
  Export-Certificate -Cert $cert -FilePath $cer | Out-Null
  $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
  $import = "Import-Certificate -FilePath '$cer' -CertStoreLocation Cert:\LocalMachine\TrustedPeople | Out-Null"
  if ($admin) {
    Invoke-Expression $import
  } else {
    "trust          the certificate goes into LocalMachine\TrustedPeople: answer the UAC prompt"
    Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile', '-Command', $import
  }
  $trusted = Get-ChildItem Cert:\LocalMachine\TrustedPeople | Where-Object { $_.Thumbprint -eq $cert.Thumbprint }
  if (-not $trusted) { throw 'win-sideload: the certificate was not trusted (the prompt was refused?)' }
  "trust          added to LocalMachine\TrustedPeople"
} else {
  "trust          already in LocalMachine\TrustedPeople"
}

# --- install -------------------------------------------------------------
Add-AppxPackage -Path $signed -ForceUpdateFromAnyVersion
$pkg = Get-AppxPackage -Name $name
if (-not $pkg) { throw 'win-sideload: installed, but Get-AppxPackage does not list it' }
"installed      $($pkg.PackageFullName)"
"               $($pkg.InstallLocation)"
if (-not $Check) { "               2ksbox is in the Start menu (-Check reads back where it keeps its library)"; exit 0 }

# --- the check: where does the installed launcher keep its library? -----
# `--diagnose` files `--paths`' answer in launcher.log beside the library,
# so the log's location is itself the answer, and its `library` line says
# whether the launcher knew it was packaged. Run through
# Invoke-CommandInDesktopPackage so the process has package identity, as
# a Start-menu launch has; an .exe run by path out of WindowsApps does not
# always get it. The command is the executable's full path: the cmdlet
# resolves a bare name against the caller's directory, not the package's,
# and a bare `2ksbox.exe` gave "could not find 2ksbox.exe".
$lib = Join-Path $env:USERPROFILE '2ksbox'
$log = Join-Path $lib 'launcher.log'
$before = 0
if (Test-Path $log) { $before = (Get-Content $log | Measure-Object -Line).Lines }
$exe = Join-Path $pkg.InstallLocation '2ksbox.exe'
Invoke-CommandInDesktopPackage -PackageFamilyName $pkg.PackageFamilyName -AppId $appId -Command $exe -Args '--diagnose'
$line = $null
$deadline = (Get-Date).AddSeconds(60)
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Seconds 2
  if (Test-Path $log) {
    $line = Get-Content $log | Select-Object -Skip $before | Where-Object { $_ -match '^library\s' } | Select-Object -Last 1
    if ($line) { break }
  }
}
if (-not $line) {
  throw "win-sideload: the installed launcher wrote no library line to $log within 60 s (a log under %APPDATA% would mean it did not see its package identity)"
}
if ($line -notmatch '\(packaged\)$') { throw "win-sideload: the installed launcher does not think it is packaged: $line" }
"library        $($line -replace '^library\s+', '')"
"ok             the installed package keeps its library outside AppData"
