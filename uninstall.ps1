# Remove the BlueJ2534 J2534 registry entry. Run as Administrator.
$ErrorActionPreference = "Stop"

if ([Environment]::Is64BitOperatingSystem) {
    $key = "HKLM:\SOFTWARE\WOW6432Node\PassThruSupport.04.04\BlueJ2534"
} else {
    $key = "HKLM:\SOFTWARE\PassThruSupport.04.04\BlueJ2534"
}

if (Test-Path $key) {
    Remove-Item -Path $key -Recurse -Force
    Write-Host "Removed $key"
} else {
    Write-Host "Not installed."
}
