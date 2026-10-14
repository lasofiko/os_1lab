@echo off
setlocal
cd /d "%~dp0"
where cl >nul 2>nul
if errorlevel 1 (
    echo Run this file from Developer Command Prompt for Visual Studio.
    exit /b 1
)
cl /nologo /W4 /EHsc /std:c++17 /utf-8 child1.cpp /Fe:child1.exe
if errorlevel 1 exit /b 1
cl /nologo /W4 /EHsc /std:c++17 /utf-8 child2.cpp /Fe:child2.exe
if errorlevel 1 exit /b 1
cl /nologo /W4 /EHsc /std:c++17 /utf-8 parent.cpp /Fe:parent.exe
if errorlevel 1 exit /b 1
echo Build completed.
