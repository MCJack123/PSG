/*
 * pico-sound-driver/main.cpp
 * PSG
 *
 * This file contains the firmware for the Raspberry Pi Pico. It handles parsing
 * MIDI events and converting them into commands for each channel, as well as
 * other miscellaneous functionality, such as fade out and PIC firmware updates.
 *
 * This code is licensed under the GPLv2 license.
 * Copyright (c) 2022-2023 JackMacWindows.
 */

#include <hardware/gpio.h>
#include <hardware/timer.h>
#include <hardware/watchdog.h>
#include <pico/mutex.h>
#include <pico/multicore.h>
#include <pico/bootrom.h>
#include <tusb.h>
#include <math.h>
#include <stdio.h>

#define BOARD_VERSION_MAJOR 0
#define BOARD_VERSION_MINOR 0

#define NUM_CHANNELS 16
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define gpio_out(pin) gpio_init(pin); gpio_set_dir(pin, true)
#define gpio_in(pin) gpio_init(pin); gpio_set_dir(pin, false)
#define COMMAND_WAVE_TYPE 0x00
#define COMMAND_VOLUME    0x40
#define COMMAND_FREQUENCY 0x80
#define COMMAND_CLOCK     0xC0

#define PIN_STROBE 19
#define PIN_DATA   20
#define PIN_CLOCK  21

#define sleep_us_sr(n) sleep_us((n))
#define sleep_us_pic(n) sleep_us((n))
#define volume(n) (COMMAND_VOLUME | (uint8_t)floor(13.0 * log((n) + 1) + 0.5))

// !! CLOCK MULTIPLIER CONSTANT !!
// UPDATE THIS IF MODIFYING THE RUN LENGTH OF THE LOOP CODE
#define CLOCKS_PER_LOOP 70

enum class WaveType {
    None,
    Sine,
    Triangle,
    Sawtooth,
    RSawtooth,
    Square,
    Noise,
    Custom,
    PitchedNoise
};

enum class InterpolationMode {
    None,
    Linear
};

struct ChannelInfo {
    double position = 0.0;
    WaveType wavetype = WaveType::None;
    double duty = 0.5;
    unsigned int frequency = 0;
    double amplitude = 1.0;
    float pan = 0.0;
    double fadeInit = 0.0;
    int64_t fadeStart = 0;
    int64_t fadeLength = 0;
    int fadeDirection = -1;
    double customWave[512];
    int customWaveSize;
    InterpolationMode interpolation;
    bool isLowFreq = false;
    uint8_t note = 0;
};

struct MidiPacket {
    uint8_t usbcode;
    uint8_t command;
    uint8_t param1;
    uint8_t param2;
};

ChannelInfo channels[NUM_CHANNELS];
uint8_t typeconv[9] = {0, 5, 4, 2, 3, 1, 6, 0, 6};
const double freqMultiplier = (65536.0 * CLOCKS_PER_LOOP) / 8000000.0;
uint8_t midiChannels[16][128];
WaveType midiPrograms[16] = {WaveType::Square};
uint8_t midiDuty[16] = {128};
uint8_t midiUsedChannels[NUM_CHANNELS] = {0xFF};
bool midiMode = true;
uint8_t command_queue[NUM_CHANNELS][4][2];
bool command_updates[4] = {false};
mutex_t command_queue_lock;
bool changed = false;
uint8_t freq_lsb[NUM_CHANNELS] = {0};
char hex_storage[0x1000];
uint16_t hex_storage_size = 0;
uint8_t inSysEx = 0;
uint16_t sysExSize;
uint8_t version_major, version_minor;

template<typename T> static T min(T a, T b) {return a < b ? a : b;}
template<typename T> static T max(T a, T b) {return a > b ? a : b;}

