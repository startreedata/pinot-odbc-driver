# Copyright 2026 StarTree Inc.
#
# Licensed under the StarTree Community License (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy
# of the License at http://www.startree.ai/startree-community-license
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OF ANY KIND, either express or implied. See the License for the
# specific language governing permissions and limitations under the License.

#Requires -RunAsAdministrator
<#
.SYNOPSIS
Registers the StarTree Pinot ODBC driver with the Windows ODBC driver manager.

.EXAMPLE
.\install_windows.ps1 -DriverPath C:\drivers\pinot_odbc.dll
After registration, connect with:
  DRIVER={StarTree Pinot ODBC Driver};HOST=broker;PORT=8099;CONTROLLER=controller:9000
or create a DSN in the ODBC Data Source Administrator (odbcad32.exe).
#>
param(
    [string]$DriverPath = (Join-Path $PSScriptRoot "..\build\Release\pinot_odbc.dll"),
    [string]$DriverName = "StarTree Pinot ODBC Driver",
    [switch]$Uninstall
)

$instKey = "HKLM:\SOFTWARE\ODBC\ODBCINST.INI"
$driverKey = Join-Path $instKey $DriverName
$driversKey = Join-Path $instKey "ODBC Drivers"

if ($Uninstall) {
    Remove-Item -Path $driverKey -Recurse -ErrorAction SilentlyContinue
    Remove-ItemProperty -Path $driversKey -Name $DriverName -ErrorAction SilentlyContinue
    Write-Host "Unregistered '$DriverName'."
    exit 0
}

$DriverPath = (Resolve-Path $DriverPath).Path
if (-not (Test-Path $DriverPath)) {
    Write-Error "Driver DLL not found at $DriverPath"
    exit 1
}

New-Item -Path $driverKey -Force | Out-Null
Set-ItemProperty -Path $driverKey -Name "Driver" -Value $DriverPath
Set-ItemProperty -Path $driverKey -Name "Description" -Value "ODBC driver for Apache Pinot"
Set-ItemProperty -Path $driverKey -Name "APILevel" -Value "1"
Set-ItemProperty -Path $driverKey -Name "ConnectFunctions" -Value "YYN"
Set-ItemProperty -Path $driverKey -Name "DriverODBCVer" -Value "03.51"
Set-ItemProperty -Path $driverKey -Name "FileUsage" -Value "0"
Set-ItemProperty -Path $driverKey -Name "SQLLevel" -Value "0"
New-ItemProperty -Path $driverKey -Name "UsageCount" -Value 1 -PropertyType DWord -Force | Out-Null

New-Item -Path $driversKey -Force | Out-Null
Set-ItemProperty -Path $driversKey -Name $DriverName -Value "Installed"

Write-Host "Registered '$DriverName' -> $DriverPath"
Write-Host "Connect with: DRIVER={$DriverName};HOST=<broker>;PORT=8099;CONTROLLER=<controller>:9000"
