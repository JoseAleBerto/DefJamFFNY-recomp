@echo off
rem Hospeda una partida de netplay. Los demas se conectan a tu IP de Radmin VPN.
setlocal
cd /d "%~dp0.."
set "EXE=build\win-x64-release\defjam_recomp.exe"
if not exist "%EXE%" (
  echo No encuentro %EXE%. Compila el juego primero.
  pause
  exit /b 1
)
set "PLAYERS="
set /p PLAYERS=Cuantos jugadores en total, contandote a ti (2-4) [2]: 
if "%PLAYERS%"=="" set PLAYERS=2
set "RECOMP_NETPLAY=host"
set "RECOMP_NETPLAY_PLAYERS=%PLAYERS%"
if "%RECOMP_NETPLAY_NAME%"=="" set "RECOMP_NETPLAY_NAME=%USERNAME%"
echo.
echo Esperando a %PLAYERS% jugadores en el puerto UDP 27015.
echo Pasales tu IP de Radmin VPN (la que empieza por 26.).
echo El juego arranca cuando se conecten todos.
echo.
"%EXE%"
