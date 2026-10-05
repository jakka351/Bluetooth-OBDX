@echo off
rem Build BlueJ2534.dll (32-bit) with MSVC
setlocal

set "VSPATH="
if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" set "VSPATH=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
if "%VSPATH%"=="" if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars32.bat" set "VSPATH=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools"
if "%VSPATH%"=="" (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "%VSWHERE%" (
        for /f "usebackq tokens=*" %%i in (`""%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath"`) do set "VSPATH=%%i"
    )
)
if "%VSPATH%"=="" (
    echo Could not locate a Visual Studio C++ toolchain.
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1

if not exist bin mkdir bin

cl /nologo /W3 /O2 /EHsc /MT /LD ^
   src\shim.cpp src\elm327.cpp src\transport.cpp src\log.cpp ^
   /Fe:bin\BlueJ2534.dll /Fo:bin\ ^
   /link /DEF:src\BlueJ2534.def ws2_32.lib

if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)

copy /y BlueJ2534.ini bin\ >nul
echo.
echo Build OK: bin\BlueJ2534.dll
dumpbin /nologo /exports bin\BlueJ2534.dll | findstr /i passthru
endlocal
