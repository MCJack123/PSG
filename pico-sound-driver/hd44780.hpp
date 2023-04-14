#ifndef HD44780_HPP
#define HD44780_HPP
#include <cstdint>
#include <hardware/i2c.h>

class HD44780 {
    bool isConnected;
    i2c_inst_t * i2c;
    int8_t address;
    uint8_t bl;
    uint64_t time = 0;
    void writeData(uint8_t data, uint8_t mode = 0);
public:
    HD44780(int8_t addr = 0x27, i2c_inst_t * port = i2c0);
    void clear();
    void reset();
    void setDisplay(bool cursor = false, bool blink = false, bool display = true, bool increment = true, bool shift = false);
    void setBacklight(bool backlight = true);
    void moveDisplay(bool left = false, bool shift = false);
    void put(char c);
    void write(const char * str, size_t len = 0);
    void resetCursor();
    void setCursor(int x, int y);
    void writeCharacter(uint8_t index, uint8_t * data);
};

#endif