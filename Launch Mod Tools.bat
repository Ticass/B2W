@echo off
cd /d "%~dp0"
if exist "%~dp0work\desktop_dist\WawConverter\WawConverter.exe" (
    start "" "%~dp0work\desktop_dist\WawConverter\WawConverter.exe"
    exit /b 0
)
set "PYTHONPATH=%~dp0src"
where pythonw.exe >nul 2>nul
if errorlevel 1 (
    echo Python with Tkinter was not found. Use the portable Windows download,
    echo or install Python 3.11 or newer from python.org, including Tcl/Tk.
    pause
    exit /b 1
)
start "" pythonw.exe -m waw2bo2.gui
