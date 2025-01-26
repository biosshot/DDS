
#pragma once

#ifndef HC4051D_H
#define HC4051D_H

#include <inttypes.h>
#include <Arduino.h>

class HC4051D
{
    uint8_t a;
    uint8_t b;
    uint8_t c;

public:
    HC4051D(uint8_t a, uint8_t b, uint8_t c) : a(a), b(b), c(c)
    {
        if (a)
            pinMode(a, OUTPUT);
        if (b)
            pinMode(b, OUTPUT);
        if (c)
            pinMode(c, OUTPUT);
    }

    void set_channel(uint8_t ch)
    {
        if (ch < 8)
        {
            digitalWrite(a, bitRead(ch, 0));
            digitalWrite(b, bitRead(ch, 1));
            digitalWrite(c, bitRead(ch, 2));
        }
        else
        {
            Serial.printf("HC4051D: error channel %d not avalible", ch);
        }
    }
};

#endif HC4051sD_H