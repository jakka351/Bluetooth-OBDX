@echo off
rem Build BlueManager.exe - x86 (must match the 32-bit DLL it loads),
rem targeting .NET Framework 4.8.1.
setlocal

set "CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe"
set "REF=%ProgramFiles(x86)%\Reference Assemblies\Microsoft\Framework\.NETFramework\v4.8.1"

if exist "%CSC%" goto have_csc
echo csc.exe not found - .NET Framework 4.x is required.
exit /b 1
:have_csc

if exist "%REF%\mscorlib.dll" goto have_ref
echo .NET Framework 4.8.1 reference assemblies not found at:
echo   "%REF%"
exit /b 1
:have_ref

if not exist bin mkdir bin

"%CSC%" /nologo /noconfig /nostdlib+ /target:winexe /platform:x86 /optimize+ ^
    /out:bin\BlueManager.exe ^
    /win32manifest:manager\app.manifest ^
    /r:"%REF%\mscorlib.dll" ^
    /r:"%REF%\System.dll" ^
    /r:"%REF%\System.Core.dll" ^
    /r:"%REF%\System.Drawing.dll" ^
    /r:"%REF%\System.Windows.Forms.dll" ^
    manager\Manager.cs manager\Logo.cs

if errorlevel 1 goto failed

copy /y manager\app.config bin\BlueManager.exe.config >nul
echo.
echo Build OK: bin\BlueManager.exe (x86, .NET Framework 4.8.1)
exit /b 0

:failed
echo MANAGER BUILD FAILED
exit /b 1
