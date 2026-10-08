#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <cstdio>
#include <cwchar>
#include <vector>
#include <string>
#include <cerrno>

namespace {

struct Options
{
    std::wstring display;
    bool list = false;
    bool exercise = false;
    bool capabilitiesString = false;
    bool brightnessOnly = false;
    DWORD holdMs = 2000;
    DWORD physicalIndex = 0;
    bool failed = false;
    unsigned matched = 0;
};

void PrintError(const wchar_t* operation, DWORD error = GetLastError())
{
    const wchar_t* detail = error == static_cast<DWORD>(ERROR_GRAPHICS_MCA_UNSUPPORTED_MCCS_VERSION) ?
        L" [ERROR_GRAPHICS_MCA_UNSUPPORTED_MCCS_VERSION]" : L"";
    std::fwprintf(stderr, L"%ls failed: Win32=%lu (0x%08lX)%ls\n", operation, error, error, detail);
}

bool Number(const wchar_t* text, DWORD maximum, DWORD* value)
{
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::wcstoul(text, &end, 10);
    if (text[0] < L'0' || text[0] > L'9' || *end != L'\0' ||
        errno == ERANGE || parsed > maximum)
    {
        return false;
    }
    *value = parsed;
    return true;
}

void PrintDisplayPath(const wchar_t* display)
{
    for (unsigned attempt = 0; attempt < 3; ++attempt)
    {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        LONG status = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
        if (status != ERROR_SUCCESS)
        {
            PrintError(L"GetDisplayConfigBufferSizes", status);
            return;
        }
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,
            &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (status == ERROR_INSUFFICIENT_BUFFER)
        {
            continue;
        }
        if (status != ERROR_SUCCESS)
        {
            PrintError(L"QueryDisplayConfig", status);
            return;
        }
        for (UINT32 index = 0; index < pathCount; ++index)
        {
            const auto& path = paths[index];
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
            source.header = { DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source),
                path.sourceInfo.adapterId, path.sourceInfo.id };
            status = DisplayConfigGetDeviceInfo(&source.header);
            if (status != ERROR_SUCCESS)
            {
                PrintError(L"DisplayConfigGetDeviceInfo(source)", status);
                continue;
            }
            if (std::wcscmp(display, source.viewGdiDeviceName) != 0)
            {
                continue;
            }
            DISPLAYCONFIG_TARGET_DEVICE_NAME target = {};
            target.header = { DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target),
                path.targetInfo.adapterId, path.targetInfo.id };
            status = DisplayConfigGetDeviceInfo(&target.header);
            if (status == ERROR_SUCCESS)
            {
                std::wprintf(L"  Target: %ls\n  Monitor path: %ls\n  Output technology: %u\n",
                    target.monitorFriendlyDeviceName, target.monitorDevicePath,
                    static_cast<unsigned>(target.outputTechnology));
            }
            else
            {
                PrintError(L"DisplayConfigGetDeviceInfo(target)", status);
            }
            DISPLAYCONFIG_ADAPTER_NAME adapter = {};
            adapter.header = { DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME, sizeof(adapter),
                path.targetInfo.adapterId, 0 };
            status = DisplayConfigGetDeviceInfo(&adapter.header);
            if (status == ERROR_SUCCESS)
            {
                std::wprintf(L"  Adapter path: %ls\n", adapter.adapterDevicePath);
            }
            else
            {
                PrintError(L"DisplayConfigGetDeviceInfo(adapter)", status);
            }
        }
        return;
    }
    PrintError(L"QueryDisplayConfig (topology changed repeatedly)", ERROR_RETRY);
}

struct PhysicalMonitors
{
    std::vector<PHYSICAL_MONITOR> values;
    bool acquired = false;
    bool Close()
    {
        if (!acquired)
        {
            return true;
        }
        acquired = false;
        if (!DestroyPhysicalMonitors(static_cast<DWORD>(values.size()), values.data()))
        {
            PrintError(L"DestroyPhysicalMonitors");
            return false;
        }
        return true;
    }
    ~PhysicalMonitors() { Close(); }
};

