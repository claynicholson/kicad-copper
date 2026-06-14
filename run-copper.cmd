@echo off
REM Launch the locally-built, self-contained KiCad Copper (staging tree).
REM All DLLs are co-located in staging\bin, so no PATH setup is needed.
REM Double-click this file to run.

REM Fresh config dir so the Copper Dark theme seeds on first run.
REM Delete this line (or point it at your normal config) once you've seen it.
set "KICAD_CONFIG_HOME=C:\Users\claya\copper-fresh-cfg"

cd /d "%~dp0build\release\staging\bin"
start "" "kicad.exe"
