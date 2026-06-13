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

<#
.SYNOPSIS
Creates (or removes) a per-user ODBC DSN for the StarTree Pinot ODBC driver.

The driver has no GUI configuration dialog, so this script writes the DSN
directly to the registry (HKCU — no admin rights required). The driver itself
must already be registered system-wide via install_windows.ps1.

.EXAMPLE
.\create_dsn_windows.ps1 -DsnName Pinot -BrokerHost broker.example.com -BrokerPort 8099 `
    -Controller controller.example.com:9000

.EXAMPLE
.\create_dsn_windows.ps1 -DsnName PinotCloud -BrokerHost broker.cloud.startree.ai -BrokerPort 443 `
    -Scheme https -Controller controller.cloud.startree.ai:443 -Token my-bearer-token

.EXAMPLE
.\create_dsn_windows.ps1 -DsnName Pinot -Remove
#>
param(
    [Parameter(Mandatory = $true)][string]$DsnName,
    [string]$BrokerHost = "localhost",
    [int]$BrokerPort = 8099,
    [string]$Scheme = "http",
    [string]$Controller = "",
    [string]$Uid = "",
    [string]$Pwd = "",
    [string]$Token = "",
    [string]$Database = "",
    [string]$QueryOptions = "",
    [int]$Timeout = 60,
    [int]$SslVerify = 1,
    [switch]$UseMultistage,
    [string]$DriverName = "StarTree Pinot ODBC Driver",
    [switch]$Remove
)

$iniKey = "HKCU:\SOFTWARE\ODBC\ODBC.INI"
$dsnKey = Join-Path $iniKey $DsnName
$sourcesKey = Join-Path $iniKey "ODBC Data Sources"

if ($Remove) {
    Remove-Item -Path $dsnKey -Recurse -ErrorAction SilentlyContinue
    Remove-ItemProperty -Path $sourcesKey -Name $DsnName -ErrorAction SilentlyContinue
    Write-Host "Removed DSN '$DsnName'."
    exit 0
}

$driverDll = (Get-ItemProperty `
    -Path "HKLM:\SOFTWARE\ODBC\ODBCINST.INI\$DriverName" `
    -Name Driver -ErrorAction SilentlyContinue).Driver
if (-not $driverDll) {
    Write-Error ("Driver '$DriverName' is not registered. " +
                 "Run install_windows.ps1 from an elevated prompt first.")
    exit 1
}

New-Item -Path $dsnKey -Force | Out-Null
# The driver reads these keys through SQLGetPrivateProfileString.
Set-ItemProperty -Path $dsnKey -Name "Driver" -Value $driverDll
Set-ItemProperty -Path $dsnKey -Name "Host" -Value $BrokerHost
Set-ItemProperty -Path $dsnKey -Name "Port" -Value "$BrokerPort"
Set-ItemProperty -Path $dsnKey -Name "Scheme" -Value $Scheme
Set-ItemProperty -Path $dsnKey -Name "Timeout" -Value "$Timeout"
Set-ItemProperty -Path $dsnKey -Name "SslVerify" -Value "$SslVerify"
if ($Controller) { Set-ItemProperty -Path $dsnKey -Name "Controller" -Value $Controller }
if ($Uid) { Set-ItemProperty -Path $dsnKey -Name "Uid" -Value $Uid }
if ($Pwd) { Set-ItemProperty -Path $dsnKey -Name "Pwd" -Value $Pwd }
if ($Token) { Set-ItemProperty -Path $dsnKey -Name "Token" -Value $Token }
if ($Database) { Set-ItemProperty -Path $dsnKey -Name "Database" -Value $Database }
if ($QueryOptions) { Set-ItemProperty -Path $dsnKey -Name "QueryOptions" -Value $QueryOptions }
if ($UseMultistage) { Set-ItemProperty -Path $dsnKey -Name "UseMultistage" -Value "1" }

New-Item -Path $sourcesKey -Force | Out-Null
Set-ItemProperty -Path $sourcesKey -Name $DsnName -Value $DriverName

Write-Host "Created user DSN '$DsnName' -> $Scheme`://$BrokerHost`:$BrokerPort"
Write-Host "It will appear in the ODBC connector of Power BI, Excel, and other 64-bit apps."
