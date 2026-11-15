@echo off
title Def Jam netplay - Unirse
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0netplay.ps1" -Mode join
pause
