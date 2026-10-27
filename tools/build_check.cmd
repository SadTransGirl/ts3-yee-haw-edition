@echo off
setlocal
rem Compile-only check build of every src\*.cpp (so new module files are picked up) into
rem build_next\check\xinput1_4.check.dll. Never installs anything. The compiler's warnings go to
rem build_next\check\warnings.txt; the baseline (commit ccba3f0) has none, so any line there is
rem new. Prints BUILD OK (and the warning count) or BUILD FAILED.
set H=%~dp0..
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if errorlevel 1 exit /b 1
cd /d "%H%"
if exist build_next\check rmdir /s /q build_next\check
mkdir build_next\check
cl /nologo /O2 /W3 /EHsc /MT /DUNICODE /D_UNICODE /std:c++17 ^
   /Fo:build_next\check\ /Fe:build_next\check\xinput1_4.check.dll src\*.cpp /LD ^
   /link /DEF:src\xinput1_4.def /OPT:REF kernel32.lib user32.lib gdi32.lib > build_next\check\build.txt 2>&1
set RC=%errorlevel%
findstr /r /c:"warning C[0-9]" build_next\check\build.txt > build_next\check\warnings.txt
if not "%RC%"=="0" (
    findstr /r /c:"error" build_next\check\build.txt
    echo BUILD FAILED
    exit /b 1
)
type build_next\check\warnings.txt
for /f %%n in ('type build_next\check\warnings.txt ^| find /c /v ""') do echo BUILD OK, %%n warning^(s^)
