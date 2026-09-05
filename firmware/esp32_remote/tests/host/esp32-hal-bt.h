#pragma once
inline bool testBluetoothOn = true;
inline bool btStarted() { return testBluetoothOn; }
inline bool btStop() { testBluetoothOn = false; return true; }
