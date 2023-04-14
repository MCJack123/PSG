#include <hardware/i2c.h>
#include <cstdint>

class MCP3428 {
    int8_t address;
    i2c_inst_t * i2c;
    uint8_t configMask;
    int16_t reg[4];
public:
    MCP3428(int8_t addr = 0x6E, i2c_inst_t * port = i2c0, int bits = 12, int gain = 1);
    uint8_t scan();
    int16_t get(int channel);
};