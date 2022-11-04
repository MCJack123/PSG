#include <portmidi.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <cstring>

#if defined(__linux__)
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static std::vector<std::string> split(const std::string& strToSplit, const char * delims) {
    std::vector<std::string> retval;
    size_t pos = strToSplit.find_first_not_of(delims);
    while (pos != std::string::npos) {
        const size_t end = strToSplit.find_first_of(delims, pos);
        retval.push_back(strToSplit.substr(pos, end - pos));
        pos = strToSplit.find_first_not_of(delims, end);
    }
    return retval;
}

std::string getMountpoint() {
    // wait for RPI-RP2 drive
    struct stat st;
    while (true) if (stat("/dev/disk/by-label/RPI-RP2", &st) == 0) break;
    std::this_thread::sleep_for(std::chrono::seconds(2)); // wait for possible mount
    // search for automount
    std::string node = std::to_string(major(st.st_rdev)) + ":" + std::to_string(minor(st.st_rdev));
    std::ifstream in("/proc/self/mountinfo");
    while (!in.eof()) {
        std::string line;
        std::getline(in, line);
        if (line.empty()) continue;
        std::vector<std::string> parts = split(line, " ");
        if (parts.empty()) continue;
        if (parts[2] == node) return parts[4];
    }
    in.close();
    // attempt to mount manually
    mkdir(".picomount", 0777);
    if (mount("/dev/disk/by-label/RPI-RP2", ".picomount", "vfat", MS_NOATIME, NULL) != 0) {
        std::cout << "Cannot find mount, and cannot mount disk manually\nPlease mount the RPI-RP2 disk manually and type the path here: ";
        std::string line;
        std::getline(std::cin, line);
        return line;
    }
    return ".picomount";
}

void mountCleanup() {
    sync();
}

#elif defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <winioctl.h>

#define BUFFER_SIZE 256
#define MAX_DRIVES 26

