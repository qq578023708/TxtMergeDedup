@echo off
rem Build TxtMergeDedup.exe (MSVC x64). ASCII only to avoid codepage issues.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere not found. Run from "x64 Native Tools Command Prompt for VS" instead.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo [ERROR] Visual Studio not found.
    exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O2 /GL /std:c++17 /EHsc /W3 /DUNICODE /D_UNICODE TxtMergeDedup.cpp /Fe:TxtMergeDedup.exe /Fo:TxtMergeDedup.obj /link /LTCG /SUBSYSTEM:WINDOWS
if errorlevel 1 (
    echo [FAILED] compile error.
    exit /b 1
)
del TxtMergeDedup.obj 2>nul
echo [OK] TxtMergeDedup.exe generated.
endlocal
