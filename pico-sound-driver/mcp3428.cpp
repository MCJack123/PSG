#include "mcp3428.hpp"

MCP3428::MCP3428(int8_t addr, i2c_inst_t * port, int bits, int gain): address(addr), i2c(port) {
    switch (bits) {
        case 16: configMask = 0x08; break;
        case 14: configMask = 0x04; break;
        default: configMask = 0x00; break;
    }
    switch (gain) {
        case 2: configMask |= 0x01; break;
        case 4: configMask |= 0x02; break;
        case 8: configMask |= 0x03; break;
    }
    i2c_write_timeout_us(i2c, address, &configMask, 1, false, 50);
}

uint8_t MCP3428::scan() {
    uint8_t retval = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t data = 0x80 | (i << 5) | configMask;
        if (i2c_write_timeout_us(i2c, address, &data, 1, false, 500) != 1) return 0;
        uint8_t recv[3] = {0, 0, 0x80};
        while (recv[2] & 0x80) if (i2c_read_timeout_us(i2c, address, recv, 3, false, 500) != 3) return 0;
        int16_t res;
        if (configMask & 0x08) res = recv[0] << 8 | recv[1];
        else if (configMask & 0x04) res = recv[0] << 10 | recv[1] << 2;
        else res = recv[0] << 12 | recv[1] << 4;
        if (res != reg[i]) retval |= 1 << i;
        reg[i] = res;
    }
    return retval;
}

int16_t MCP3428::get(int channel) {
    if (channel < 0 || channel > 3) return 0;
    return reg[channel & 3];
}