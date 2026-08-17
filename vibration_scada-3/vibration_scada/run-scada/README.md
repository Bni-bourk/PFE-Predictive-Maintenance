Run SCADA helper scripts

This folder contains simple PowerShell helpers to start and stop the SCADA backend and frontend for development on Windows.

Files:
- `start-scada.ps1` — opens two new PowerShell windows and runs the backend (`node server.js`) and frontend (`npm run dev`). It also runs `npm install` in each folder if dependencies are missing.
- `stop-scada.ps1` — attempts to stop processes listening on ports `3001` (backend) and `5173` (Vite frontend).

Usage:
1. Open PowerShell as your normal user (no admin required).
2. Run `run-scada\start-scada.ps1` (right-click -> Run with PowerShell or `.







- `stop-scada.ps1` forcefully kills processes on the two ports; use with care if other apps use these ports.- `start-scada.ps1` opens separate windows so you can see logs and stop each server independently with Ctrl+C if needed.- The scripts assume the repository layout is the same as the workspace (top-level `backend/` and `frontend/` folders).Notes:3. To stop, run `.
un-scada\stop-scada.ps1`.un-scada\start-scada.ps1`).