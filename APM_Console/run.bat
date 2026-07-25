@echo off
cd /d "%~dp0"

rem Note: On this dev PC, Windows Smart App Control blocks locally-built
rem assemblies from loading (see the Smart App Control section in HOW_TO_RUN.md).
rem This may run fine on other Windows PCs without that restriction.
cd src\ApmConsole.Host
dotnet run --urls "http://localhost:5299"

echo.
echo [run] Exited with code %errorlevel%.
echo [run] If the message above says the app was blocked by "application control policy",
echo [run] that is Smart App Control - see HOW_TO_RUN.md. Not a code/build problem.
pause
