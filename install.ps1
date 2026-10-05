# BlueJ2534 - register as a J2534 04.04 PassThru device for 32-bit software.
# Run as Administrator.

$ErrorActionPreference = "Stop"

$dll = Join-Path $PSScriptRoot "bin\BlueJ2534.dll"
if (-not (Test-Path $dll)) {
    Write-Error "bin\BlueJ2534.dll not found - run build.bat first."
}

# 32-bit apps on 64-bit Windows read the WOW6432Node view.
if ([Environment]::Is64BitOperatingSystem) {
    $base = "HKLM:\SOFTWARE\WOW6432Node\PassThruSupport.04.04"
} else {
    $base = "HKLM:\SOFTWARE\PassThruSupport.04.04"
}
$key = "$base\BlueJ2534"

New-Item -Path $key -Force | Out-Null
Set-ItemProperty -Path $key -Name "Name"            -Value "Bluetooth OBDX J2534"
Set-ItemProperty -Path $key -Name "Vendor"          -Value "Tester Present Specialist Automotive Solutions"
Set-ItemProperty -Path $key -Name "FunctionLibrary" -Value $dll
Set-ItemProperty -Path $key -Name "ConfigApplication" -Value ""

# Protocol support flags (OBDX Pro FT: CAN/ISO15765 incl. pin-select MS-CAN,
# J1850 VPW; no K-line, no PWM, no SCI)
foreach ($p in @{ CAN=1; ISO15765=1; CAN_PS=1; ISO15765_PS=1; J1850VPW=1;
                  ISO9141=0; ISO14230=0; J1850PWM=0;
                  SCI_A_ENGINE=0; SCI_A_TRANS=0; SCI_B_ENGINE=0; SCI_B_TRANS=0 }.GetEnumerator()) {
    New-ItemProperty -Path $key -Name $p.Key -Value $p.Value -PropertyType DWord -Force | Out-Null
}

Write-Host "Registered '$key'"
Write-Host "FunctionLibrary = $dll"
Write-Host "BlueJ2534 should now appear as a J2534 device in 32-bit applications."
