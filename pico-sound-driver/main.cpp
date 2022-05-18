#include <hardware/gpio.h>
#include <hardware/pwm.h>
#include <hardware/timer.h>
#include <pico/critical_section.h>
#include <pico/multicore.h>
#include <pico/stdio_usb.h>
#include <math.h>
#include <stdio.h>

#define NUM_CHANNELS 16
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define gpio_out(pin) gpio_init(pin); gpio_set_dir(pin, true)
#define COMMAND_WAVE_TYPE 0x00
#define COMMAND_VOLUME    0x40
#define COMMAND_FREQUENCY 0x80
#define COMMAND_RESET     0xC0

#define PIN_STROBE 19
#define PIN_DATA   20
#define PIN_CLOCK  21

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
    int id;
    double position = 0.0;
    WaveType wavetype = WaveType::None;
    double duty = 0.5;
    unsigned int frequency = 0;
    float amplitude = 1.0;
    float newAmplitude = -1;
    float pan = 0.0;
    float fade = 0;
    float fadeMax = 0;
    float fadeInit = 0.0;
    int channelCount = 4;
    int fadeDirection = -1;
    double customWave[512];
    int customWaveSize;
    InterpolationMode interpolation;
    double lastUpdateTime;
};

ChannelInfo channels[NUM_CHANNELS];
uint8_t typeconv[9] = {0, 5, 4, 2, 3, 1, 6, 0, 6};

template<typename T> static T min(T a, T b) {return a < b ? a : b;}
template<typename T> static T max(T a, T b) {return a > b ? a : b;}

// Command format:
// 3 bits - command
// 5 bits - channel number
// commands:
// - 0: set wave type
//      arguments:
//      1 bit - high bit for wavetable size
//      3 bits - reserved
//      4 bits - wave type
//      - same numbers as WaveType enum
//      if using custom wavetable:
//      8 bits - low byte for wavetable size
//      <n> bytes - wavetable
//      if using square wave:
//      8 bits - duty cycle
// - 1: set frequency
//      arguments:
//      16 bits - frequency
// - 2: set volume
//      arguments:
//      8 bits - volume
// - 3: set pan
//      arguments:
//      8 bits - signed pan
// - 4: set interpolation
//      arguments:
//      7 bits - reserved
//      1 bit - 0 for none, 1 for linear
// - 5: fade in/out
//      arguments:
//      32 bits - floating-point time to fade for

int currentChannel = 0;

void interrupt(int channel) {
    gpio_put(PICO_DEFAULT_LED_PIN, false);
    gpio_put(PIN_DATA, true);
    sleep_us(5);
    gpio_put(PIN_CLOCK, true);
    sleep_us(5);
    gpio_put(PIN_CLOCK, false);
    sleep_us(5);
    gpio_put(PIN_DATA, false);
    sleep_us(5);
    for (int i = 0; i < channel; i++) {
        gpio_put(PIN_CLOCK, true);
        sleep_us(5);
        gpio_put(PIN_CLOCK, false);
        sleep_us(5);
    }
    gpio_put(PIN_STROBE, true);
    sleep_us(5);
    gpio_put(PIN_STROBE, false);
    sleep_us(5);
    for (int i = channel; i < 32; i++) {
        gpio_put(PIN_CLOCK, true);
        sleep_us(5);
        gpio_put(PIN_CLOCK, false);
        sleep_us(5);
    }
    gpio_put(PIN_STROBE, true);
    sleep_us(5);
    gpio_put(PIN_STROBE, false);
    sleep_us(5);
    gpio_put(PICO_DEFAULT_LED_PIN, true);
}

void write_data(uint8_t data) {
    gpio_put(6, data & 0x80);
    gpio_put(7, data & 0x40);
    gpio_put(8, data & 0x20);
    gpio_put(9, data & 0x10);
    gpio_put(10, data & 0x08);
    gpio_put(11, data & 0x04);
    gpio_put(12, data & 0x02);
    gpio_put(13, data & 0x01);
    sleep_us(2);
    gpio_put(14, true);
    sleep_us(1);
    gpio_put(14, false);
    sleep_us(10);
}

