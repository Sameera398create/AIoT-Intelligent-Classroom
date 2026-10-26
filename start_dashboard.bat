@echo off
cd /d "%~dp0"
echo ====== AIoT CLASSROOM : LIVE DASHBOARD ======
echo.
echo Close Arduino IDE Serial Monitor / Serial Plotter first.
echo Keep ACS712 current terminals WITHOUT a load when USB connects.
echo.
python -m pip install -r requirements.txt
if errorlevel 1 (
  echo Please install Python 3, then run this file again.
  pause
  exit /b 1
)
echo.
python dashboard_server.py --list
echo.
set /p ARDUINO_PORT=Enter your Arduino COM port (example COM6): 
if "%ARDUINO_PORT%"=="" (
 echo A COM port is required.
 pause
 exit /b 1
)
echo.
echo Once the server starts, open http://127.0.0.1:8000 in your browser.
echo.
python dashboard_server.py --port "%ARDUINO_PORT%"
pause
