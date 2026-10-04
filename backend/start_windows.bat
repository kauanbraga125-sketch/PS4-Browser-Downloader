@echo off
cd /d %~dp0
where node >nul 2>nul
if errorlevel 1 (
  echo Instale Node.js 18 ou superior em https://nodejs.org/
  pause
  exit /b 1
)
if not exist node_modules call npm install
call npx playwright install chromium
call npm start
pause
