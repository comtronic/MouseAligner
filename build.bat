@echo off
setlocal enabledelayedexpansion

echo === MouseAligner build ===

rem ---- build C++ ----
echo.
echo [1/2] Building C++ version...

set CPP_SRC=%~dp0cpp\src\MouseAligner.cpp
set CPP_OUTDIR=%~dp0cpp\build
set CPP_EXE=%CPP_OUTDIR%\MouseAligner.exe

if not exist "%CPP_OUTDIR%" mkdir "%CPP_OUTDIR%"

where cl >nul 2>nul
if errorlevel 1 (
  echo cl.exe not found in PATH.
  echo Open "x64 Native Tools Command Prompt for VS" and run build.bat again.
) else (
  cl /nologo /EHsc /std:c++17 "%CPP_SRC%" user32.lib shcore.lib ^
    /Fe:"%CPP_EXE%" /Fo:"%CPP_OUTDIR%\MouseAligner.obj"
  if errorlevel 1 (
    echo C++ build failed.
    exit /b 1
  ) else (
    echo C++ build OK: %CPP_EXE%
  )
)

rem ---- build dotnet ----
echo.
echo [2/2] Building .NET version...

pushd "%~dp0dotnet\MouseAligner"
dotnet build -c Release
if errorlevel 1 (
  popd
  echo .NET build failed.
  exit /b 1
)
popd

echo.
echo All done.
exit /b 0
