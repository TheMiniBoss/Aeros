@echo off
rem Build and run the standalone tests with MSVC.
rem Run from "x64 Native Tools Command Prompt for VS" (cl.exe in PATH).
cd /d "%~dp0"
set SRC=..\..\Source\AerosWeather
set FLAGS=/nologo /std:c++20 /O2 /EHsc /permissive- /Ishim /I%SRC%\Public /I%SRC%\Private
set CORE=%SRC%\Private\Core\AerosSimulation.cpp %SRC%\Private\Core\AerosSnapshot.cpp
if not exist bin mkdir bin
cl %FLAGS% /Fe:bin\run_automation_tests.exe /Fo:bin\ run_automation_tests.cpp %CORE% || exit /b 1
cl %FLAGS% /Fe:bin\scenarios.exe /Fo:bin\ scenarios.cpp %CORE% || exit /b 1
cl %FLAGS% /Fe:bin\hazards.exe /Fo:bin\ hazards.cpp %CORE% || exit /b 1
bin\run_automation_tests.exe
