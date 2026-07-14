@echo off
setlocal

echo.
echo ============================================
echo   The Dawning V3 Engine - Build Script
echo ============================================
echo.

set "CMAKE_EXE="

for /f "delims=" %%I in ('where cmake 2^>nul') do (
    set "CMAKE_EXE=%%I"
    goto :found_cmake
)

if exist "%ProgramFiles%\CMake\bin\cmake.exe" (
    set "CMAKE_EXE=%ProgramFiles%\CMake\bin\cmake.exe"
    goto :found_cmake
)

if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    goto :found_cmake
)

if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    goto :found_cmake
)

if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    goto :found_cmake
)

if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "CMAKE_EXE=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    goto :found_cmake
)

echo ERROR: CMake was not found.
echo Install CMake or Visual Studio 2022 with the C++ CMake tools component.
exit /b 1

:found_cmake
echo Using CMake: "%CMAKE_EXE%"
echo.

echo [1/2] Configuring with CMake...
"%CMAKE_EXE%" -S . -B build -G "Visual Studio 17 2022" -A x64
if %ERRORLEVEL% neq 0 (
    echo.
    echo ERROR: CMake configuration failed.
    echo Make sure Visual Studio 2022 with the C++ workload is installed.
    exit /b 1
)

echo.
echo [2/2] Building Debug...
"%CMAKE_EXE%" --build build --config Debug
if %ERRORLEVEL% neq 0 (
    echo.
    echo ERROR: Build failed.
    exit /b 1
)

echo.
echo ============================================
echo   BUILD SUCCESSFUL
echo   Output: build\Debug\TheDawningV3.exe
echo ============================================
echo.
