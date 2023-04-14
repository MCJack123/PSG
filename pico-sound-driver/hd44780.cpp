#include "hd44780.hpp"
#include <pico/time.h>
#include <cstring>

// commands
#define LCD_CLEARDISPLAY 0x01
#define LCD_RETURNHOME 0x02
#define LCD_ENTRYMODESET 0x04
#define LCD_DISPLAYCONTROL 0x08
#define LCD_CURSORSHIFT 0x10
#define LCD_FUNCTIONSET 0x20
#define LCD_SETCGRAMADDR 0x40
#define LCD_SETDDRAMADDR 0x80

// flags for display entry mode
#define LCD_ENTRYRIGHT 0x00
#define LCD_ENTRYLEFT 0x02
#define LCD_ENTRYSHIFTINCREMENT 0x01
#define LCD_ENTRYSHIFTDECREMENT 0x00

// flags for display on/off control
#define LCD_DISPLAYON 0x04
#define LCD_DISPLAYOFF 0x00
#define LCD_CURSORON 0x02
#define LCD_CURSOROFF 0x00
#define LCD_BLINKON 0x01
#define LCD_BLINKOFF 0x00

// flags for display/cursor shift
#define LCD_DISPLAYMOVE 0x08
#define LCD_CURSORMOVE 0x00
#define LCD_MOVERIGHT 0x04
#define LCD_MOVELEFT 0x00

// flags for function set
#define LCD_8BITMODE 0x10
#define LCD_4BITMODE 0x00
#define LCD_2LINE 0x08
#define LCD_1LINE 0x00
#define LCD_5x10DOTS 0x04
#define LCD_5x8DOTS 0x00

// flags for backlight control
#define LCD_BACKLIGHT 0x08
#define LCD_NOBACKLIGHT 0x00

#define En 0b00000100 // Enable bit
#define Rw 0b00000010 // Read/Write bit
#define Rs 0b00000001 // Register select bit

#define W(v, d) (v = d, &v)

HD44780::HD44780(int8_t addr, i2c_inst_t * port): i2c(port), address(addr), bl(LCD_BACKLIGHT) {
    writeData(0x03);
    writeData(0x03);
    writeData(0x03);
    writeData(0x02);
    reset();
}

void HD44780::writeData(uint8_t data, uint8_t mode) {
    if (!isConnected) return;
    if (time_us_64() < time) sleep_us(time - time_us_64());
    uint8_t d;
    if (i2c_write_timeout_us(i2c, address, W(d, mode | bl | (data & 0xF0)), 1, false, 250) != 1) {isConnected = false; return;}
    if (i2c_write_timeout_us(i2c, address, W(d, mode | bl | (data & 0xF0) | En), 1, false, 250) != 1) {isConnected = false; return;}
    sleep_us(500);
    if (i2c_write_timeout_us(i2c, address, W(d, (mode | bl | (data & 0xF0) & ~En)), 1, false, 250) != 1) {isConnected = false; return;}
    sleep_us(100);
    if (i2c_write_timeout_us(i2c, address, W(d, mode | bl | ((data << 4) & 0xF0)), 1, false, 250) != 1) {isConnected = false; return;}
    if (i2c_write_timeout_us(i2c, address, W(d, mode | bl | ((data << 4) & 0xF0) | En), 1, false, 250) != 1) {isConnected = false; return;}
    sleep_us(500);
    if (i2c_write_timeout_us(i2c, address, W(d, (mode | bl | ((data << 4) & 0xF0) & ~En)), 1, false, 250) != 1) {isConnected = false; return;}
    sleep_us(100);
}

void HD44780::clear() {
    writeData(LCD_CLEARDISPLAY);
    time = time_us_64() + 37;
}

void HD44780::reset() {
    writeData(LCD_FUNCTIONSET | LCD_2LINE | LCD_5x8DOTS | LCD_4BITMODE);
    sleep_us(37);
    setDisplay();
    clear();
    resetCursor();
}

void HD44780::setDisplay(bool cursor, bool blink, bool display, bool increment, bool shift) {
    writeData(LCD_DISPLAYCONTROL | (cursor ? LCD_CURSORON : LCD_CURSOROFF) | (blink ? LCD_BLINKON : LCD_BLINKOFF) | (display ? LCD_DISPLAYON : LCD_DISPLAYOFF));
    sleep_us(37);
    writeData(LCD_ENTRYMODESET | (increment ? LCD_ENTRYRIGHT : LCD_ENTRYLEFT) | (shift ? LCD_ENTRYSHIFTINCREMENT : LCD_ENTRYSHIFTDECREMENT));
    time = time_us_64() + 37;
}

void HD44780::setBacklight(bool backlight) {
    bl = backlight ? LCD_BACKLIGHT : LCD_NOBACKLIGHT;
    writeData(0);
}

void HD44780::moveDisplay(bool left, bool shift) {
    writeData(LCD_CURSORSHIFT | (shift ? 0x08 : 0) | (left ? 0x04 : 0));
    time = time_us_64() + 37;
}

void HD44780::put(char c) {
    writeData(c, Rs);
    time = time_us_64() + 37;
}

void HD44780::write(const char * str, size_t len) {
    if (len == 0) len = strlen(str);
    for (int i = 0; i < len; i++) put(str[i]);
}

void HD44780::resetCursor() {
    writeData(LCD_RETURNHOME);
    time = time_us_64() + 1520;
}

void HD44780::setCursor(int x, int y) {
    writeData(LCD_SETDDRAMADDR | (x & 0x3F) | ((y << 6) & 0x40));
    time = time_us_64() + 37;
}

void HD44780::writeCharacter(uint8_t index, uint8_t * data) {
    writeData(LCD_SETCGRAMADDR | ((index << 3) & 0x3F));
    sleep_us(37);
    for (int i = 0; i < 8; i++) {
        writeData(data[i], Rs);
        sleep_us(37);
    }
    writeData(LCD_SETDDRAMADDR | 0);
    time = time_us_64() + 37;
}