bool ReadBrightness(HANDLE monitor, DWORD* minimum, DWORD* current, DWORD* maximum)
{
    if (!GetMonitorBrightness(monitor, minimum, current, maximum))
    {
        PrintError(L"GetMonitorBrightness");
        return false;
    }
    std::wprintf(L"  Brightness: min=%lu current=%lu max=%lu\n", *minimum, *current, *maximum);
    if (*minimum > *current || *current > *maximum)
    {
        PrintError(L"Invalid brightness range", ERROR_INVALID_DATA);
        return false;
    }
    return true;
}

bool VerifyBrightness(HANDLE monitor, DWORD expected)
{
    for (unsigned attempt = 0; attempt < 5; ++attempt)
    {
        Sleep(100);
        DWORD minimum = 0, current = 0, maximum = 0;
        if (!ReadBrightness(monitor, &minimum, &current, &maximum))
        {
            return false;
        }
        if (current == expected)
        {
            return true;
        }
    }
    PrintError(L"Brightness readback differs from requested value", ERROR_INVALID_DATA);
    return false;
}

bool ExerciseBrightness(HANDLE monitor, DWORD minimum, DWORD original, DWORD maximum, DWORD holdMs)
{
    if (minimum == maximum)
    {
        PrintError(L"Brightness range is not adjustable", ERROR_NOT_SUPPORTED);
        return false;
    }
    const DWORD step = (maximum - minimum) / 20 != 0 ? (maximum - minimum) / 20 : 1;
    const DWORD changed = maximum - original >= step ? original + step :
        original - (original - minimum < step ? original - minimum : step);
    std::wprintf(L"  EXERCISE: %lu -> %lu; restore after %lu ms\n", original, changed, holdMs);
    bool success = SetMonitorBrightness(monitor, changed) != FALSE;
    if (!success)
    {
        PrintError(L"SetMonitorBrightness(test)");
    }
    else
    {
        success = VerifyBrightness(monitor, changed);
        Sleep(holdMs);
    }
    // Attempt restoration even if a write reports failure after reaching the monitor.
    std::wprintf(L"  RESTORE: %lu\n", original);
    if (!SetMonitorBrightness(monitor, original))
    {
        PrintError(L"SetMonitorBrightness(restore)");
        return false;
    }
    const bool restored = VerifyBrightness(monitor, original);
    std::wprintf(L"  RESTORATION VERIFIED: %ls\n", restored ? L"YES" : L"NO");
    return success && restored;
}

bool InspectMonitor(HANDLE monitor, const Options& options)
{
    bool success = true;
    DWORD capabilities = 0, colorTemperatures = 0;
    if (options.brightnessOnly)
    {
        std::wprintf(L"  GetMonitorCapabilities: SKIPPED (--brightness-only)\n");
    }
    else if (GetMonitorCapabilities(monitor, &capabilities, &colorTemperatures))
    {
        std::wprintf(L"  GetMonitorCapabilities: caps=0x%08lX temperatures=0x%08lX brightness=%ls\n",
            capabilities, colorTemperatures,
            (capabilities & MC_CAPS_BRIGHTNESS) != 0 ? L"YES" : L"NO");
    }
    else
    {
        PrintError(L"GetMonitorCapabilities");
        success = false;
    }
    DWORD minimum = 0, current = 0, maximum = 0;
    const bool brightness = ReadBrightness(monitor, &minimum, &current, &maximum);
    success = brightness && success;
    if (options.capabilitiesString)
    {
        DWORD length = 0;
        if (!GetCapabilitiesStringLength(monitor, &length))
        {
            PrintError(L"GetCapabilitiesStringLength");
            success = false;
        }
        else if (length == 0 || length > 65536)
        {
            PrintError(L"Invalid capabilities string length", ERROR_INVALID_DATA);
            success = false;
        }
        else
        {
            std::vector<char> text(length, '\0');
            if (!CapabilitiesRequestAndCapabilitiesReply(monitor, text.data(), length))
            {
                PrintError(L"CapabilitiesRequestAndCapabilitiesReply");
                success = false;
            }
            else if (text.back() != '\0')
            {
                PrintError(L"Unterminated capabilities string", ERROR_INVALID_DATA);
                success = false;
            }
            else
            {
                std::wprintf(L"  MCCS capabilities: %hs\n", text.data());
            }
        }
    }
    if (options.exercise)
    {
        if (!brightness)
        {
            std::fwprintf(stderr, L"Brightness exercise refused: original value was not readable.\n");
            return false;
        }
        success = ExerciseBrightness(monitor, minimum, current, maximum, options.holdMs) && success;
    }
    return success;
}

