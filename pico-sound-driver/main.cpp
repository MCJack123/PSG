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

#include <hardware/flash.h>
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
#define BOARD_VERSION_MINOR 1

#define MAX_CHANNELS 16
#define NUM_CHANNELS (dualChannel ? MAX_CHANNELS / 2 : MAX_CHANNELS)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define gpio_out(pin) gpio_init(pin); gpio_set_dir(pin, true)
#define gpio_in(pin) gpio_init(pin); gpio_set_dir(pin, false)
#define COMMAND_WAVE_TYPE 0x00
#define COMMAND_VOLUME    0x40
#define COMMAND_FREQUENCY 0x80
#define COMMAND_PARAM     0xC0

#define PIN_STROBE 19
#define PIN_DATA   20
#define PIN_CLOCK  21

#define sleep_us_sr(n) sleep_us((n))
#define sleep_us_pic(n) sleep_us((n))

// !! CLOCK MULTIPLIER CONSTANT !!
// UPDATE THIS IF MODIFYING THE RUN LENGTH OF THE LOOP CODE
#define CLOCKS_PER_LOOP 252

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

struct Point {
    uint16_t x, y;
};

struct Envelope {
    Point points[12];
    uint8_t npoints = 0;
    uint8_t sustain = 0xFF;
    uint8_t loopStart = 0xFF;
    uint8_t loopEnd = 0xFF;
};

struct Instrument {
    Envelope volume;
    Envelope pan;
    Envelope frequency;
    Envelope duty;
    Envelope cutoff;
    Envelope resonance;
    uint8_t waveType;
    uint8_t linkedInst;
    int8_t detune;
};

struct ChannelInfo {
    // current status fields
    double position = 0.0;
    WaveType wavetype = WaveType::None;
    double duty = 0.5;
    unsigned int frequency = 0;
    double amplitude = 1.0;
    float pan = 0.0;
    uint32_t cutoff = 128*62.5;
    float resonance = 0.0;
    // fade fields
    double fadeInit = 0.0;
    int64_t fadeStart = 0;
    int64_t fadeLength = 0;
    int fadeDirection = -1;
    // sound 2.0 (probably irrelevant here)
    double customWave[512];
    int customWaveSize;
    InterpolationMode interpolation;
    bool isLowFreq = false;
    uint8_t note = 0;
    // instrument fields
    Instrument * inst = NULL;
    uint16_t ticks[6] = {0, 0, 0, 0, 0, 0};
    uint8_t points[6] = {0, 0, 0, 0, 0, 0};
    uint8_t typeIndex = 0;
    bool release = false;
    uint8_t linkedChannel = 0xFF;
};

struct MidiPacket {
    uint8_t usbcode;
    uint8_t command;
    uint8_t param1;
    uint8_t param2;
};

extern char usb_serial[];
ChannelInfo channels[MAX_CHANNELS];
uint8_t typeconv[9] = {0, 5, 4, 2, 3, 1, 6, 0, 6};
const double freqMultiplier = (65536.0 * CLOCKS_PER_LOOP) / 8000000.0;
uint8_t midiChannels[16][128];
int midiPrograms[16] = {0};
uint8_t midiDuty[16] = {128};
uint8_t midiCutoff[16] = {127};
uint8_t midiResonance[16] = {0};
uint8_t midiUsedChannels[MAX_CHANNELS] = {0xFF};
bool midiMode = true;
uint8_t command_queue[MAX_CHANNELS][6][2];
bool command_updates[6] = {false};
mutex_t command_queue_lock;
bool changed = false;
uint8_t freq_lsb[MAX_CHANNELS] = {0};
char hex_storage[0x4000];
uint16_t hex_storage_size = 0;
uint8_t inSysEx = 0;
uint16_t sysExSize;
uint8_t version_major, version_minor;
bool stereo = false, dualChannel = false;
Instrument patches[128];

/*
 * Base64 encoding/decoding (RFC1341)
 * Copyright (c) 2005-2011, Jouni Malinen <j@w1.fi>
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

static const uint8_t base64_table[65] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/**
 * base64_decode - Base64 decode
 * @src: Data to be decoded
 * @len: Length of the data to be decoded
 * @out: Pointer to output variable
 * @out_len: Length of output buffer
 * Returns: 0 on success, non-0 on error
 */
