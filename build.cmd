@echo off
setlocal
rem Builds the xinput1_4.dll proxy with MSVC and installs it next to the game exe.
rem Usage: build.cmd            (build + install)
rem        build.cmd uninstall  (remove the proxy from the game folder)

set HERE=%~dp0
set GAME=%HERE%..
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

if /i "%~1"=="uninstall" (
    del /q "%GAME%\xinput1_4.dll" 2>nul
    echo Removed xinput1_4.dll from the game folder. The ini and runs were left in place.
    exit /b 0
)

if not exist %VCVARS% (
    echo vcvars64.bat not found at %VCVARS%
    exit /b 1
)
call %VCVARS% >nul
if errorlevel 1 exit /b 1

cd /d "%HERE%"
if not exist build mkdir build

rem Compiles every src\*.cpp: the harness modules (xinput_proxy.cpp with DllMain and the
rem exports, plus the modules sharing proxy_shared.h), gfx_d3d12.cpp and debug_menu.cpp.
cl /nologo /O2 /W3 /EHsc /MT /DUNICODE /D_UNICODE /std:c++17 ^
   /Fo:build\ /Fe:build\xinput1_4.dll src\*.cpp /LD ^
   /link /DEF:src\xinput1_4.def /OPT:REF kernel32.lib user32.lib gdi32.lib
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)

copy /y build\xinput1_4.dll "%GAME%\xinput1_4.dll" >nul
fc /b build\xinput1_4.dll "%GAME%\xinput1_4.dll" >nul 2>&1
if errorlevel 1 (
    echo.
    echo NOT INSTALLED: couldn't replace the game folder's xinput1_4.dll ^(is the game running?^).
    echo Close the game and run build.cmd again.
    exit /b 2
)
if not exist "%GAME%\xinput_harness.ini" copy /y xinput_harness.ini "%GAME%\xinput_harness.ini" >nul
echo.
echo Installed: "%GAME%\xinput1_4.dll"
echo Config:    "%GAME%\xinput_harness.ini"
exit /b 0