void write_data(uint8_t c, uint8_t data) {
    gpio_put(6, data & 0x80);
    gpio_put(7, data & 0x40);
    gpio_put(8, data & 0x20);
    gpio_put(9, data & 0x10);
    gpio_put(10, data & 0x08);
    gpio_put(11, data & 0x04);
    gpio_put(12, data & 0x02);
    gpio_put(13, data & 0x01);
    sleep_us_pic(channels[c].isLowFreq ? 16 : 1);
    gpio_put(14, true);
    sleep_us_pic(channels[c].isLowFreq ? 16 : 1);
    gpio_put(14, false);
    sleep_us_pic(channels[c].isLowFreq ? 48 : 3);
}

static int htob(const char **str, const char *end) {
    int n = 0;
    if (*str >= end) return -1;
    else if (**str >= '0' && **str <= '9') n += *(*str)++ - '0';
    else if (**str >= 'A' && **str <= 'F') n += *(*str)++ - 'A' + 10;
    else if (**str >= 'a' && **str <= 'f') n += *(*str)++ - 'a' + 10;
    else return -1;
    n <<= 4;
    if (*str >= end) return -1;
    else if (**str >= '0' && **str <= '9') n += *(*str)++ - '0';
    else if (**str >= 'A' && **str <= 'F') n += *(*str)++ - 'A' + 10;
    else if (**str >= 'a' && **str <= 'f') n += *(*str)++ - 'a' + 10;
    else return -1;
    return n;
}

int loadhex(const char *data, size_t size) {
    uint16_t program[0x800];
    struct {uint16_t start, end;} program_extents[16];
    uint8_t max_extent = 0;
    const char *end = data + size;
    uint16_t addr_hi = 0;
    memset(program, 0, 0x800);
    gpio_put(PICO_DEFAULT_LED_PIN, false);
    while (data < end && *data) {
        if (*data++ != ':') continue;
        uint8_t bc = htob(&data, end);
        if (bc == -1) return -3;
        bc >>= 1;
        uint16_t addr_h = htob(&data, end);
        if (addr_h == -1) return -4;
        uint16_t addr = htob(&data, end);
        if (addr == -1) return -5;
        addr |= addr_h << 8;
        addr >>= 1;
        uint8_t rec = htob(&data, end);
        if (rec == -1) return -6;
        switch (rec) {
            case 0: {
                for (int i = 0; i < bc; i++) {
                    if (addr_hi == 0) {
                        int l = htob(&data, end);
                        if (l == -1) return -7;
                        int h = htob(&data, end);
                        if (h == -1) return -8;
                        if (addr + i < 0x800) program[addr + i] = h << 8 | l;
                    } else {htob(&data, end); htob(&data, end);}
                }
                bool found = false;
                for (int i = 0; i < max_extent; i++) {if (program_extents[i].end == addr) {program_extents[i].end += bc; found = true; break;}}
                if (!found && max_extent < 16) program_extents[max_extent++] = {addr, (uint16_t)(addr + bc)};
                break;
            }
            case 1: {
                // execute programming
                // collapse extents over the same rows
                bool loop;
                do {
                    loop = false;
                    for (int i = 0; i < max_extent; i++) {
                        for (int j = 0; j < max_extent; j++) {
                            if ((program_extents[i].end & 0x7F0) == (program_extents[j].start & 0x7F0)) {
                                program_extents[i].end = program_extents[j].end;
                                for (int k = j + 1; k < max_extent; k++) program_extents[k-1] = program_extents[k];
                                max_extent--;
                                loop = true;
                            } else if ((program_extents[j].end & 0x7F0) == (program_extents[i].start & 0x7F0)) {
                                program_extents[j].end = program_extents[i].end;
                                for (int k = i + 1; k < max_extent; k++) program_extents[k-1] = program_extents[k];
                                max_extent--;
                                loop = true;
                            }
                        }
                    }
                } while (loop);
                // flip all chips into bootloader mode
                gpio_put(PIN_DATA, true);
                sleep_us(1);
                gpio_put(PIN_CLOCK, true);
                sleep_us(1);
                gpio_put(PIN_CLOCK, false);
                sleep_us(1);
                gpio_put(PIN_DATA, false);
                sleep_us(1);
                for (int i = 0; i < NUM_CHANNELS; i++) {
                    gpio_put(PIN_STROBE, true);
                    sleep_us(1);
                    gpio_put(PIN_STROBE, false);
                    gpio_put(PIN_CLOCK, true);
                    sleep_us(1);
                    gpio_put(PIN_CLOCK, false);
                    sleep_us(1);
                }
                gpio_put(PIN_STROBE, true);
                sleep_us(1);
                gpio_put(PIN_STROBE, false);
                channels[0].isLowFreq = true; // run slower for safety
                write_data(0, 0xFF); // system command
                write_data(0, 0x01); // enter bootloader
                for (int i = 0; i < max_extent; i++) {
                    for (uint16_t addr = program_extents[i].start & 0x7F0; addr < program_extents[i].end; addr += 0x10) {
                        uint8_t len = min(program_extents[i].end - addr, 0x10);
                        if (addr + len >= 0x700) continue; // don't overwrite bootloader
                        write_data(0, len << 1);
                        write_data(0, addr >> 7);
                        write_data(0, addr << 1);
                        write_data(0, 0); // write data
                        sleep_ms(5); // wait for erase
                        for (int j = addr; j < addr + len; j++) {
                            write_data(0, program[j] & 0xFF);
                            write_data(0, program[j] >> 8);
                        }
                        sleep_ms(5); // wait for write
                        write_data(0, 0); // checksum is ignored
                    }
                }
                // send end code
                write_data(0, 0);
                write_data(0, 0);
                write_data(0, 0);
                write_data(0, 1);
                write_data(0, 0xFF);
                // notify host
                MidiPacket p = {0x0F, 0xFF, 0, 0};
                tud_midi_packet_write((uint8_t*)&p);
                sleep_ms(5); // is this needed?
                // reset
                //(*((volatile uint32_t*)(PPB_BASE + 0x0ED0C))) = 0x5FA0004;
                // we should be done by now, but just in case:
                watchdog_enable(1, 1);
                while (true);
            }
            case 4: {
                int haddr_h = htob(&data, end);
                if (haddr_h == -1) return -9;
                int haddr = htob(&data, end);
                if (haddr == -1) return -10;
                addr_hi = (haddr_h << 8) | haddr;
                break;
            }
            default: return -11;
        }
        htob(&data, end); // checksum
    }
    return -12;
}