int base64_decode(const uint8_t *src, size_t len, uint8_t *out, size_t out_len) {
    static uint8_t dtable[256];
    static uint8_t dtable_done = 0;
	uint8_t *pos, block[4], tmp;
	size_t i, count, olen;
	int pad = 0;
    if (!dtable_done) {
        for (i = 0; i < 256; i++) dtable[i] = 0x80;
        for (i = 0; i < sizeof(base64_table) - 1; i++) dtable[base64_table[i]] = (uint8_t)i;
        dtable['='] = 0;
        dtable_done = 1;
    }
	count = 0;
	for (i = 0; i < len; i++) if (dtable[src[i]] != 0x80) count++;
	if (count == 0 || count % 4) return 1;
	olen = count / 4 * 3;
    if (out_len < olen) return 2;
	pos = out;
	count = 0;
	for (i = 0; i < len; i++) {
		tmp = dtable[src[i]];
		if (tmp == 0x80) continue;
		if (src[i] == '=') pad++;
		block[count] = tmp;
		count++;
		if (count == 4) {
			*pos++ = (block[0] << 2) | (block[1] >> 4);
			if (pad != 2) *pos++ = (block[1] << 4) | (block[2] >> 2);
			if (pad == 0) *pos++ = (block[2] << 6) | block[3];
			count = 0;
		}
	}
	return 0;
}

template<typename T> static T min(T a, T b) {return a < b ? a : b;}
template<typename T> static T max(T a, T b) {return a > b ? a : b;}

void writeWaveType(uint8_t c, WaveType type, uint8_t duty = 128) {
    command_queue[c][0][0] = COMMAND_WAVE_TYPE | typeconv[(int)type];
    if (type == WaveType::Square) command_queue[c][0][1] = duty;
    if (dualChannel) {
        command_queue[c+8][0][0] = COMMAND_WAVE_TYPE | typeconv[(int)type];
        if (type == WaveType::Square) command_queue[c+8][0][1] = duty;
    }
    command_updates[0] = true;
    changed = true;
}

void writeFrequency(uint8_t c, uint16_t freq) {
    freq = (uint16_t)floor(freq * freqMultiplier + 0.5);
    command_queue[c][1][0] = COMMAND_FREQUENCY | ((freq >> 8) & 0x3F);
    command_queue[c][1][1] = freq & 0xFF;
    if (dualChannel) {
        command_queue[c+8][1][0] = COMMAND_FREQUENCY | ((freq >> 8) & 0x3F);
        command_queue[c+8][1][1] = freq & 0xFF;
    }
    command_updates[1] = true;
    changed = true;
}

void writeVolume(uint8_t c, uint8_t vol) {
    if (dualChannel) {
        command_queue[c][2][0] = COMMAND_VOLUME | (uint8_t)floor(13.0 * log(vol * min(channels[c].pan + 1.0f, 1.0f) + 1) + 0.5);
        command_queue[c+8][2][0] = COMMAND_VOLUME | (uint8_t)floor(13.0 * log(vol * min(1.0f - channels[c].pan, 1.0f) + 1) + 0.5);
    } else {
        command_queue[c][2][0] = COMMAND_VOLUME | (uint8_t)floor(13.0 * log(vol + 1) + 0.5);
    }
    command_updates[2] = true;
    changed = true;
}

void writeCutoff(uint8_t c, uint8_t cutoff) {
    if (cutoff > 127) cutoff = 127;
    channels[c].cutoff = cutoff * 62.5;
    uint8_t alpha = cutoff == 127 ? 255 : (uint8_t)floor((1 - pow(M_E, -((cutoff * 62.5) / (32000000.0 / CLOCKS_PER_LOOP))*2*M_PI)) * 255.0);
    double beta_d = cutoff == 127 ? 0 : (2.0 * channels[c].resonance * cos(2.0 * M_PI * (channels[c].cutoff / (32000000.0 / CLOCKS_PER_LOOP))));
    uint8_t beta = ((uint8_t)floor(abs(beta_d) * 63.0) & 0x3F) << 2 | (beta_d < 0 ? 2 : 0) | (abs(beta_d) >= 1 ? 1 : 0);
    command_queue[c][3][0] = COMMAND_PARAM | 7;
    command_queue[c][3][1] = alpha;
    command_queue[c][4][0] = COMMAND_PARAM | 10;
    command_queue[c][4][1] = beta;
    if (dualChannel) {
        command_queue[c+8][3][0] = COMMAND_PARAM | 7;
        command_queue[c+8][3][1] = alpha;
        command_queue[c+8][4][0] = COMMAND_PARAM | 10;
        command_queue[c+8][4][1] = beta;
    }
    command_updates[3] = true;
    command_updates[4] = true;
    changed = true;
}

