#pragma once

#include <stdint.h>

// Exponenciální odstup opakovaných pokusů: min, 2*min, 4*min ... max.
// Bez Arduino API - testy v test/test_core.
class Backoff
{
public:
    Backoff(uint32_t minMs, uint32_t maxMs)
        : minMs_(minMs), maxMs_(maxMs), delayMs_(minMs)
    {
    }

    void reset()
    {
        failures_ = 0;
        delayMs_ = minMs_;
    }

    void failed()
    {
        if (failures_ < UINT8_MAX) failures_++;
        delayMs_ = delayMs_ * 2 > maxMs_ ? maxMs_ : delayMs_ * 2;
    }

    uint32_t delayMs() const { return delayMs_; }
    // Počet neúspěchů v řadě (nasycuje se na 255).
    uint8_t failures() const { return failures_; }

private:
    uint32_t minMs_;
    uint32_t maxMs_;
    uint32_t delayMs_;
    uint8_t failures_ = 0;
};
