@echo off
title Def Jam - Recoger reporte
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0collect-report.ps1" 
pause
