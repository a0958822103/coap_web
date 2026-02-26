#ifndef BATTERY_PROTOCOL_H
#define BATTERY_PROTOCOL_H

#include <Arduino.h>
#pragma pack(push, 1)

struct BatteryPacket {
    uint16_t voltage;   // Byte 0-1 (mV)
    int16_t  current;   // Byte 2-3 (mA)
    uint8_t  status;    // Byte 4 (0:Off, 1:On)
    int8_t   temp;      // Byte 5 (degC)
    uint32_t timestamp; // Byte 6-9 (Unix s)
};
#pragma pack(pop)

#endif