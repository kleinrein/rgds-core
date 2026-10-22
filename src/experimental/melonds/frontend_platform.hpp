#pragma once
#include <cstdint>
namespace rgds_melonds {
void stop(int reason);
void mic_start();
void mic_stop();
int mic_read(int16_t* data, int maximum);
void battery_write(const uint8_t* data, uint32_t length, uint32_t offset, uint32_t count);
}
