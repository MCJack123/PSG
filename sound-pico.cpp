/*
 * sound-pico.cpp plugin for CraftOS-PC
 * Adds an interface between the original CraftOS-PC sound plugin and an auxiliary sound generation board over a serial connection.
 * THIS REQUIRES PROPER CONFIGURATION TO WORK! Assumes a Linux system with a USB serial port. Requires root to run.
 * Windows: cl /EHsc /Fesound-pico.dll /LD /Icraftos2\api /Icraftos2\craftos2-lua\include sound-pico.cpp /link craftos2\craftos2-lua\src\lua51.lib
 * Linux: g++ -fPIC -shared -Icraftos2/api -Icraftos2/craftos2-lua/include -o sound-pico.so sound-pico.cpp craftos2/craftos2-lua/src/liblua.a
 * Licensed under the MIT license.
 *
 * MIT License
 * 
 * Copyright (c) 2021-2022 JackMacWindows
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <CraftOS-PC.hpp>
#include <cmath>
#include <termios.h>
#define NUM_CHANNELS 32
#define channelGroup(id) ((id) | 0x74A800)
#define command(cmd, ch) (((cmd) << 5) | ((ch)-1))
#define reset "\xE0\xE0\xE0\xE0\xE0"

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
    static constexpr int identifier = 0x1d4c1cd0;
    int id;
    WaveType wavetype = WaveType::None;
    double duty = 0.5;
    unsigned int frequency = 0;
    float amplitude = 1.0;
    float pan = 0.0;
    double customWave[512];
    int customWaveSize;
    InterpolationMode interpolation;
};

static const PluginFunctions * func;
constexpr int ChannelInfo::identifier;
static FILE * output = NULL;
static std::mutex output_lock;

static void ChannelInfo_destructor(Computer * comp, int id, void* data) {
    ChannelInfo * channels = (ChannelInfo*)data;
    delete[] channels;
}

/*
 * Returns the type of wave assigned to the channel.
 * 1: The channel to check (1 - NUM_CHANNELS)
 * Returns: The current wave type
 */
static int sound_getWaveType(lua_State *L) {
    const int channel = luaL_checkinteger(L, 1);
    if (channel < 1 || channel > NUM_CHANNELS) luaL_error(L, "bad argument #1 (channel out of range)");
    ChannelInfo * info = (ChannelInfo*)get_comp(L)->userdata[ChannelInfo::identifier] + (channel - 1);
    switch (info->wavetype) {
        case WaveType::None: lua_pushstring(L, "none"); break;
        case WaveType::Sine: lua_pushstring(L, "sine"); break;
        case WaveType::Triangle: lua_pushstring(L, "triangle"); break;
        case WaveType::Sawtooth: lua_pushstring(L, "sawtooth"); break;
        case WaveType::RSawtooth: lua_pushstring(L, "rsawtooth"); break;
        case WaveType::Square: lua_pushstring(L, "square"); lua_pushnumber(L, info->duty); return 2;
        case WaveType::Noise: lua_pushstring(L, "noise"); break;
        case WaveType::Custom:
            lua_pushstring(L, "custom");
            lua_createtable(L, info->customWaveSize, 0);
            for (int i = 0; i < info->customWaveSize; i++) {
                lua_pushinteger(L, i+1);
                lua_pushnumber(L, info->customWave[i]);
                lua_settable(L, -3);
            }
            return 2;
        case WaveType::PitchedNoise: lua_pushstring(L, "pitched_noise"); break;
        default: lua_pushstring(L, "unknown"); break;
    }
    return 1;
}

/*
 * Sets the wave type for a channel.
 * 1: The channel to set (1 - NUM_CHANNELS)
 * 2: The type of wave as a string (from {"none", "sine", "triangle", "sawtooth", "square", and "noise"})
 */