int main() {
    gpio_init(PICO_DEFAULT_LED_PIN); gpio_set_dir(PICO_DEFAULT_LED_PIN, true);
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
    gpio_put(PIN_DATA, false);
    sleep_us(5);
    for (int i = 0; i < 32; i++) {
        gpio_put(PIN_CLOCK, true);
        sleep_us(5);
        gpio_put(PIN_CLOCK, false);
        sleep_us(5);
    }
    gpio_put(PIN_STROBE, true);
    sleep_us(5);
    gpio_put(PIN_CLOCK, true);
    sleep_us(5);
    gpio_put(PIN_CLOCK, false);
    sleep_us(5);
    gpio_put(PIN_STROBE, false);
    sleep_us(5);
    for (int i = 0; i < NUM_CHANNELS; i++) {
        channels[i].id = i;
        channels[i].lastUpdateTime = time_us_64() / 1000000.0;
    }
    stdio_init_all();
    gpio_put(PICO_DEFAULT_LED_PIN, true);
    // TODO: batch channels
    while (true) {
        int cmd = getchar();
        int channel = cmd & 0x1F;
        if (cmd == 0x5C) {
            for (int i = 0; i < NUM_CHANNELS; i++) {
                printf("Channel: %d\nWave type: %d\nFrequency: %d\nVolume: %f\nPan: %f\nPosition: %f\n\n", i, channels[i].wavetype, channels[i].frequency, channels[i].amplitude, channels[i].pan, channels[i].position);
            }
            continue;
        }
        switch (cmd & 0xE0) {
            case 0x00: { // wave type
                int typef = getchar();
                int type = typef & 0x0F;
                if (type == 7) {
                    int size = (getchar() | ((int)(typef & 0x80) << 1)) + 1;
                    uint8_t samples[512];
                    fread(samples, 1, size, stdin);
                    if (channel >= NUM_CHANNELS) break;
                    channels[channel].wavetype = WaveType::Custom;
                    for (int i = 0; i < size; i++) channels[channel].customWave[i] = samples[i] / 255.0;
                    channels[channel].customWaveSize = size;
                    channels[channel].position = 0;
                } else if (type == 5) {
                    int duty = getchar();
                    if (channel >= NUM_CHANNELS || (channels[channel].wavetype == WaveType::Square && channels[channel].duty == duty / 255.0)) break;
                    channels[channel].wavetype = WaveType::Square;
                    channels[channel].duty = duty / 255.0;
                } else if (type == 8) {
                    if (channel >= NUM_CHANNELS || channels[channel].wavetype == WaveType::PitchedNoise) break;
                    channels[channel].wavetype = WaveType::PitchedNoise;
                    for (int i = 0; i < 512; i++) channels[channel].customWave[i] = (float)rand() / (float)RAND_MAX;
                    channels[channel].customWaveSize = 512;
                    channels[channel].position = 0;
                } else {
                    if (channel >= NUM_CHANNELS || channels[channel].wavetype == (WaveType)type) break;
                    channels[channel].wavetype = (WaveType)type;
                }
                interrupt(channel);
                write_data(COMMAND_WAVE_TYPE | typeconv[type]);
                if (type == 5) write_data(channels[channel].duty * 255.0);
                break;
            } case 0x20: { // frequency
                uint16_t freq = 0;
                fread(&freq, 2, 1, stdin);
                if (channel >= NUM_CHANNELS || channels[channel].frequency == freq) break;
                channels[channel].frequency = freq;
                interrupt(channel);
                write_data(COMMAND_FREQUENCY | ((freq >> 8) & 0x3F));
                write_data(freq & 0xFF);
                break;
            } case 0x40: { // volume
                uint8_t vol = getchar();
                if (channel >= NUM_CHANNELS || channels[channel].newAmplitude == min(max(vol / 255.0, 0.0), 1.0)) break;
                channels[channel].newAmplitude = min(max(vol / 255.0, 0.0), 1.0);
                interrupt(channel);
                write_data(COMMAND_VOLUME);
                write_data(vol);
                break;
            } case 0x60: { // pan
                int8_t pan = getchar();
                if (channel >= NUM_CHANNELS) break;
                //channels[channel].pan = pan / (pan < 0 ? 128.0 : 127.0);
                break;
            } case 0x80: { // interpolation
                bool interpolate = getchar();
                if (channel >= NUM_CHANNELS) break;
                channels[channel].interpolation = interpolate ? InterpolationMode::Linear : InterpolationMode::None;
                break;
            } case 0xA0: { // fade
                float time = 0;
                fread(&time, 4, 1, stdin);
                if (channel >= NUM_CHANNELS) break;
                if (time < -0.000001) {
                    channels[channel].fadeInit = 1 - channels[channel].amplitude;
                    channels[channel].fadeDirection = 1;
                    channels[channel].fade = channels[channel].fadeMax = -time;
                    interrupt(channel);
                    write_data(COMMAND_VOLUME);
                    write_data(1);
                } else if (time < 0.000001) {
                    channels[channel].fadeInit = 0.0;
                    channels[channel].fade = channels[channel].fadeMax = 0;
                } else {
                    channels[channel].fadeInit = channels[channel].amplitude;
                    channels[channel].fadeDirection = -1;
                    channels[channel].fade = channels[channel].fadeMax = time;
                    interrupt(channel);
                    write_data(COMMAND_VOLUME);
                    write_data(0);
                }
                break;
            } case 0xC0: { // set channel (debugging)
                currentChannel = channel;
                break;
            }
        }
    }
}
