#pragma once

// Restart na povel (MQTT hdo/cmd). Značka v RTC paměti přežije
// softwarový restart, takže po startu lze tento restart odlišit
// od ostatních (OTA, pád).

// Nastaví značku a restartuje zařízení, nevrací se.
void restartOnCommand();

// true = předchozí restart byl na povel. Volat jednou při startu,
// značku smaže.
bool restartedOnCommand();
