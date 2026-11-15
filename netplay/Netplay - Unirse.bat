@echo off
rem Se une a una partida de netplay hospedada por otro.
setlocal
cd /d "%~dp0.."
set "EXE=build\win-x64-release\defjam_recomp.exe"
if not exist "%EXE%" (
  echo No encuentro %EXE%. Compila el juego primero.
  pause
  exit /b 1
)
set "HOSTIP="
set /p HOSTIP=IP de Radmin del que hospeda (ej. 26.12.34.56): 
if "%HOSTIP%"=="" (
  echo Necesito la IP del host.
  pause
  exit /b 1
)
set "RECOMP_NETPLAY=join"
set "RECOMP_NETPLAY_ADDR=%HOSTIP%"
if "%RECOMP_NETPLAY_NAME%"=="" set "RECOMP_NETPLAY_NAME=%USERNAME%"
echo.
echo Conectando con %HOSTIP%:27015 ...
echo.
"%EXE%"