static int sound_setWaveType(lua_State *L) {
    const int channel = luaL_checkinteger(L, 1);
    if (channel < 1 || channel > NUM_CHANNELS) luaL_error(L, "bad argument #1 (channel out of range)");
    ChannelInfo * info = (ChannelInfo*)get_comp(L)->userdata[ChannelInfo::identifier] + (channel - 1);
    std::string type = luaL_checkstring(L, 2);
    std::transform(type.begin(), type.end(), type.begin(), tolower);
    if (type == "none") info->wavetype = WaveType::None;
    else if (type == "sine") info->wavetype = WaveType::Sine;
    else if (type == "triangle") info->wavetype = WaveType::Triangle;
    else if (type == "sawtooth") info->wavetype = WaveType::Sawtooth;
    else if (type == "rsawtooth") info->wavetype = WaveType::RSawtooth;
    else if (type == "square") {
        info->wavetype = WaveType::Square;
        if (!lua_isnoneornil(L, 3)) {
            double duty = luaL_checknumber(L, 3);
            if (duty < 0.0 || duty > 1.0) luaL_error(L, "bad argument #3 (duty out of range)");
            info->duty = duty;
        } else info->duty = 0.5;
    } else if (type == "noise") info->wavetype = WaveType::Noise;
    else if (type == "custom") {
        luaL_checktype(L, 3, LUA_TTABLE);
        double points[512];
        lua_pushinteger(L, 1);
        lua_gettable(L, 3);
        if (lua_isnil(L, -1)) luaL_error(L, "bad argument #3 (no points in wavetable)");
        int i;
        for (i = 0; !lua_isnil(L, -1); i++) {
            if (i >= 512) luaL_error(L, "bad argument #3 (wavetable too large)");
            if (!lua_isnumber(L, -1)) luaL_error(L, "bad point %d in wavetable (expected number, got %s)", i+1, lua_typename(L, lua_type(L, -1)));
            points[i] = lua_tonumber(L, -1);
            if (points[i] < -1.0 || points[i] > 1.0) luaL_error(L, "bad point %d in wavetable (value out of range)", i+1);
            lua_pop(L, 1);
            lua_pushinteger(L, i+2);
            lua_gettable(L, 3);
        }
        info->wavetype = WaveType::Custom;
        memcpy(info->customWave, points, i * sizeof(double));
        info->customWaveSize = i;
    } else if (type == "pitched_noise" || type == "pitchedNoise" || type == "pnoise") info->wavetype = WaveType::PitchedNoise;
    else luaL_error(L, "bad argument #2 (invalid option '%s')", type.c_str());
    std::lock_guard<std::mutex> lock(output_lock);
    if (info->wavetype == WaveType::Square) {
        fputc(command(0, channel), output);
        fputc((int)info->wavetype, output);
        fputc(info->duty * 255, output);
    } else if (info->wavetype == WaveType::Custom) {
        /*fputc(command(0, channel), output);
        fputc((int)info->wavetype | (((info->customWaveSize - 1) >> 1) & 0x80), output);
        fputc((info->customWaveSize - 1) & 0xFF, output);*/
    } else {
        fputc(command(0, channel), output);
        fputc((int)info->wavetype, output);
    }
    fflush(output);
    return 0;
}

/*
 * Returns the frequency assigned to the channel.
 * 1: The channel to check (1 - NUM_CHANNELS)
 * Returns: The current frequency
 */
static int sound_getFrequency(lua_State *L) {
    const int channel = luaL_checkinteger(L, 1);
    if (channel < 1 || channel > NUM_CHANNELS) luaL_error(L, "bad argument #1 (channel out of range)");
    ChannelInfo * info = (ChannelInfo*)get_comp(L)->userdata[ChannelInfo::identifier] + (channel - 1);
    lua_pushinteger(L, info->frequency);
    return 1;
}

/*
 * Sets the frequency of the wave on a channel.
 * 1: The channel to set (1 - NUM_CHANNELS)
 * 2: The frequency in Hz
 */
static int sound_setFrequency(lua_State *L) {
    const int channel = luaL_checkinteger(L, 1);
    if (channel < 1 || channel > NUM_CHANNELS) luaL_error(L, "bad argument #1 (channel out of range)");
    ChannelInfo * info = (ChannelInfo*)get_comp(L)->userdata[ChannelInfo::identifier] + (channel - 1);
    lua_Integer frequency = luaL_checkinteger(L, 2);
    if (frequency < 0 || frequency > 65535) luaL_error(L, "bad argument #2 (frequency out of range)");
    info->frequency = frequency;
    uint16_t freq = frequency;
    std::lock_guard<std::mutex> lock(output_lock);
    fputc(command(1, channel), output);
    fwrite(&freq, 2, 1, output);
    fflush(output);
    return 0;
}