BOOL CALLBACK EnumerateMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter)
{
    auto& options = *reinterpret_cast<Options*>(parameter);
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info))
    {
        PrintError(L"GetMonitorInfoW");
        options.failed = true;
        return TRUE;
    }
    if (!options.display.empty() && _wcsicmp(info.szDevice, options.display.c_str()) != 0)
    {
        return TRUE;
    }
    ++options.matched;
    std::wprintf(L"\nDisplay: %ls primary=%ls bounds=(%ld,%ld)-(%ld,%ld)\n",
        info.szDevice, (info.dwFlags & MONITORINFOF_PRIMARY) ? L"YES" : L"NO",
        info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right, info.rcMonitor.bottom);
    PrintDisplayPath(info.szDevice);
    DWORD count = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(monitor, &count))
    {
        PrintError(L"GetNumberOfPhysicalMonitorsFromHMONITOR");
        options.failed = true;
        return TRUE;
    }
    if (count == 0 || count > 256)
    {
        PrintError(L"Invalid physical monitor count", ERROR_INVALID_DATA);
        options.failed = true;
        return TRUE;
    }
    PhysicalMonitors physical;
    physical.values.resize(count);
    if (!GetPhysicalMonitorsFromHMONITOR(monitor, count, physical.values.data()))
    {
        PrintError(L"GetPhysicalMonitorsFromHMONITOR");
        options.failed = true;
        return TRUE;
    }
    physical.acquired = true;
    for (DWORD index = 0; index < count; ++index)
    {
        const auto& item = physical.values[index];
        std::wprintf(L"  Physical[%lu]: %ls\n", index, item.szPhysicalMonitorDescription);
        if (!options.list && (!options.exercise || index == options.physicalIndex))
        {
            options.failed = !InspectMonitor(item.hPhysicalMonitor, options) || options.failed;
        }
    }
    if (options.exercise && options.physicalIndex >= count)
    {
        PrintError(L"Requested physical monitor index is out of range", ERROR_INVALID_PARAMETER);
        options.failed = true;
    }
    options.failed = !physical.Close() || options.failed;
    return TRUE;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    Options options;
    bool valid = true;
    for (int index = 1; index < argc && valid; ++index)
    {
        const std::wstring argument = argv[index];
        if (argument == L"--list") { options.list = true; }
        else if (argument == L"--exercise") { options.exercise = true; }
        else if (argument == L"--capabilities-string") { options.capabilitiesString = true; }
        else if (argument == L"--brightness-only") { options.brightnessOnly = true; }
        else if (argument == L"--display" && index + 1 < argc) { options.display = argv[++index]; }
        else if (argument == L"--hold-ms" && index + 1 < argc)
        {
            valid = Number(argv[++index], 60000, &options.holdMs);
        }
        else if (argument == L"--physical" && index + 1 < argc)
        {
            valid = Number(argv[++index], 255, &options.physicalIndex);
        }
        else { valid = false; }
    }
    if (!valid || (options.exercise && (options.display.empty() || options.list)))
    {
        std::fwprintf(stderr,
            L"Usage: monitor-ddc [--list] [--display \\\\.\\DISPLAYn] [--capabilities-string] [--brightness-only]\n"
            L"       monitor-ddc --display \\\\.\\DISPLAYn --exercise [--physical N] [--hold-ms 0..60000] [--brightness-only]\n"
            L"Default is read-only. Exercise changes brightness by ~5%% and restores/verifies it.\n");
        return 2;
    }
    std::wprintf(L"Monitor DDC test: %ls (run in the interactive desktop, not an SSH/RDP session)\n",
        options.exercise ? L"BRIGHTNESS EXERCISE" : L"READ ONLY");
    if (!EnumDisplayMonitors(nullptr, nullptr, EnumerateMonitor, reinterpret_cast<LPARAM>(&options)))
    {
        PrintError(L"EnumDisplayMonitors");
        return 1;
    }
    if (options.matched == 0)
    {
        PrintError(L"No matching active display", ERROR_NOT_FOUND);
        return 1;
    }
    std::wprintf(L"\nRESULT: %ls\n", options.failed ? L"FAIL" : L"PASS");
    return options.failed ? 1 : 0;
}
