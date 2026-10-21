#pragma once

#include <stdint.h>

// Vstup HDO (optočlen PC817 na HDO_PIN).

void hdoSetup();

// Polling s debounce, volat v každém průchodu smyčkou.
// true = tarif se právě změnil.
bool hdoUpdate();

// HDO aktivní = nízký tarif (NT), jinak vysoký tarif (VT).
bool hdoLowTariff();
const char* hdoTariffText();

// Jak dlouho (s) trvá aktuální tarif: od poslední změny, nebo od startu.
uint32_t hdoTariffSeconds();