/*
 * Returns the volume of the channel.
 * 1: The channel to check (1 - NUM_CHANNELS)
 * Returns: The current volume
 */
static int sound_getVolume(lua_State *L) {
    const int channel = luaL_checkinteger(L, 1);
    if (channel < 1 || channel > NUM_CHANNELS) luaL_error(L, "bad argument #1 (channel out of range)");
    ChannelInfo * info = (ChannelInfo*)get_comp(L)->userdata[ChannelInfo::identifier] + (channel - 1);
    lua_pushnumber(L, info->amplitude);
    return 1;
}

/*
 * Sets the volume of a channel.
 * 1: The channel to set (1 - NUM_CHANNELS)
 * 2: The volume, from 0.0 to 1.0
 */
static int sound_setVolume(lua_State *L) {
    const int channel = luaL_checkinteger(L, 1);
    if (channel < 1 || channel > NUM_CHANNELS) luaL_error(L, "bad argument #1 (channel out of range)");
    ChannelInfo * info = (ChannelInfo*)get_comp(L)->userdata[ChannelInfo::identifier] + (channel - 1);
    float amplitude = luaL_checknumber(L, 2);
    if (amplitude < 0.0 || amplitude > 1.0) luaL_error(L, "bad argument #2 (volume out of range)");
    info->amplitude = amplitude;
    std::lock_guard<std::mutex> lock(output_lock);
    fputc(command(2, channel), output);
    fputc(amplitude * 255, output);
    fflush(output);
    return 0;
}

/*
 * Returns the panning of the channel.
 * 1: The channel to check (1 - NUM_CHANNELS)
 * Returns: The current panning
 */
static int sound_getPan(lua_State *L) {
    const int channel = luaL_checkinteger(L, 1);
    if (channel < 1 || channel > NUM_CHANNELS) luaL_error(L, "bad argument #1 (channel out of range)");
    ChannelInfo * info = (ChannelInfo*)get_comp(L)->userdata[ChannelInfo::identifier] + (channel - 1);
    lua_pushnumber(L, info->pan);
    return 1;
}

/*
 * Sets the panning for a channel.
 * 1: The channel to set (1 - NUM_CHANNELS)
 * 2: The panning, from -1.0 (right) to 1.0 (left)
 */
static int sound_setPan(lua_State *L) {
    const int channel = luaL_checkinteger(L, 1);
    if (channel < 1 || channel > NUM_CHANNELS) luaL_error(L, "bad argument #1 (channel out of range)");
    ChannelInfo * info = (ChannelInfo*)get_comp(L)->userdata[ChannelInfo::identifier] + (channel - 1);
    float pan = luaL_checknumber(L, 2);
    if (pan < -1.0 || pan > 1.0) luaL_error(L, "bad argument #2 (pan out of range)");
    info->pan = pan;
    std::lock_guard<std::mutex> lock(output_lock);
    /*fputc(command(3, channel), output);
    fputc(pan * (pan < 0 ? 128 : 127), output);
    fflush(output);*/
    return 0;
}

/*
 * Returns the interpolation for a channel's custom wave.
 * 1: The channel to check (1 - NUM_CHANNELS)
 * Returns: The current interpolation
 */
static int sound_getInterpolation(lua_State *L) {
    const int channel = luaL_checkinteger(L, 1);
    if (channel < 1 || channel > NUM_CHANNELS) luaL_error(L, "bad argument #1 (channel out of range)");
    ChannelInfo * info = (ChannelInfo*)get_comp(L)->userdata[ChannelInfo::identifier] + (channel - 1);
    switch (info->interpolation) {
        case InterpolationMode::None: lua_pushstring(L, "none");
        case InterpolationMode::Linear: lua_pushstring(L, "linear");
        default: lua_pushstring(L, "unknown");
    }
    return 1;
}

/*
 * Sets the interpolation mode for a channel's custom wave.
 * 1: The channel to set (1 - NUM_CHANNELS)
 * 2: The interpolation ("none", "linear"; 1, 2)
 */
