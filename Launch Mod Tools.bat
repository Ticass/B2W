@echo off
rem Start the converter's launcher: the packaged one, else the checkout's build.
cd /d "%~dp0"
if exist "%~dp0work\desktop_dist\WawConverter\B2W.exe" (
    start "" "%~dp0work\desktop_dist\WawConverter\B2W.exe"
    exit /b 0
)
if exist "%~dp0native\build\B2W.exe" (
    start "" "%~dp0native\build\B2W.exe"
    exit /b 0
)
echo The converter is not built yet. Use the portable Windows download, or build it with:
echo     powershell -ExecutionPolicy Bypass -File tools\restore_native.ps1
pause
exit /b 1
