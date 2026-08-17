# stop-scada.ps1
# Stops processes listening on typical SCADA ports (3001 for backend, 5173 for Vite frontend).
# Use with caution: this will attempt to stop any process using those ports.

$ports = @(3001, 5173)
foreach ($p in $ports) {
  $lines = netstat -ano | findstr ":$p"
  if ($lines) {
    foreach ($l in $lines) {
      $cols = $l -split '\s+' | Where-Object { $_ -ne '' }
      $pid = $cols[-1]
      if ($pid -and ($pid -as [int])) {
        try { Stop-Process -Id $pid -Force -ErrorAction Stop; Write-Host "Stopped PID $pid on port $p" }
        catch { & taskkill /PID $pid /F | Out-Null; Write-Host "Forced kill PID $pid on port $p" }
      }
    }
  } else {
    Write-Host "No process found on port $p"
  }
}
