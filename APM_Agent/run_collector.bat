@echo off
cd /d "%~dp0"

if exist build-static\Debug\Collector.exe (
    build-static\Debug\Collector.exe
) else if exist build\Debug\Collector.exe (
    build\Debug\Collector.exe
) else (
    echo [run_collector] Collector.exe not found under build-static\Debug\ or build\Debug\.
    echo [run_collector] Build first - see HOW_TO_RUN.md section 3.
    pause
)
