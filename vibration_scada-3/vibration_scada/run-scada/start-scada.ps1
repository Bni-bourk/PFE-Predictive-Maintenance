# start-scada.ps1
# Starts backend and frontend in separate PowerShell windows.
# Usage: Right-click -> Run with PowerShell, or execute from a PowerShell prompt.

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path "$root\.."

# Auto-detect Node.js path if not globally registered in env variables yet
$nodeDir = "C:\Program Files\nodejs"
if (!(Get-Command npm -ErrorAction SilentlyContinue) -and (Test-Path "$nodeDir\npm.cmd")) {
    $env:Path = "$nodeDir;$env:Path"
}

# Construct the commands with Path injection to ensure npm is recognized
$backendCmd = "Set-Location -LiteralPath '$repoRoot\backend'; `$env:Path = '$nodeDir;' + `$env:Path; npm install; node server.js"
$frontendCmd = "Set-Location -LiteralPath '$repoRoot\frontend'; `$env:Path = '$nodeDir;' + `$env:Path; npm install; npm run dev"

# Backend: runs node server.js in backend folder
Start-Process -FilePath powershell -ArgumentList @("-NoExit","-Command", $backendCmd) -WindowStyle Normal

# Frontend: runs npm run dev in frontend folder
Start-Process -FilePath powershell -ArgumentList @("-NoExit","-Command", $frontendCmd) -WindowStyle Normal

Write-Host "Started backend and frontend in new PowerShell windows."