$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$PackageRoot = Join-Path $Root "package\network-esp32-ble-printer"
$RemoteHost = "root@Megamind"
$RemoteRoot = "/tmp/network-esp32-ble-printer"
$BridgePy = Join-Path $PackageRoot "files\usr\libexec\network-esp32-ble-printer\bridge.py"
$BridgeVersion = (Select-String -Path $BridgePy -Pattern '^SERVICE_VERSION = "([^"]+)"').Matches[0].Groups[1].Value

Write-Host "Preparing remote temp root on $RemoteHost ..."
ssh $RemoteHost "rm -rf ${RemoteRoot} && mkdir -p ${RemoteRoot}"

Write-Host "Copying package files to $RemoteHost ..."
Write-Host "Local bridge version: $BridgeVersion"

scp -O -r "$PackageRoot\files" "${RemoteHost}:${RemoteRoot}/"
scp -O "$PSScriptRoot\install-network-esp32-ble-printer.sh" "${RemoteHost}:${RemoteRoot}/install-network-esp32-ble-printer.sh"

Write-Host "Installing/updating package on $RemoteHost ..."

ssh $RemoteHost "chmod 755 ${RemoteRoot}/install-network-esp32-ble-printer.sh && sh ${RemoteRoot}/install-network-esp32-ble-printer.sh ${RemoteRoot}"

Write-Host "Done."
