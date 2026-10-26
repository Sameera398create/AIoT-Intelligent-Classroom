AIoT-BASED INTELLIGENT CLASSROOM - COMPLETE ARDUINO UNO + LOCAL DASHBOARD
============================================================================

PIN CONNECTIONS (unchanged from your successful tests):
  DHT11 DATA -> D2            SW-420 DO -> D3
  IR module DO -> D4          MQ gas AO -> A0
  LDR module AO -> A1         ACS712 OUT -> A2
  All modules share ground; use the correct voltage for each module.

A. ARDUINO IDE
  1. Extract this ZIP.
  2. Open Arduino/AIoT_Classroom_Live/AIoT_Classroom_Live.ino.
  3. Install Adafruit DHT sensor library and Adafruit Unified Sensor.
  4. Confirm ACS712 sensitivity (5A 0.185; 20A 0.100; 30A 0.066 V/A).
  5. Select Arduino Uno & correct COM port in Arduino IDE, then Upload.
  6. Open Arduino Serial Monitor at 115200 baud.
     Every 2 seconds: readable report + DATA CSV line.
  7. For the dashboard CLOSE Arduino Serial Monitor and Serial Plotter.

B. WINDOWS LOCAL DASHBOARD
  1. Install Python 3 if needed. During installation, enable 'Add Python to PATH'.
  2. Open dashboard/start_dashboard.bat. The launcher installs pyserial,
     lists COM ports, and asks you to enter the Arduino COM port.
  3. Open http://127.0.0.1:8000 if your browser did not open automatically.
  4. Your Arduino sensor values appear live every 2 seconds. The console
     running Python also MIRRORS THE SAME HUMAN-READABLE ARDUINO OUTPUT.

Alternative manual run in CMD (from dashboard/ folder):
  python -m pip install -r requirements.txt
  python dashboard_server.py --list
  python dashboard_server.py --port COM6
  (Replace COM6 with YOUR actual Arduino port.)

LIMITATION: Serial Monitor and Python dashboard cannot ordinarily open the
same Arduino Uno COM port simultaneously on Windows. Use Serial Monitor to
verify, CLOSE IT, then start the Python dashboard. When dashboard is running,
its command window echoes everything Arduino would show in Serial Monitor.

Important:
- This is a REAL hardware dashboard. NO simulated/live-fake sensor values.
- ACS712 current must be VERIFIED with a real, safe, low-voltage load.
  With only ACS712 VCC/GND/OUT and no load in the IP+/- current path, display
  is NO LOAD, not an invented current. The sensitivity/deadband may hide
  currents smaller than your sensor can reliably measure.
- ACS712 is zero-calibrated whenever Arduino resets. Keep its CURRENT PATH
  UNLOADED during startup and when opening the Python serial port (which
  typically resets Arduino). Do not use unsafe mains wiring on a breadboard.
- Gas = RAW ANALOG 0..1023, NOT ppm. Threshold is demonstration-only and NOT a
  life safety / gas leak warning. MQ sensors need warmup and calibration.
- IR detection events are not human counts; the module senses only nearby
  objects in its field of view.
- Vibration DETECTED is retained for 4 seconds after an interrupt.
- The dashboard provides rule-based activity indication / possible energy
  waste warning after 20 seconds of no sensed activity while a real load is
  measured. NOT a trained AI occupancy prediction. A genuine occupancy model
  comes later, after collecting and labelling datasets.
- If brightness seems reversed, flip LDR_HIGH_IS_BRIGHT in sketch.
- If IR/vibration seems reversed, change active-level configuration.
- If readings do not appear, check COM port, 115200 baud, cable, and USB
  drivers, and close any application using the COM port.

OUTPUT FORMAT: DATA,ms,tempC,humidityPct,gasRaw,ldrRaw,currentA_or_NO_LOAD,
  loadStatus,vibration(0/1),irStatus,irEvents,gasStatus,lightStatus

This dashboard uses pyserial + built-in Python HTTP server; works offline.
