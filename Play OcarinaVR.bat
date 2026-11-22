@echo off
rem OcarinaVR: starts the game in VR. Before: put your headset on and start its link with the PC
rem (Meta Quest: Quest Link by cable or Air Link). Sit down and look straight ahead: that becomes "forward".
rem Without a headset the game starts normally, flat on your screen.
rem Keys in the game's window: P first / third person, V in the game / big screen, G recentre,
rem O the same picture for both eyes (rests the eyes).
set LONGSHOT_NO_UPDATE_CHECK=1
set HOS_OUTPUT_RES=2
set RX_VK_ANISO=16
set HOS_DYNRES=0
set RX_VK_DLSS=0
set RX_VK_FSR=0
set RX_VK_XESS=0
set RX_VK_FRAMEGEN=0
set HOS_INPUT_BACKGROUND=1
set RETRACED_VR=1
if not exist "%~dp0game\longshot.exe" (
  echo The game is not built yet: double-click "Install OcarinaVR.bat" first.
  pause
  exit /b 1
)
cd /d "%~dp0game"
start "" "%~dp0game\longshot.exe"