void writeResonance(uint8_t c, uint8_t res) {
    channels[c].resonance = res / 128.0;
    double beta_d = channels[c].cutoff >= (uint32_t)127*62.5 ? 0 : (2.0 * channels[c].resonance * cos(2.0 * M_PI * (channels[c].cutoff / (32000000.0 / CLOCKS_PER_LOOP))));
    uint8_t beta = ((uint8_t)floor(abs(beta_d) * 63.0) & 0x3F) << 2 | (beta_d < 0 ? 2 : 0) | (abs(beta_d) >= 1 ? 1 : 0);
    uint8_t gamma = channels[c].cutoff >= (uint32_t)127*62.5 ? 0 : (uint8_t)floor(channels[c].resonance * channels[c].resonance * 255.0);
    command_queue[c][4][0] = COMMAND_PARAM | 10;
    command_queue[c][4][1] = beta;
    command_queue[c][5][0] = COMMAND_PARAM | 11;
    command_queue[c][5][1] = gamma;
    if (dualChannel) {
        command_queue[c+8][4][0] = COMMAND_PARAM | 10;
        command_queue[c+8][4][1] = beta;
        command_queue[c+8][5][0] = COMMAND_PARAM | 11;
        command_queue[c+8][5][1] = gamma;
    }
    command_updates[4] = true;
    command_updates[5] = true;
    changed = true;
}

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
                        if (addr < 0x200) continue; // don't overwrite bootloader
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
                if (inSysEx == 1 || inSysEx == 3) {
                    memset(hex_storage, 0, 0x4000);
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
            } else if (inSysEx == 3) {
                // load instrument envelope
                uint8_t s = packet.usbcode & 0x03;
                if (s != 1) hex_storage[hex_storage_size++] = packet.command;
                if (s == 0 || s == 3) hex_storage[hex_storage_size++] = packet.param1;
                if (s == 0) hex_storage[hex_storage_size++] = packet.param2;
                if (s != 0) {
                    inSysEx = 0;
                    base64_decode((const uint8_t*)hex_storage + 1, hex_storage_size - 1, (uint8_t*)&patches[hex_storage[0]], sizeof(Instrument) + 3);
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
                        break;
                    }
                    int program = midiPrograms[channel];
                    ChannelInfo * parent = NULL;
                linkedInst_resume:
                    for (int c = 0; c < NUM_CHANNELS; c++) {
                        if (midiUsedChannels[c] == 0xFF && channels[c].inst == NULL) {
                            midiUsedChannels[c] = channel;
                            if (parent) parent->linkedChannel = c;
                            else midiChannels[channel][packet.param1] = c;
                            uint16_t freq = (uint16_t)floor(pow(2.0, ((double)packet.param1 - 69.0 + patches[program].detune) / 12.0) * 440.0 + 0.5);
                            channels[c].amplitude = packet.param2 / 127.5;
                            channels[c].frequency = freq;
                            channels[c].wavetype = (WaveType)patches[program].waveType;
                            channels[c].note = packet.param1;
                            channels[c].cutoff = midiCutoff[channel];
                            channels[c].resonance = midiResonance[channel];
                            channels[c].fadeStart = 0;
                            channels[c].inst = &patches[program];
                            channels[c].points[0] = channels[c].points[1] = channels[c].points[2] = channels[c].points[3] = channels[c].points[4] = channels[c].points[5] = 0;
                            channels[c].ticks[0] = channels[c].ticks[1] = channels[c].ticks[2] = channels[c].ticks[3] = channels[c].ticks[4] = channels[c].ticks[5] = 0;
                            channels[c].release = false;
                            channels[c].linkedChannel = 0xFF;
                            if (channels[c].wavetype == WaveType::Square) channels[c].duty = midiDuty[channel] / 255.0;
                            writeWaveType(c, channels[c].wavetype, midiDuty[channel]);
                            if (channels[c].inst->linkedInst) {
                                program = channels[c].inst->linkedInst;
                                parent = &channels[c];
                                goto linkedInst_resume;
                            }
                            break;
                        }
                    }
                } else {
                    uint16_t freq = (uint16_t)floor(pow(2.0, (packet.param1 - 69.0) / 12.0) * 440 + 0.5);
                    channels[channel].amplitude = packet.param2 / 127.5;
                    channels[channel].frequency = freq;
                    channels[channel].fadeStart = 0; 
                    writeFrequency(channel, freq);
                    writeVolume(channel, packet.param2);
                }
                break;
            } // fall through
        } case 0x80: { // note off
            if (packet.param2 == 0 || packet.param2 == 127) {
                if (midiMode) {
                    if (midiChannels[channel][packet.param1] < NUM_CHANNELS) {
                        uint8_t c = midiChannels[channel][packet.param1];
                        do {
                            if (channels[c].inst == NULL) {
                                channels[c].amplitude = 0;
                                channels[c].fadeStart = 0;
                                writeVolume(c, 0);
                            } else {
                                channels[c].release = true;
                            }
                            midiUsedChannels[c] = 0xFF;
                        } while ((c = channels[c].linkedChannel) != 0xFF);
                    }
                    midiChannels[channel][packet.param1] = 0xFF;
                } else {
                    channels[channel].amplitude = 0;
                    channels[channel].fadeStart = 0;
                    writeVolume(channel, 0);
                }
            } else {
                if (midiMode) {
                    if (midiChannels[channel][packet.param1] < NUM_CHANNELS) {
                        uint8_t c = midiChannels[channel][packet.param1];
                        do {
                            channels[c].fadeInit = channels[c].amplitude;
                            channels[c].fadeStart = time_us_64();
                            channels[c].fadeDirection = -1;
                            channels[c].fadeLength = (127 - packet.param2) * (1000000/64);
                            channels[c].inst = NULL;
                        } while ((c = channels[c].linkedChannel) != 0xFF);
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
                    do {
                        channels[c].amplitude = packet.param2 / 127.5;
                        if (channels[c].inst == NULL || channels[c].inst->volume.npoints == 0) writeVolume(c, packet.param2);
                    } while ((c = channels[c].linkedChannel) != 0xFF);
                }
            } else {
                // just change whole channel volume
                channels[channel].amplitude = packet.param2 / 127.5;
                writeVolume(channel, packet.param2);
            }
            break;
        } case 0xB0: { // control change
            switch (packet.param1) {
            case 1: { // square duty
                if (midiMode) {
                    midiDuty[channel] = packet.param2 * 2;
                    if ((WaveType)patches[midiPrograms[channel]].waveType == WaveType::Square) {
                        for (int i = 0; i < 128; i++) {
                            if (midiChannels[channel][i] < NUM_CHANNELS) {
                                uint16_t c = midiChannels[channel][i];
                                do {
                                    channels[c].duty = packet.param2 / 127.5;
                                    if (channels[c].inst == NULL || channels[c].inst->duty.npoints == 0) writeWaveType(c, WaveType::Square, midiDuty[channel]);
                                } while ((c = channels[c].linkedChannel) != 0xFF);
                            }
                        }
                    }
                } else {
                    channels[channel].duty = packet.param2 / 127.5;
                    if (channels[channel].wavetype == WaveType::Square) writeWaveType(channel, WaveType::Square, packet.param2 * 2);
                }
                break;
            } case 7: { // volume
                if (midiMode) {
                    for (int i = 0; i < 128; i++) {
                        if (midiChannels[channel][i] < NUM_CHANNELS) {
                            uint16_t c = midiChannels[channel][i];
                            do {
                                channels[c].amplitude = packet.param2 / 127.5;
                                channels[c].fadeStart = 0;
                                if (channels[c].inst == NULL || channels[c].inst->volume.npoints == 0) {
                                    writeVolume(c, packet.param2);
                                }
                            } while ((c = channels[c].linkedChannel) != 0xFF);
                        }
                    }
                } else {
                    channels[channel].amplitude = packet.param2 / 127.5;
                    channels[channel].fadeStart = 0;
                    writeVolume(channel, packet.param2);
                }
                break;
            } case 10: { // pan
                if (midiMode) {
                    for (int i = 0; i < 128; i++) {
                        if (midiChannels[channel][i] < NUM_CHANNELS) {
                            uint16_t c = midiChannels[channel][i];
                            do {
                                channels[c].pan = (packet.param2 - 64.0) / (packet.param2 > 64 ? 63.0 : 64.0);
                                if (channels[c].inst == NULL || channels[c].inst->volume.npoints == 0) {
                                    writeVolume(c, channels[c].amplitude * 127.5);
                                }
                            } while ((c = channels[c].linkedChannel) != 0xFF);
                        }
                    }
                } else {
                    channels[channel].pan = (packet.param2 - 64.0) / (packet.param2 > 64 ? 63.0 : 64.0);
                    writeVolume(channel, channels[channel].amplitude * 127.5);
                }
                break;
            } case 24: { // frequency (MSB)
                uint16_t freq = freq_lsb[channel] | ((uint16_t)packet.param2 << 7);
                channels[channel].frequency = freq;
                channels[channel].inst = NULL;
                writeFrequency(channel, freq);
                break;
            } case 56: { // frequency (LSB)
                freq_lsb[channel] = packet.param2;
                uint16_t freq = freq_lsb[channel] | (channels[channel].frequency & 0xFF00);
                channels[channel].frequency = freq;
                channels[channel].inst = NULL;
                writeFrequency(channel, freq);
                break;
            } case 71: { // resonance
                if (midiMode) {
                    midiResonance[channel] = packet.param2;
                    for (int i = 0; i < NUM_CHANNELS; i++) {
                        writeResonance(i, packet.param2);
                    }
                } else {
                    writeResonance(channel, packet.param2);
                }
                break;
            } case 74: { // LPF cutoff
                if (midiMode) {
                    midiCutoff[channel] = packet.param2;
                    for (int i = 0; i < NUM_CHANNELS; i++) {
                        writeCutoff(i, packet.param2);
                    }
                } else {
                    writeCutoff(channel, packet.param2);
                }
                break;
            } case 86: { // stereo mode
                stereo = packet.param2 & 0x40;
                dualChannel = packet.param2 & 0x20;
                if (version_minor >= 1) gpio_put(18, stereo);
                break;
            } case 123: { // all notes off
                //if (!(packet.param2 & 0x40)) break;
                if (midiMode) {
                    for (int i = 0; i < 128; i++) {
                        if (midiChannels[channel][i] < NUM_CHANNELS) {
                            uint8_t c = midiChannels[channel][i];
                            do {
                                channels[c].amplitude = 0;
                                channels[c].inst = NULL;
                                writeVolume(c, 0);
                                midiUsedChannels[c] = 0xFF;
                            } while ((c = channels[c].linkedChannel) != 0xFF);
                        }
                        midiChannels[channel][i] = 0xFF;
                    }
                    for (int i = 0; i < 16; i++) midiUsedChannels[i] = 0xFF;
                } else {
                    channels[channel].amplitude = 0;
                    writeVolume(channel, 0);
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
                midiPrograms[channel] = packet.param1;
                for (int i = 0; i < 128; i++) {
                    if (midiChannels[channel][i] < NUM_CHANNELS) {
                        uint16_t c = midiChannels[channel][i];
                        uint8_t program = midiPrograms[channel];
                        do {
                            channels[c].wavetype = (WaveType)patches[program].waveType;
                            channels[c].fadeStart = 0;
                            channels[c].inst = &patches[program];
                            channels[c].points[0] = channels[c].points[1] = channels[c].points[2] = channels[c].points[3] = 0;
                            channels[c].ticks[0] = channels[c].ticks[1] = channels[c].ticks[2] = channels[c].ticks[3] = 0;
                            channels[c].release = false;
                            if (channels[c].wavetype == WaveType::Square) channels[c].duty = midiDuty[channel] / 255.0;
                            writeWaveType(c, channels[c].wavetype, midiDuty[channel]);
                            if (patches[program].linkedInst) program = patches[program].linkedInst;
                        } while ((c = channels[c].linkedChannel) != 0xFF);
                    }
                }
            } else {
                WaveType type = (WaveType)((packet.param1) & 7);
                if (type == WaveType::None) {
                    type = WaveType::Square;
                    channels[channel].duty = 0.5;
                }
                channels[channel].wavetype = type;
                channels[channel].fadeStart = 0;
                writeWaveType(channel, type, channels[channel].duty * 255);
            }
            break;
        } case 0xD0: { // aftertouch (volume change per channel)
            if (midiMode) {
                for (int i = 0; i < 128; i++) {
                    if (midiChannels[channel][i] < NUM_CHANNELS) {
                        uint16_t c = midiChannels[channel][i];
                        do {
                            channels[c].amplitude = packet.param1 / 127.5;
                            if (channels[c].inst == NULL || channels[c].inst->volume.npoints == 0) writeVolume(c, packet.param1);
                        } while ((c = channels[c].linkedChannel) != 0xFF);
                    }
                }
            } else {
                channels[channel].amplitude = packet.param1 / 127.5;
                writeVolume(channel, packet.param1);
            }
            break;
        } case 0xE0: { // pitch bend
            double offset = ((packet.param1 | ((int)packet.param2 << 7)) - 8192) / 4096.0;
            if (midiMode) {
                for (int i = 0; i < 128; i++) {
                    if (midiChannels[channel][i] < NUM_CHANNELS) {
                        uint16_t c = midiChannels[channel][i];
                        do {
                            writeFrequency(c, channels[c].frequency * pow(2.0, offset / 12.0));
                        } while ((c = channels[c].linkedChannel) != 0xFF);
                    }
                }
            } else {
                writeFrequency(channel, channels[channel].frequency * pow(2.0, offset / 12.0));
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
                for (int i = 0; i < MAX_CHANNELS; i++) {
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
        //tud_midi_packet_write((const uint8_t*)&packet);
    }
    mutex_exit(&command_queue_lock);
}

#define TIMER_PERIOD 10000

float processEnvelope(ChannelInfo * info, const Envelope * env, uint16_t * tick, uint8_t * point, bool release) {
    if ((*point == env->sustain && !release) || *point + 1 >= env->npoints) return env->points[*point].y;
    else if (++(*tick) >= env->points[*point+1].x) {
        if (++(*point) == env->loopEnd && env->loopStart < 12 && !release) {
            *point = env->loopStart;
            *tick = env->points[*point].x;
        }
        return env->points[*point].y;
    } else {
        const Point a = env->points[*point], b = env->points[*point+1];
        return a.y + (b.y - a.y) * ((float)(*tick - a.x) / (float)(b.x - a.x));
    }
}

void core2() {
    while (true) {
        int64_t time = time_us_64();
        mutex_enter_blocking(&command_queue_lock);
        for (int i = 0; i < NUM_CHANNELS; i++) {
            ChannelInfo * info = &channels[i];
            if (info->inst != NULL) {
                if (info->inst->pan.npoints > 0 && stereo && dualChannel) {
                    int8_t val = (int8_t)processEnvelope(info, &info->inst->pan, &info->ticks[1], &info->points[1], info->release);
                    info->pan = (val - 64.0) / (val > 64 and 63.0 or 64.0);
                }
                if (info->inst->volume.npoints > 0) {
                    writeVolume(i, info->amplitude * processEnvelope(info, &info->inst->volume, &info->ticks[0], &info->points[0], info->release));
                } else if (info->ticks[0] == 0) {
                    writeVolume(i, info->amplitude * 127);
                    info->ticks[0]++;
                }
                if (info->inst->frequency.npoints > 0) {
                    writeFrequency(i, info->frequency * pow(2.0, (processEnvelope(info, &info->inst->frequency, &info->ticks[2], &info->points[2], info->release) - 0x8000) / 192.0));
                } else if (info->ticks[2] == 0) {
                    writeFrequency(i, info->frequency);
                    info->ticks[2]++;
                }
                if (info->inst->cutoff.npoints > 0) {
                    writeCutoff(i, processEnvelope(info, &info->inst->cutoff, &info->ticks[4], &info->points[4], info->release));
                } else if (info->ticks[4] == 0) {
                    writeCutoff(i, info->cutoff);
                    info->ticks[4]++;
                }
                if (info->inst->resonance.npoints > 0) {
                    writeResonance(i, processEnvelope(info, &info->inst->resonance, &info->ticks[5], &info->points[5], info->release));
                } else if (info->ticks[5] == 0) {
                    writeResonance(i, info->resonance);
                    info->ticks[5]++;
                }
                if (info->inst->duty.npoints > 0 && info->wavetype == WaveType::Square) {
                    writeWaveType(i, WaveType::Square, (uint8_t)processEnvelope(info, &info->inst->duty, &info->ticks[3], &info->points[3], info->release) * 2);
                }
                if ((info->inst->volume.npoints > 0 && info->points[0] + 1 >= info->inst->volume.npoints && info->points[1] + 1 >= info->inst->pan.npoints && info->points[2] + 1 >= info->inst->frequency.npoints && (info->wavetype != WaveType::Square || info->points[3] + 1 >= info->inst->duty.npoints)) || (info->inst->volume.npoints == 0 && info->release)) {
                    info->inst = NULL;
                    writeWaveType(i, WaveType::None);
                }
            } else if (info->fadeStart > 0) {
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
                writeVolume(i, info->amplitude * 127);
            }
        }
        if (changed) {
            changed = false;
            gpio_put(PICO_DEFAULT_LED_PIN, false);
            for (int n = 0; n < 6; n++) {
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
                    for (int i = 0; i < MAX_CHANNELS; i++) {
                        if (command_queue[i][n][0] != 0xFF) {
                            gpio_put(PIN_STROBE, true);
                            sleep_us_sr(1);
                            gpio_put(PIN_STROBE, false);
                            sleep_us_sr(1);
                            write_data(i, command_queue[i][n][0]);
                            if (!(n == 0 || n == 2) || command_queue[i][n][0] == (COMMAND_WAVE_TYPE | 1)) write_data(i, command_queue[i][n][1]);
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
    for (int i = 0; i < MAX_CHANNELS; i++) {
        command_queue[i][0][0] = 0xFF;
        command_queue[i][1][0] = 0xFF;
        command_queue[i][2][0] = 0xFF;
        command_queue[i][3][0] = 0xFF;
        command_queue[i][4][0] = 0xFF;
        command_queue[i][5][0] = 0xFF;
    }
    memset(midiChannels, 0xFF, 2048);
    memset(midiUsedChannels, 0xFF, MAX_CHANNELS);
    for (uint8_t i = 0; i < 128; i++) {
        patches[i] = {
            { // volume
                {{0, 127}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
                0, 0xFF, 0xFF, 0xFF
            },
            { // pan
                {{0, 64}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
                0, 0xFF, 0xFF, 0xFF
            },
            { // frequency
                {{0, 0x8000}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
                0, 0xFF, 0xFF, 0xFF
            },
            { // duty
                {{0, i == 0 ? (uint16_t)64 : (uint16_t)i}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
                (i == 0 || i % 8 == 5) ? (uint8_t)0 : (uint8_t)1, 0, 0xFF, 0xFF
            },
            { // cutoff
                {{0, 0x7F}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
                0, 0xFF, 0xFF, 0xFF
            },
            { // resonance
                {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
                0, 0xFF, 0xFF, 0xFF
            },
            i % 8 == 0 ? (uint8_t)5 : (uint8_t)(i % 8), // waveType
            0, // linkedInst
            0 // detune
        };
    }
    uint64_t serial = 0;
    flash_get_unique_id((uint8_t*)&serial);
    sprintf(usb_serial, "%016lx", serial);
    usb_serial[16] = ':';
    tusb_init();
    multicore_launch_core1(core2);
    gpio_put(PICO_DEFAULT_LED_PIN, true);
    while (true) tud_task(); // tinyusb device task
}