// Finds the device interface for the CDROM drive with the given interface number.
DEVINST GetDrivesDevInstByDeviceNumber(long DeviceNumber) {
    const GUID *guid = &GUID_DEVINTERFACE_DISK;
    // Get device interface info set handle
    // for all devices attached to system
    HDEVINFO hDevInfo = SetupDiGetClassDevs(guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE) return 0;
    // Retrieve a context structure for a device interface of a device information set.
    BYTE buf[1024];
    PSP_DEVICE_INTERFACE_DETAIL_DATA pspdidd = (PSP_DEVICE_INTERFACE_DETAIL_DATA)buf;
    SP_DEVICE_INTERFACE_DATA spdid;
    SP_DEVINFO_DATA spdd;
    DWORD dwSize;
    spdid.cbSize = sizeof(spdid);
    // Iterate through all the interfaces and try to match one based on
    // the device number.
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, NULL, guid, i, &spdid); i++) {
        // Get the device path.
        dwSize = 0;
        SetupDiGetDeviceInterfaceDetail(hDevInfo, &spdid, NULL, 0, &dwSize, NULL);
        if (dwSize == 0 || dwSize > sizeof(buf)) continue;
        pspdidd->cbSize = sizeof(*pspdidd);
        ZeroMemory((PVOID)&spdd, sizeof(spdd));
        spdd.cbSize = sizeof(spdd);
        if (!SetupDiGetDeviceInterfaceDetail(hDevInfo, &spdid, pspdidd, dwSize, &dwSize, &spdd)) continue;
        // Open the device.
        HANDLE hDrive = CreateFile(pspdidd->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (hDrive == INVALID_HANDLE_VALUE) continue;
        // Get the device number.
        STORAGE_DEVICE_NUMBER sdn;
        dwSize = 0;
        if (DeviceIoControl(hDrive, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &sdn, sizeof(sdn), &dwSize, NULL)) {
            // Does it match?
            if (DeviceNumber == (long)sdn.DeviceNumber) {
                CloseHandle(hDrive);
                SetupDiDestroyDeviceInfoList(hDevInfo);
                return spdd.DevInst;
            }
        }
        CloseHandle(hDrive);
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
    return 0;
}

// Returns true if the given device instance belongs to the USB device with the given VID and PID.
boolean matchDevInstToUsbDevice(DEVINST device, DWORD vid, DWORD pid) {
    // This is the string we will be searching for in the device harware IDs.
    TCHAR hwid[64];
    sprintf(hwid, "VID_%04X&PID_%04X", vid, pid);
    // Get a list of hardware IDs for all USB devices.
    ULONG ulLen;
    CM_Get_Device_ID_List_Size(&ulLen, NULL, CM_GETIDLIST_FILTER_NONE);
    TCHAR *pszBuffer = malloc(sizeof(TCHAR) * ulLen);
    CM_Get_Device_ID_List(NULL, pszBuffer, ulLen, CM_GETIDLIST_FILTER_NONE);
    // Iterate through the list looking for our ID.
    for (LPTSTR pszDeviceID = pszBuffer; *pszDeviceID; pszDeviceID += _tcslen(pszDeviceID) + 1) {
        // Some versions of Windows have the string in upper case and other versions have it
        // in lower case so just make it all upper.
        for (int i = 0; pszDeviceID[i]; i++) pszDeviceID[i] = toupper(pszDeviceID[i]);
        if (_tcsstr(pszDeviceID, hwid)) {
            // Found the device, now we want the grandchild device, which is the "generic volume"
            DEVINST MSDInst = 0;
            if (CR_SUCCESS == CM_Locate_DevNode(&MSDInst, pszDeviceID, CM_LOCATE_DEVNODE_NORMAL)) {
                DEVINST DiskDriveInst = 0;
                if (CR_SUCCESS == CM_Get_Child(&DiskDriveInst, MSDInst, 0)) {
                    // Now compare the grandchild node against the given device instance.
                    if (device == DiskDriveInst) return TRUE;
                }
            }
        }
    }
    return FALSE;
}

std::string getMountpoint() {
    // TODO: wait for device
    DWORD vid = 0x2e8a, pid = 0x0003;
    TCHAR caDrive[4] = TEXT("A:\\");
    TCHAR volume[BUFFER_SIZE];
    TCHAR volume_path_name[BUFFER_SIZE];
    DWORD dwDriveMask;
    int count = 0;
    // Get all drives in the system.
    dwDriveMask = GetLogicalDrives();
    if (dwDriveMask == 0) {
        std::cerr << "Error - GetLogicalDrives failed\n";
        return "";
    }
    // Loop for all drives.
    for (int nLoopIndex = 0; nLoopIndex < MAX_DRIVES; nLoopIndex++, dwDriveMask >>= 1) {
        // If a drive is present,
        if (dwDriveMask & 1) {
            caDrive[0] = TEXT('A') + nLoopIndex;
            // If a drive is removable.
            if (GetDriveType(caDrive) == DRIVE_REMOVABLE) {
                //Get its volume info.
                if (GetVolumeNameForVolumeMountPoint(caDrive, volume, BUFFER_SIZE)) {
                    DWORD lpcchReturnLength;
                    GetVolumePathNamesForVolumeName(volume, volume_path_name, BUFFER_SIZE, &lpcchReturnLength);
                    char szVolumeAccessPath[] = "\\\\.\\X:";
                    szVolumeAccessPath[4] = caDrive[0];
                    long DeviceNumber = -1;
                    HANDLE hVolume = CreateFile(szVolumeAccessPath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
                    if (hVolume == INVALID_HANDLE_VALUE) return 1;
                    STORAGE_DEVICE_NUMBER sdn;
                    DWORD dwBytesReturned = 0;
                    long res = DeviceIoControl(hVolume, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &sdn, sizeof(sdn), &dwBytesReturned, NULL);
                    if (res) DeviceNumber = sdn.DeviceNumber;
                    CloseHandle(hVolume);
                    if (DeviceNumber != -1) {
                        DEVINST DevInst = GetDrivesDevInstByDeviceNumber(DeviceNumber);
                        boolean match = matchDevInstToUsbDevice(DevInst, vid, pid);
                        if (match) return volume_path_name;
                    }
                    count++;
                }
            }
        }
    }
    std::cerr << "Could not find drive\n";
    return "";
}

void mountCleanup() {}

#elif defined(__APPLE__)
#error Not implemented
#else
#error Unsupported platform
#endif

static std::chrono::system_clock::time_point startTime = std::chrono::system_clock::now();
static PmTimestamp milliseconds(void *time_info) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - startTime).count();
}

