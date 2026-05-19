@echo off
setlocal

call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" || exit /b 1

if not exist build mkdir build

cl ^
  /nologo ^
  /std:c++17 ^
  /EHsc ^
  /W4 ^
  /O2 ^
  /DUNICODE ^
  /D_UNICODE ^
  /DWIN32_LEAN_AND_MEAN ^
  /c lowdpi.cpp ^
  /Fo:build\lowdpi.obj || exit /b 1

link ^
  /NOLOGO ^
  /SUBSYSTEM:WINDOWS ^
  /OUT:build\lowdpi.exe ^
  build\lowdpi.obj ^
  gdi32.lib ^
  ole32.lib ^
  user32.lib ^
  windowscodecs.lib || exit /b 1

echo Built build\lowdpi.exe