void tud_midi_rx_cb(uint8_t itf) {
    mutex_enter_blocking(&command_queue_lock);
    while (tud_midi_available()) {
        MidiPacket packet;
        tud_midi_packet_read((uint8_t*)&packet);
        if ((packet.usbcode & 0x0C) == 0x04) {
            // sysex
            if (inSysEx == 0) { // start
                if (packet.command == 0xF0 && packet.param1 == 0x00 && packet.param2 == 0x46) inSysEx = 0xFE;
                else inSysEx = 0xFF;
            } else if (inSysEx == 0xFE) { // packet header
                if (packet.command != 0x71) {
                    inSysEx = 0xFF;
                    continue;
                }
                inSysEx = packet.param1 + 1;
                // ignore param2
                if (inSysEx == 1) {
                    memset(hex_storage, 0, 0x1000);
                    hex_storage_size = 0;
                } else if (inSysEx == 2) {
                    // boot to bootloader
                    reset_usb_boot(1 << PICO_DEFAULT_LED_PIN, 0); // no return
                }
            } else if (inSysEx == 1) {
                // flash PIC chips - load HEX
                uint8_t s = packet.usbcode & 0x03;
                if (s != 1) hex_storage[hex_storage_size++] = packet.command;
                if (s == 0 || s == 3) hex_storage[hex_storage_size++] = packet.param1;
                if (s == 0) hex_storage[hex_storage_size++] = packet.param2;
                if (s != 0) {
                    inSysEx = 0;
                    loadhex(hex_storage, hex_storage_size);
                }
            } else { // unrecognized vendor/command
                if (packet.usbcode & 0x03) inSysEx = 0;
            }
            continue;
        }
        uint8_t channel = packet.command & 0x0F;
        switch (packet.command & 0xF0) {
        case 0x90: { // note on
            if (packet.param2) {
                if (midiMode) {
                    if (midiChannels[channel][packet.param1] < NUM_CHANNELS) {
                        uint8_t c = midiChannels[channel][packet.param1];
                        channels[c].amplitude = packet.param2 / 127.5;
                        command_queue[c][2][0] = volume(packet.param2);
                        command_updates[2] = true;
                        changed = true;
                        break;
                    }
                    for (int c = 0; c < NUM_CHANNELS; c++) {
                        if (midiUsedChannels[c] == 0xFF) {
                            midiUsedChannels[c] = channel;
                            midiChannels[channel][packet.param1] = c;
                            uint16_t freq = (uint16_t)floor(pow(2.0, ((double)packet.param1 - 69.0) / 12.0) * 440.0 + 0.5);
                            channels[c].amplitude = packet.param2 / 127.5;
                            channels[c].frequency = freq;
                            channels[c].wavetype = midiPrograms[channel];
                            channels[c].note = packet.param1;
                            channels[c].fadeStart = 0;
                            freq = (uint16_t)floor(freq * freqMultiplier + 0.5);
                            command_queue[c][1][0] = COMMAND_FREQUENCY | ((freq >> 8) & 0x3F);
                            command_queue[c][1][1] = freq & 0xFF;
                            command_queue[c][0][0] = COMMAND_WAVE_TYPE | typeconv[(int)midiPrograms[channel]];
                            if (midiPrograms[channel] == WaveType::Square) {
                                channels[c].duty = midiDuty[channel] / 255.0;
                                command_queue[c][0][1] = midiDuty[channel];
                            }
                            command_queue[c][2][0] = volume(packet.param2);
                            command_updates[0] = true;
                            command_updates[1] = true;
                            command_updates[2] = true;
                            changed = true;
                            break;
                        }
                    }
                } else {
                    uint16_t freq = (uint16_t)floor(pow(2.0, (packet.param1 - 69.0) / 12.0) * 440 + 0.5);
                    channels[channel].amplitude = packet.param2 / 127.5;
                    channels[channel].frequency = freq;
                    channels[channel].fadeStart = 0;
                    
                    freq = (uint16_t)floor(freq * freqMultiplier + 0.5);
                    command_queue[channel][1][0] = COMMAND_FREQUENCY | ((freq >> 8) & 0x3F);
                    command_queue[channel][1][1] = freq & 0xFF;
                    command_queue[channel][2][0] = volume(packet.param2);
                    command_updates[1] = true;
                    command_updates[2] = true;
                    changed = true;
                }
                break;
            } // fall through
        } case 0x80: { // note off
            if (packet.param2 == 0 || packet.param2 == 127) {
                if (midiMode) {
                    if (midiChannels[channel][packet.param1] < NUM_CHANNELS) {
                        uint8_t c = midiChannels[channel][packet.param1];
                        channels[c].amplitude = 0;
                        channels[c].fadeStart = 0;
                        command_queue[c][2][0] = COMMAND_VOLUME | 0;
                        command_updates[2] = true;
                        changed = true;
                        midiUsedChannels[c] = 0xFF;
                    }
                    midiChannels[channel][packet.param1] = 0xFF;
                } else {
                    channels[channel].amplitude = 0;
                    channels[channel].fadeStart = 0;
                    command_queue[channel][2][0] = COMMAND_VOLUME | 0;
                    command_updates[2] = true;
                    changed = true;
                }
            } else {
                if (midiMode) {
                    if (midiChannels[channel][packet.param1] < NUM_CHANNELS) {
                        uint8_t c = midiChannels[channel][packet.param1];
                        channels[c].fadeInit = channels[c].amplitude;
                        channels[c].fadeStart = time_us_64();
                        channels[c].fadeDirection = -1;
                        channels[c].fadeLength = (127 - packet.param2) * (1000000/64);
                    }
                    midiChannels[channel][packet.param1] = 0xFF;
                } else {
                    channels[channel].fadeInit = channels[channel].amplitude;
                    channels[channel].fadeStart = time_us_64();
                    channels[channel].fadeDirection = -1;
                    channels[channel].fadeLength = (127 - packet.param2) * (1000000/64);
                }
            }
            break;
        } case 0xA0: { // polyphony aftertouch (volume change per note)
            if (midiMode) {
                if (midiChannels[channel][packet.param1] < NUM_CHANNELS) {
                    uint8_t c = midiChannels[channel][packet.param1];
                    channels[c].amplitude = packet.param2 / 127.5;
                    command_queue[c][2][0] = volume(packet.param2);
                    command_updates[2] = true;
                    changed = true;
                }
            } else {
                // just change whole channel volume
                channels[channel].amplitude = packet.param2 / 127.5;
                command_queue[channel][2][0] = volume(packet.param2);
                command_updates[2] = true;
                changed = true;
            }
            break;
        } case 0xB0: { // control change
            switch (packet.param1) {
            case 1: { // square duty
                if (midiMode) {
                    midiDuty[channel] = packet.param2 * 2;
                    if (midiPrograms[channel] == WaveType::Square) {
                        for (int i = 0; i < 128; i++) {
                            if (midiChannels[channel][i] < NUM_CHANNELS) {
                                uint16_t c = midiChannels[channel][i];
                                channels[c].duty = packet.param2 / 127.5;
                                command_queue[c][0][0] = COMMAND_WAVE_TYPE | 1;
                                command_queue[c][0][1] = midiDuty[channel];
                                command_updates[0] = true;
                                changed = true;
                            }
                        }
                    }
                } else {
                    channels[channel].duty = packet.param2 / 127.5;
                    if (channels[channel].wavetype == WaveType::Square) {
                        command_queue[channel][0][0] = COMMAND_WAVE_TYPE | 1;
                        command_queue[channel][0][1] = packet.param2 * 2;
                        command_updates[0] = true;
                        changed = true;
                    }
                }
                break;
            } case 7: { // volume
                if (midiMode) {
                    for (int i = 0; i < 128; i++) {
                        if (midiChannels[channel][i] < NUM_CHANNELS) {
                            uint16_t c = midiChannels[channel][i];
                            channels[c].amplitude = packet.param2 / 127.5;
                            channels[c].fadeStart = 0;
                            command_queue[c][2][0] = volume(packet.param2);
                            command_updates[2] = true;
                            changed = true;
                        }
                    }
                } else {
                    channels[channel].amplitude = packet.param2 / 127.5;
                    channels[channel].fadeStart = 0;
                    command_queue[channel][2][0] = volume(packet.param2);
                    command_updates[2] = true;
                    changed = true;
                }
                break;
            } case 16: { // clock frequency adjustment
                if (midiMode) {
                    for (int i = 0; i < 128; i++) {
                        if (midiChannels[channel][i] < NUM_CHANNELS) {
                            uint16_t c = midiChannels[channel][i];
                            channels[c].isLowFreq = packet.param2 & 0x40;
                            command_queue[c][3][0] = COMMAND_CLOCK | (packet.param2 >> 4);
                            command_updates[3] = true;
                            changed = true;
                        }
                    }
                } else {
                    channels[channel].isLowFreq = packet.param2 & 0x40;
                    command_queue[channel][3][0] = COMMAND_CLOCK | (packet.param2 >> 4);
                    command_updates[3] = true;
                    changed = true;
                }
                break;
            } case 24: { // frequency (MSB)
                uint16_t freq = freq_lsb[channel] | ((uint16_t)packet.param2 << 7);
                channels[channel].frequency = freq;
                freq = (uint16_t)floor(freq * freqMultiplier + 0.5);
                command_queue[channel][1][0] = COMMAND_FREQUENCY | ((freq >> 8) & 0x3F);
                command_queue[channel][1][1] = freq & 0xFF;
                command_updates[1] = true;
                changed = true;
                break;
            } case 56: { // frequency (LSB)
                freq_lsb[channel] = packet.param2;
                uint16_t freq = freq_lsb[channel] | (channels[channel].frequency & 0xFF00);
                channels[channel].frequency = freq;
                freq = (uint16_t)floor(freq * freqMultiplier + 0.5);
                command_queue[channel][1][0] = COMMAND_FREQUENCY | ((freq >> 8) & 0x3F);
                command_queue[channel][1][1] = freq & 0xFF;
                command_updates[1] = true;
                changed = true;
                break;
            } case 123: { // all notes off
                //if (!(packet.param2 & 0x40)) break;
                if (midiMode) {
                    for (int i = 0; i < 128; i++) {
                        if (midiChannels[channel][i] < NUM_CHANNELS) {
                            uint8_t c = midiChannels[channel][i];
                            channels[c].amplitude = 0;
                            command_queue[c][2][0] = COMMAND_VOLUME | 0;
                            command_updates[2] = true;
                            changed = true;
                            midiUsedChannels[c] = 0xFF;
                        }
                        midiChannels[channel][i] = 0xFF;
                    }
                    for (int i = 0; i < 16; i++) midiUsedChannels[i] = 0xFF;
                } else {
                    channels[channel].amplitude = 0;
                    command_queue[channel][2][0] = COMMAND_VOLUME | 0;
                    command_updates[2] = true;
                    changed = true;
                }
                break;
            } case 126: { // mono mode
                midiMode = false;
                break;
            } case 127: { // poly mode
                midiMode = true;
                break;
            }
            }
            break;
        } case 0xC0: { // program (wave type) change
            if (midiMode) {
                midiPrograms[channel] = (WaveType)((packet.param1 - 1) & 7);
                if (midiPrograms[channel] == WaveType::None) {
                    midiPrograms[channel] = WaveType::Square;
                    midiDuty[channel] = 128;
                }
                for (int i = 0; i < 128; i++) {
                    if (midiChannels[channel][i] < NUM_CHANNELS) {
                        uint16_t c = midiChannels[channel][i];
                        channels[c].wavetype = midiPrograms[channel];
                        channels[c].fadeStart = 0;
                        command_queue[c][0][0] = COMMAND_WAVE_TYPE | typeconv[(int)midiPrograms[channel]];
                        if (midiPrograms[channel] == WaveType::Square) {
                            channels[c].duty = midiDuty[channel] / 255.0;
                            command_queue[c][0][1] = midiDuty[channel];
                        }
                        command_updates[0] = true;
                        changed = true;
                    }
                }
            } else {
                WaveType type = (WaveType)((packet.param1) & 7);
                if (type == WaveType::None) {
                    type = WaveType::Square;
                    channels[channel].duty = 128;
                }
                channels[channel].wavetype = type;
                channels[channel].fadeStart = 0;
                command_queue[channel][0][0] = COMMAND_WAVE_TYPE | typeconv[(int)type];
                if (type == WaveType::Square) command_queue[channel][0][1] = channels[channel].duty * 255;
                command_updates[0] = true;
                changed = true;
            }
            break;
        } case 0xD0: { // aftertouch (volume change per channel)
            if (midiMode) {
                for (int i = 0; i < 128; i++) {
                    if (midiChannels[channel][i] < NUM_CHANNELS) {
                        uint16_t c = midiChannels[channel][i];
                        channels[c].amplitude = packet.param1 / 127.5;
                        command_queue[c][2][0] = volume(packet.param1);
                        command_updates[2] = true;
                        changed = true;
                    }
                }
            } else {
                channels[channel].amplitude = packet.param1 / 127.5;
                command_queue[channel][2][0] = volume(packet.param1);
                command_updates[2] = true;
                changed = true;
            }
            break;
        } case 0xE0: { // pitch bend
            double offset = ((packet.param1 | ((int)packet.param2 << 7)) - 8192) / 4096.0;
            if (midiMode) {
                for (int i = 0; i < 128; i++) {
                    if (midiChannels[channel][i] < NUM_CHANNELS) {
                        uint16_t c = midiChannels[channel][i];
                        uint16_t freq = floor(channels[c].frequency * pow(2.0, offset / 12.0) * freqMultiplier + 0.5);
                        command_queue[c][1][0] = COMMAND_FREQUENCY | ((freq >> 8) & 0x3F);
                        command_queue[c][1][1] = freq & 0xFF;
                        command_updates[1] = true;
                        changed = true;
                    }
                }
            } else {
                uint16_t freq = floor(channels[channel].frequency * pow(2.0, offset / 12.0) * freqMultiplier + 0.5);
                command_queue[channel][1][0] = COMMAND_FREQUENCY | ((freq >> 8) & 0x3F);
                command_queue[channel][1][1] = freq & 0xFF;
                command_updates[1] = true;
                changed = true;
            }
            break;
        } case 0xF0: { // system commands
            switch (channel) {
            case 0x0F: { // reset
                gpio_put(PIN_DATA, true);
                sleep_us(1);
                gpio_put(PIN_CLOCK, true);
                sleep_us(1);
                gpio_put(PIN_CLOCK, false);
                sleep_us(1);
                gpio_put(PIN_DATA, false);
                sleep_us(1);
                for (int i = 0; i < NUM_CHANNELS; i++) {
                    gpio_put(PIN_STROBE, true);
                    sleep_us(1);
                    gpio_put(PIN_STROBE, false);
                    write_data(i, 0xFF);
                    gpio_put(PIN_CLOCK, true);
                    sleep_us(1);
                    gpio_put(PIN_CLOCK, false);
                    sleep_us(1);
                }
                gpio_put(PIN_STROBE, true);
                sleep_us(1);
                gpio_put(PIN_STROBE, false);
                // apparently this works to reset the chip?
                (*((volatile uint32_t*)(PPB_BASE + 0x0ED0C))) = 0x5FA0004;
                // we should be done by now, but just in case:
                watchdog_enable(1, 1);
                while (true);
            }
            }
            break;
        }
        }
        tud_midi_packet_write((const uint8_t*)&packet);
    }
    mutex_exit(&command_queue_lock);
}