static int sound_setInterpolation(lua_State *L) {
    const int channel = luaL_checkinteger(L, 1);
    if (channel < 1 || channel > NUM_CHANNELS) luaL_error(L, "bad argument #1 (channel out of range)");
    if (!lua_isnumber(L, 2) && !lua_isstring(L, 2)) luaL_error(L, "bad argument #2 (expected string or number, got %s)", lua_typename(L, lua_type(L, 2)));
    ChannelInfo * info = (ChannelInfo*)get_comp(L)->userdata[ChannelInfo::identifier] + (channel - 1);
    if (lua_isstring(L, 2)) {
        std::string str(lua_tostring(L, 2));
        if (str == "none") info->interpolation = InterpolationMode::None;
        else if (str == "linear") info->interpolation = InterpolationMode::Linear;
        else luaL_error(L, "bad argument #2 (invalid option %s)", str.c_str());
    } else {
        switch (lua_tointeger(L, 2)) {
            case 1: info->interpolation = InterpolationMode::None; break;
            case 2: info->interpolation = InterpolationMode::Linear; break;
            default: luaL_error(L, "bad argument #2 (invalid option %d)", lua_tointeger(L, 2));
        }
    }
    std::lock_guard<std::mutex> lock(output_lock);
    fputc(command(4, channel), output);
    fputc(info->interpolation == InterpolationMode::Linear ? 1 : 0, output);
    fflush(output);
    return 0;
}

/*
 * Starts or stops a fade out operation on a channel.
 * 1: The channel to fade out (1 - NUM_CHANNELS)
 * 2: The time for the fade out in seconds (0 to stop any fade out in progress)
 */
static int sound_fadeOut(lua_State *L) {
    const int channel = luaL_checkinteger(L, 1);
    if (channel < 1 || channel > NUM_CHANNELS) luaL_error(L, "bad argument #1 (channel out of range)");
    ChannelInfo * info = (ChannelInfo*)get_comp(L)->userdata[ChannelInfo::identifier] + (channel - 1);
    float time = luaL_checknumber(L, 2);
    std::lock_guard<std::mutex> lock(output_lock);
    fputc(command(5, channel), output);
    fwrite(&time, 4, 1, output);
    fflush(output);
    return 0;
}

static PluginInfo info("sound");
static luaL_Reg sound_lib[] = {
    {"getWaveType", sound_getWaveType},
    {"setWaveType", sound_setWaveType},
    {"getFrequency", sound_getFrequency},
    {"setFrequency", sound_setFrequency},
    {"getVolume", sound_getVolume},
    {"setVolume", sound_setVolume},
    {"getPan", sound_getPan},
    {"setPan", sound_setPan},
    {"getInterpolation", sound_getInterpolation},
    {"setInterpolation", sound_setInterpolation},
    {"fadeOut", sound_fadeOut},
    {NULL, NULL}
};

static Uint32 timer(Uint32 interval, void* param) {
    std::lock_guard<std::mutex> lock(output_lock);
    fwrite(reset, sizeof(reset)-1, 1, output);
    fflush(output);
    return interval;
}

extern "C" {
#ifdef _WIN32
_declspec(dllexport)
#endif
PluginInfo * plugin_init(const PluginFunctions * func, const path_t& path) {
    if (func->abi_version != PLUGIN_VERSION) return &info;
    ::func = func;
    output = fopen("/dev/ttyACM0", "wb");
    setvbuf(output, NULL, _IOFBF, 4096);
    struct termios tty;
    tcgetattr(fileno(output), &tty);
    cfsetspeed(&tty, B115200);
    cfmakeraw(&tty);
    tcsetattr(fileno(output), TCSANOW, &tty);
    SDL_AddTimer(50, timer, NULL);
    return &info;
}

#ifdef _WIN32
_declspec(dllexport)
#endif
int luaopen_sound(lua_State *L) {
    Computer * comp = get_comp(L);
    if (comp->userdata.find(ChannelInfo::identifier) == comp->userdata.end()) {
        ChannelInfo * channels = new ChannelInfo[NUM_CHANNELS];
        for (int i = 0; i < NUM_CHANNELS; i++) {
            channels[i].id = i;
        }
        comp->userdata[ChannelInfo::identifier] = channels;
        comp->userdata_destructors[ChannelInfo::identifier] = ChannelInfo_destructor;
    }
    luaL_register(L, "sound", sound_lib);
    lua_pushinteger(L, 2);
    lua_setfield(L, -2, "version");
    return 1;
}

#ifdef _WIN32
_declspec(dllexport)
#endif
void plugin_deinit(PluginInfo * info) {
    fclose(output);
}
}
