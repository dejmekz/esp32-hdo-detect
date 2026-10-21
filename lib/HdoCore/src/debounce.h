#pragma once

#include <stdint.h>

// Debounce úrovňového vstupu: nový stav se převezme, až je vstup
// stabilní debounceMs. Vstup se čte v každém průchodu smyčkou, takže
// nehrozí zaseknutí stavu kvůli zmeškané hraně.
// Bez Arduino API - testy v test/test_core.
class Debouncer
{
public:
    explicit Debouncer(uint32_t debounceMs) : debounceMs_(debounceMs) {}

    void reset(bool state, uint32_t now)
    {
        state_ = state;
        raw_ = state;
        rawChangeTime_ = now;
    }

    // true = odfiltrovaný stav se právě změnil.
    bool update(bool raw, uint32_t now)
    {
        if (raw != raw_) {
            raw_ = raw;
            rawChangeTime_ = now;
            return false;
        }

        if (raw == state_ || now - rawChangeTime_ < debounceMs_) return false;

        state_ = raw;
        return true;
    }

    bool state() const { return state_; }

private:
    uint32_t debounceMs_;
    bool state_ = false;
    bool raw_ = false;
    uint32_t rawChangeTime_ = 0;
};
