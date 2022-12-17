/*
 * sound-midi.cpp
 * PSG
 * 
 * Adds an interface between the original CraftOS-PC sound plugin and an
 * auxiliary sound generation board over a MIDI connection.
 * 
 * Windows: cl /EHsc /Fesound-midi.dll /LD /Icraftos2\api /Icraftos2\craftos2-lua\include sound-midi.cpp /link craftos2\craftos2-lua\src\lua51.lib
 * Linux: g++ -fPIC -shared -Icraftos2/api -Icraftos2/craftos2-lua/include -o sound-midi.so sound-midi.cpp craftos2/craftos2-lua/src/liblua.a
 *
 * This code is licensed under the GPLv2 license.
 * Copyright (c) 2022-2023 JackMacWindows.
 */

#include <CraftOS-PC.hpp>
#include <portmidi.h>
#include <thread>
#include <cmath>
#include <termios.h>

#define NUM_CHANNELS 16
#define channelGroup(id) ((id) | 0x74A800)

#define MESSAGE_NOTE_OFF        0x80
#define MESSAGE_NOTE_ON         0x90
#define MESSAGE_POLY_AFTERTOUCH 0xA0
#define MESSAGE_CONTROL_CHANGE  0xB0
#define MESSAGE_PROGRAM_CHANGE  0xC0
#define MESSAGE_AFTERTOUCH      0xD0
#define MESSAGE_PITCH_BEND      0xE0
#define MESSAGE_SYSTEM          0xF0

#define CONTROL_CHANGE_DUTY     1
#define CONTROL_CHANGE_VOLUME   7
#define CONTROL_CHANGE_PAN      10
#define CONTROL_CHANGE_CLOCK    16
#define CONTROL_CHANGE_FREQ_MSB 24
#define CONTROL_CHANGE_FREQ_LSB 56
#define CONTROL_CHANGE_ALL_OFF  123
#define CONTROL_CHANGE_MONO     126
#define CONTROL_CHANGE_POLY     127

enum class WaveType {
    None,
    Sine,
    Triangle,
    Sawtooth,
    RSawtooth,
    Square,
    Noise,
    Custom,
    PitchedNoise = 22
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
static PortMidiStream * stream = NULL;
static std::chrono::system_clock::time_point startTime;

static void ChannelInfo_destructor(Computer * comp, int id, void* data) {
    ChannelInfo * channels = (ChannelInfo*)data;
    delete[] channels;
}

#define trigger_time milliseconds(NULL)
static PmTimestamp milliseconds(void *time_info) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - startTime).count();
}

static void sendMessage(uint8_t message, uint8_t channel, uint8_t param1, uint8_t param2) {
    Pm_WriteShort(stream, trigger_time, Pm_Message(message | channel, param1, param2));
}

static void sendDualMessage(uint8_t message, uint8_t channel, uint8_t param1, uint8_t param2, uint8_t message2, uint8_t channel2, uint8_t param12, uint8_t param22) {
    PmEvent e[2];
    e[0].message = Pm_Message(message | channel, param1, param2);
    e[0].timestamp = trigger_time;
    e[1].message = Pm_Message(message2 | channel2, param12, param22);
    e[1].timestamp = trigger_time;
    Pm_Write(stream, e, 2);
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
    WaveType old = info->wavetype;
    double oldduty = info->duty;
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
    if (info->wavetype != old || info->duty != oldduty) {
        if (info->wavetype == WaveType::Square) {
            sendMessage(MESSAGE_CONTROL_CHANGE, channel - 1, CONTROL_CHANGE_DUTY, info->duty * 127);
            if (old != WaveType::Square) sendMessage(MESSAGE_PROGRAM_CHANGE, channel - 1, (uint8_t)info->wavetype, 0);
        } else if (info->wavetype == WaveType::Custom) {
            /*fputc(command(0, channel), output);
            fputc((int)info->wavetype | (((info->customWaveSize - 1) >> 1) & 0x80), output);
            fputc((info->customWaveSize - 1) & 0xFF, output);*/
        } else sendMessage(MESSAGE_PROGRAM_CHANGE, channel - 1, (uint8_t)info->wavetype, 0);
    }
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
    if (info->frequency != frequency) {
        info->frequency = frequency;
        uint16_t freq = frequency & 0x3FFF;
        sendDualMessage(
            MESSAGE_CONTROL_CHANGE, channel - 1, CONTROL_CHANGE_FREQ_LSB, freq & 0x7F,
            MESSAGE_CONTROL_CHANGE, channel - 1, CONTROL_CHANGE_FREQ_MSB, freq >> 7
        );
    }
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
    if (abs(info->amplitude - amplitude) >= .0078125) {
        info->amplitude = amplitude;
        sendMessage(MESSAGE_CONTROL_CHANGE, channel - 1, CONTROL_CHANGE_VOLUME, amplitude * 127);
    }
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
    sendMessage(MESSAGE_CONTROL_CHANGE, channel - 1, CONTROL_CHANGE_PAN, (pan + 1.0) * 63.5);
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
    // not implemented
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
    if (time > (127.0/64.0)) time = 127.0/64.0;
    else if (time <= 0) return 0;
    sendMessage(MESSAGE_NOTE_OFF, channel - 1, 0, 127 - floor(time * 64));
    info->amplitude = 0.0;
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

extern "C" {
#ifdef _WIN32
_declspec(dllexport)
#endif
PluginInfo * plugin_init(const PluginFunctions * func, const path_t& path) {
    if (func->abi_version != PLUGIN_VERSION) return &info;
    ::func = func;
    startTime = std::chrono::system_clock::now();
    PmError error;
    if ((error = Pm_Initialize()) != pmNoError) throw std::runtime_error(std::string("Could not init: ") + Pm_GetErrorText(error));
    for (int i = 0; i < Pm_CountDevices(); i++) {
        const PmDeviceInfo * inf = Pm_GetDeviceInfo(i);
        if (inf == NULL) throw std::invalid_argument("No PSG device found");
        if (inf->output && strstr(inf->name, "PSG")) {
            if ((error = Pm_OpenOutput(&stream, i, NULL, 256, milliseconds, NULL, 10)) != pmNoError) throw std::runtime_error(std::string("Could not open: ") + Pm_GetErrorText(error));
            printf("Opened MIDI device %s\n", inf->name);
            break;
        }
    }
    if (stream == NULL) throw std::invalid_argument("No PSG device found");
    sendMessage(MESSAGE_CONTROL_CHANGE, 0, CONTROL_CHANGE_MONO, 0);
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
    sendMessage(MESSAGE_CONTROL_CHANGE, 0, CONTROL_CHANGE_POLY, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Pm_Close(stream);
    Pm_Terminate();
}
}
