@echo off
cd /d "%~dp0"

if exist build-static\Debug\Agent.exe (
    build-static\Debug\Agent.exe
) else if exist build\Debug\Agent.exe (
    build\Debug\Agent.exe
) else (
    echo [run_agent] Agent.exe not found under build-static\Debug\ or build\Debug\.
    echo [run_agent] Build first - see HOW_TO_RUN.md section 3.
    pause
)
