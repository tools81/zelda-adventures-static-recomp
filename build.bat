@echo off
rem Build a CD-i static recompilation.
rem Requirements: Python 3, CMake 3.19+, Visual Studio 2022 (C compiler), and the
rem game's disc image (.bin/.cue) in the rom\ folder.  Per-game settings: game.json
setlocal
cd /d "%~dp0"

for /f "delims=" %%n in ('python -c "import json;print(json.load(open('game.json')).get('exe_name','cdigame'))"') do set EXE=%%n
if "%EXE%"=="" goto :error

echo [1/3] Recompiling 68000 code to C...
python tools\gen.py || goto :error

echo [2/3] Configuring...
cmake -S . -B build\cmake -G "Visual Studio 17 2022" -A x64 > nul || goto :error

echo [3/3] Compiling...
cmake --build build\cmake --config Release --target game game_hd --parallel || goto :error

copy /y build\cmake\Release\%EXE%.exe %EXE%.exe > nul
copy /y build\cmake\Release\%EXE%_hd.exe %EXE%_hd.exe > nul
echo.
echo Done. Run %EXE%.exe (original picture) or %EXE%_hd.exe (Cartoon HD upscaling) to play.
exit /b 0

:error
echo Build failed.
exit /b 1