#define TIMER_PERIOD 10000

void core2() {
    while (true) {
        int64_t time = time_us_64();
        mutex_enter_blocking(&command_queue_lock);
        for (int i = 0; i < NUM_CHANNELS; i++) {
            ChannelInfo * info = &channels[i];
            if (info->fadeStart > 0) {
                info->amplitude = info->fadeInit + (double)(time - info->fadeStart) / info->fadeLength * info->fadeDirection;
                if (time - info->fadeStart >= info->fadeLength) {
                    info->fadeInit = 0.0f;
                    info->fadeStart = info->fadeLength = 0;
                    info->amplitude = info->fadeDirection == 1 ? 1 : 0;
                    if (midiMode && midiUsedChannels[i] != 0xFF) {
                        midiChannels[midiUsedChannels[i]][info->note] = 0xFF;
                        midiUsedChannels[i] = 0xFF;
                    }
                }
                command_queue[i][2][0] = volume(info->amplitude * 127);
                command_updates[2] = true;
                changed = true;
            }
        }
        if (changed) {
            changed = false;
            gpio_put(PICO_DEFAULT_LED_PIN, false);
            for (int n = 0; n < 4; n++) {
                if (command_updates[n]) {
                    command_updates[n] = false;
                    gpio_put(PIN_DATA, true);
                    sleep_us_sr(1);
                    gpio_put(PIN_CLOCK, true);
                    sleep_us_sr(1);
                    gpio_put(PIN_CLOCK, false);
                    sleep_us_sr(1);
                    gpio_put(PIN_DATA, false);
                    sleep_us_sr(1);
                    for (int i = 0; i < NUM_CHANNELS; i++) {
                        if (command_queue[i][n][0] != 0xFF) {
                            gpio_put(PIN_STROBE, true);
                            sleep_us_sr(1);
                            gpio_put(PIN_STROBE, false);
                            sleep_us_sr(1);
                            write_data(i, command_queue[i][n][0]);
                            if (n == 1 || command_queue[i][n][0] == (COMMAND_WAVE_TYPE | 1)) write_data(i, command_queue[i][n][1]);
                            command_queue[i][n][0] = 0xFF;
                        }
                        gpio_put(PIN_CLOCK, true);
                        sleep_us_sr(1);
                        gpio_put(PIN_CLOCK, false);
                        sleep_us_sr(1);
                    }
                }
                gpio_put(PIN_STROBE, true);
                sleep_us_sr(1);
                gpio_put(PIN_STROBE, false);
                sleep_us_sr(1);
            }
            gpio_put(PICO_DEFAULT_LED_PIN, true);
        }
        mutex_exit(&command_queue_lock);
        int64_t period = time_us_64() - time;
        if (period < TIMER_PERIOD) sleep_us(TIMER_PERIOD - period);
    }
}