int main(int argc, const char * argv[]) {
    std::string hexdata;
    uint8_t * uf2data;
    size_t uf2size = 0;
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <firmware.bin|uf2|hex>\n";
        return 1;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in.is_open()) {
        std::cerr << "Could not open input file\n";
        return 2;
    }
    std::cout << "Reading firmware file\n";
    std::string line;
    int i = 1;
    for (std::getline(in, line); !in.eof() && line != "UF2"; std::getline(in, line), i++) {
        if (!line.empty()) {
            if (line[0] != ':') {
                std::cerr << "Invalid HEX data on line " << i << "\n";
                return 3;
            }
            hexdata += line + "\n";
        }
    }
    if (!in.eof()) {
        uint8_t buf[508];
        uint32_t * buf32 = (uint32_t*)buf;
        in.read((char*)buf, 508);
        if (in.gcount() < 508 || buf32[0] != 0x9E5D5157) {
            std::cerr << "Invalid UF2 data\n";
            return 4;
        }
        uf2size = buf32[5] * 512;
        uf2data = new uint8_t[uf2size];
        uf2data[0] = 'U';
        uf2data[1] = 'F';
        uf2data[2] = '2';
        uf2data[3] = '\n';
        memcpy(uf2data + 4, buf, 508);
        in.read((char*)uf2data + 512, uf2size - 512);
    }
    in.close();
    if (uf2size == 0 && hexdata.empty()) {
        std::cerr << "No firmware data present\n";
        return 5;
    }
    std::cout << "Opening MIDI device\n";
    PortMidiStream * stream = NULL, * streamin = NULL;
    PmError error;
    if ((error = Pm_Initialize()) != pmNoError) throw std::runtime_error(std::string("Could not init: ") + Pm_GetErrorText(error));
    for (int i = 0; i < Pm_CountDevices(); i++) {
        const PmDeviceInfo * inf = Pm_GetDeviceInfo(i);
        if (inf == NULL) {
            std::cerr << "No PSG device found\n";
            return 6;
        }
        if (inf->output && strstr(inf->name, "PSG")) {
            if ((error = Pm_OpenOutput(&stream, i, NULL, 0, milliseconds, NULL, 0)) != pmNoError) {
                std::cerr << "Could not open device: " << Pm_GetErrorText(error) << "\n";
                return error;
            }
            std::cout << "Opened MIDI output device " << inf->name << "\n";
            if (streamin != NULL) break;
        }
        if (inf->input && strstr(inf->name, "PSG")) {
            if ((error = Pm_OpenInput(&streamin, i, NULL, 0, milliseconds, NULL)) != pmNoError) {
                std::cerr << "Could not open device: " << Pm_GetErrorText(error) << "\n";
                return error;
            }
            std::cout << "Opened MIDI input device " << inf->name << "\n";
            if (stream != NULL) break;
        }
    }
    if (stream == NULL || streamin == NULL) {
        std::cerr << "No PSG device found\n";
        return 6;
    }
    if (!hexdata.empty()) {
        std::cout << "Uploading PIC firmware (" << hexdata.size() << " bytes)\n";
        uint8_t * data = new uint8_t[hexdata.size()+7];
        data[0] = 0xF0;
        data[1] = 0x00;
        data[2] = 0x46;
        data[3] = 0x71;
        data[4] = 0x00;
        data[5] = 0;
        memcpy(data + 6, hexdata.c_str(), hexdata.size());
        data[hexdata.size() + 6] = 0xF7;
        Pm_WriteSysEx(stream, 0, data);
        std::cout << "Waiting for write to complete\n";
        while (!Pm_Poll(streamin)) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::cout << "Flash finished, reloading output\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // make sure the device has time to boot
        Pm_Close(stream);
        for (int i = 0; i < Pm_CountDevices(); i++) {
            const PmDeviceInfo * inf = Pm_GetDeviceInfo(i);
            if (inf == NULL) {
                std::cerr << "No PSG device found\n";
                return 6;
            }
            if (inf->output && strstr(inf->name, "PSG")) {
                if ((error = Pm_OpenOutput(&stream, i, NULL, 0, milliseconds, NULL, 0)) != pmNoError) {
                    std::cerr << "Could not open device: " << Pm_GetErrorText(error) << "\n";
                    return error;
                }
                std::cout << "Opened MIDI output device " << inf->name << "\n";
                break;
            }
        }
        if (stream == NULL) {
            std::cerr << "No PSG device found\n";
            return 6;
        }
    }
    if (uf2size) {
        std::cout << "Flipping device into bootloader mode\n";
        uint8_t msg[] = {0xF0, 0x00, 0x46, 0x71, 0x01, 0x00, 0xF7};
        Pm_WriteSysEx(stream, 0, msg);
        std::cout << "Waiting for USB device\n";
        std::string mountpoint = getMountpoint();
        if (mountpoint == "") return 6;
        std::cout << "Found Pico at " << mountpoint << "\nUploading Pico firmware (" << uf2size << " bytes)\n";
        std::ofstream out(mountpoint + "/firmware.uf2", std::ios::binary);
        out.write((char*)uf2data, uf2size);
        out.close();
        mountCleanup();
        delete[] uf2data;
        std::cout << "Upload finished, Pico will reboot momentarily\n";
    }
    Pm_Close(stream);
    Pm_Close(streamin);
    std::cout << "Finished programming device\n";
    return 0;
}