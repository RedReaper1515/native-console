@echo off
setlocal enabledelayedexpansion

echo ==========================================
echo Native Console - Setup Script for Windows
echo ==========================================
echo.

:: Check for required tools
echo Checking for Chocolatey package manager...
choco -? >nul 2>&1
if !errorlevel! neq 0 (
    echo.
    echo ERROR: Chocolatey package manager not found!
    echo Please install Chocolatey first by following these steps:
    echo 1. Open Command Prompt as Administrator
    echo 2. Run: @"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -InputFormat None -ExecutionPolicy Bypass -Command "[System.Net.ServicePointManager]::SecurityProtocol = 3072; iex ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))" && SET "PATH=%PATH%;%ALLUSERSPROFILE%\chocolatey\bin"
    echo 3. Restart Command Prompt as Administrator
    echo 4. Run this script again
    echo.
    pause
    exit /b 1
)

echo Chocolatey found.
echo.

:: Install dependencies
echo Installing dependencies...
echo.

choco install -y cmake ninja sdl2 vcpkg git python3

:: Check if Python installation worked
python --version >nul 2>&1
if !errorlevel! neq 0 (
    echo.
    echo ERROR: Python installation failed!
    echo Please try installing Python manually from https://www.python.org/downloads/
    echo.
    pause
    exit /b 1
)

:: Install Python dependencies
echo Installing Python dependencies...
pip install pyboy

echo.
echo Dependencies installed successfully!
echo.

:: Check if vcpkg is available
vcpkg version >nul 2>&1
if !errorlevel! neq 0 (
    echo.
    echo WARNING: vcpkg not found in PATH.
    echo SDL2 should still be available through Chocolatey, but some features may be limited.
    echo.
)

:: Build the project
echo Building the recompiler...
echo.

if not exist "build" mkdir build

cd build
cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release ..

if !errorlevel! neq 0 (
    echo.
    echo ERROR: CMake configuration failed!
    echo Please check the error message above.
    echo.
    cd ..
    pause
    exit /b 1
)

echo.
echo Compiling...
ninja

if !errorlevel! neq 0 (
    echo.
    echo ERROR: Build failed!
    echo Please check the error message above.
    echo.
    cd ..
    pause
    exit /b 1
)

cd ..

echo.
echo ==========================================
echo Build completed successfully!
echo ==========================================
echo.
echo Recompiler executable: build\bin\gbrecomp.exe
echo.
echo Usage examples:
echo   1. Basic recompilation:
echo      build\bin\gbrecomp.exe path\to\game.gb -o output\game
echo.
echo   2. Ground truth workflow:
echo      python tools\run_ground_truth.py path\to\game.gb
echo.
echo Detailed instructions in docs/GROUND_TRUTH_WORKFLOW.md
echo.

pause
