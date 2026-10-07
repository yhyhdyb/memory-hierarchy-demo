@echo off
rem ============================================================
rem  One-click build (Windows)
rem  Tries in order: g++ in PATH / MSVC cl / common MinGW paths
rem  Output: memhier.exe
rem  NOTE: keep this file pure ASCII (cmd parses .bat as ANSI/GBK)
rem ============================================================
setlocal
cd /d "%~dp0"

set "SRC=src\main.cpp src\cache_hierarchy.cpp src\ram_vs_disk.cpp src\dma_sim.cpp"

where g++ >nul 2>nul
if %errorlevel%==0 (
    echo [1/2] Building with g++ ...
    g++ -O2 -std=c++17 -Wall -Wextra -pthread -o memhier.exe %SRC%
    if errorlevel 1 goto :fail
    echo [2/2] Build OK: memhier.exe
    goto :done
)

where cl >nul 2>nul
if %errorlevel%==0 (
    echo [1/2] Building with MSVC cl ...
    cl /nologo /O2 /std:c++17 /EHsc /utf-8 /W3 src\*.cpp /Fe:memhier.exe
    if errorlevel 1 goto :fail
    echo [2/2] Build OK: memhier.exe
    goto :done
)

for %%C in (
    "C:\Program Files (x86)\Embarcadero\Dev-Cpp\TDM-GCC-64\bin\g++.exe"
    "C:\msys64\mingw64\bin\g++.exe"
    "C:\ProgramData\chocolatey\bin\g++.exe"
    "D:\Qt\Tools\mingw1120_64\bin\g++.exe"
) do (
    if exist "%%~C" (
        echo [1/2] Building with %%~C ...
        "%%~C" -O2 -std=c++17 -Wall -Wextra -pthread -o memhier.exe %SRC%
        if errorlevel 1 goto :fail
        echo [2/2] Build OK: memhier.exe
        goto :done
    )
)

echo [ERROR] No C++ compiler found (g++ / cl).
echo         Install MinGW-w64 or Visual Studio Build Tools, then re-run.
exit /b 1

:fail
echo [ERROR] Build failed. Check the messages above.
exit /b 1

:done
echo.
echo Run:
echo   memhier.exe            all experiments
echo   memhier.exe 1          cache hierarchy only
echo   memhier.exe 2          RAM vs disk only
echo   memhier.exe 3          DMA simulation only
exit /b 0