int main() {
    gpio_init(PICO_DEFAULT_LED_PIN); gpio_set_dir(PICO_DEFAULT_LED_PIN, true);
    gpio_in(0);
    gpio_in(1);
    gpio_in(2);
    gpio_in(3);
    gpio_in(4);
    gpio_in(5);
    gpio_out(6);
    gpio_out(7);
    gpio_out(8);
    gpio_out(9);
    gpio_out(10);
    gpio_out(11);
    gpio_out(12);
    gpio_out(13);
    gpio_out(14);
    gpio_out(PIN_STROBE);
    gpio_out(PIN_DATA);
    gpio_out(PIN_CLOCK);
    // Check board version number (0-1 = major revision, 2-5 = minor revision)
    // major mismatch = do not run, minor mismatch = disable features
    version_major = (gpio_get(0) ? 2 : 0) + (gpio_get(1) ? 1 : 0);
    if (version_major != BOARD_VERSION_MAJOR) {
        while (true) {
            gpio_put(PICO_DEFAULT_LED_PIN, true);
            sleep_ms(500);
            gpio_put(PICO_DEFAULT_LED_PIN, false);
            sleep_ms(500);
        }
    }
    version_minor = (gpio_get(2) ? 8 : 0) + (gpio_get(3) ? 4 : 0) + (gpio_get(4) ? 2 : 0) + (gpio_get(5) ? 1 : 0);
    gpio_put(PIN_DATA, false);
    sleep_us(1);
    for (int i = 0; i < 32; i++) {
        gpio_put(PIN_CLOCK, true);
        sleep_us(1);
        gpio_put(PIN_CLOCK, false);
        sleep_us(1);
    }
    gpio_put(PIN_STROBE, true);
    sleep_us(1);
    gpio_put(PIN_CLOCK, true);
    sleep_us(1);
    gpio_put(PIN_CLOCK, false);
    sleep_us(1);
    gpio_put(PIN_STROBE, false);
    sleep_us(1);
    mutex_init(&command_queue_lock);
    for (int i = 0; i < NUM_CHANNELS; i++) {
        command_queue[i][0][0] = 0xFF;
        command_queue[i][1][0] = 0xFF;
        command_queue[i][2][0] = 0xFF;
        command_queue[i][3][0] = 0xFF;
    }
    memset(midiChannels, 0xFF, 2048);
    memset(midiUsedChannels, 0xFF, NUM_CHANNELS);
    tusb_init();
    multicore_launch_core1(core2);
    gpio_put(PICO_DEFAULT_LED_PIN, true);
    while (true) tud_task(); // tinyusb device task
}
