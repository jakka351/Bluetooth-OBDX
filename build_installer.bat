@echo off
rem Build BlueJ2534Setup.exe (GUI installer) targeting .NET Framework 4.8.1
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
echo Install the ".NET Framework 4.8.1 Developer Pack" or adjust REF.
exit /b 1
:have_ref

if not exist bin mkdir bin

"%CSC%" /nologo /noconfig /nostdlib+ /target:winexe /platform:anycpu /optimize+ ^
    /out:bin\BlueJ2534Setup.exe ^
    /win32manifest:installer\app.manifest ^
    /r:"%REF%\mscorlib.dll" ^
    /r:"%REF%\System.dll" ^
    /r:"%REF%\System.Core.dll" ^
    /r:"%REF%\System.Drawing.dll" ^
    /r:"%REF%\System.Windows.Forms.dll" ^
    installer\Setup.cs

if errorlevel 1 goto failed

copy /y installer\app.config bin\BlueJ2534Setup.exe.config >nul
echo.
echo Build OK: bin\BlueJ2534Setup.exe (.NET Framework 4.8.1)
exit /b 0

:failed
echo INSTALLER BUILD FAILED
exit /b 1
