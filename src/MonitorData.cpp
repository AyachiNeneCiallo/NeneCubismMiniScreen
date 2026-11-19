#include "MonitorData.hpp"
#include "Common.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
// netioapi.h only declares GetIfTable2 when targeting Windows 10 RS2+.
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000003
#endif
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <shellapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ws2_32.lib")

namespace
{
    const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    const wchar_t* kRunValue = L"NeneCubism";

    unsigned long long FileTimeToU64(const FILETIME& ft)
    {
        ULARGE_INTEGER value;
        value.LowPart = ft.dwLowDateTime;
        value.HighPart = ft.dwHighDateTime;
        return value.QuadPart;
    }

    // --- NVML function signatures (nvml.h is not shipped with the SDK) -----
    typedef int nvmlReturn_t;                 // 0 == NVML_SUCCESS
    typedef void* nvmlDevice_t;
    struct nvmlMemory_t
    {
        unsigned long long total;
        unsigned long long free;
        unsigned long long used;
    };
    const unsigned int kNvmlTemperatureGpu = 0;

    typedef nvmlReturn_t (*NvmlInitV2)(void);
    typedef nvmlReturn_t (*NvmlShutdown)(void);
    typedef nvmlReturn_t (*NvmlGetHandleByIndexV2)(unsigned int, nvmlDevice_t*);
    typedef nvmlReturn_t (*NvmlGetMemoryInfo)(nvmlDevice_t, nvmlMemory_t*);
    typedef nvmlReturn_t (*NvmlGetTemperature)(nvmlDevice_t, unsigned int, unsigned int*);
    typedef nvmlReturn_t (*NvmlGetName)(nvmlDevice_t, char*, unsigned int);
    struct nvmlUtilization_t
    {
        unsigned int gpu;
        unsigned int memory;
    };
    typedef nvmlReturn_t (*NvmlGetUtilizationRates)(nvmlDevice_t, nvmlUtilization_t*);

    // --- nvidia-smi fallback ------------------------------------------------
    bool RunNvidiaSmi(char* out, size_t outBytes)
    {
        out[0] = '\0';
        HANDLE readPipe = nullptr;
        HANDLE writePipe = nullptr;
        SECURITY_ATTRIBUTES attributes = {};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        if (!CreatePipe(&readPipe, &writePipe, &attributes, 0))
        {
            return false;
        }
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.hStdOutput = writePipe;
        startup.hStdError = writePipe;
        startup.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION process = {};
        wchar_t command[] =
            L"nvidia-smi --query-gpu=memory.used,memory.total,temperature.gpu "
            L"--format=csv,noheader,nounits";
        const BOOL created = CreateProcessW(nullptr, command, nullptr, nullptr, TRUE,
                                            CREATE_NO_WINDOW, nullptr, nullptr,
                                            &startup, &process);
        CloseHandle(writePipe);
        if (!created)
        {
            CloseHandle(readPipe);
            return false;
        }

        DWORD total = 0;
        size_t remaining = outBytes - 1;
        while (remaining > 0)
        {
            DWORD read = 0;
            if (!ReadFile(readPipe, out + total, static_cast<DWORD>(remaining), &read, nullptr) ||
                read == 0)
            {
                break;
            }
            total += read;
            remaining -= read;
        }
        out[total] = '\0';
        WaitForSingleObject(process.hProcess, 3000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(readPipe);
        return total > 0;
    }
}

// ===========================================================================
// PerformanceMonitor
// ===========================================================================
PerformanceMonitor::PerformanceMonitor()
{
}

PerformanceMonitor::~PerformanceMonitor()
{
    ShutdownPdh();
    ShutdownNvml();
}

void PerformanceMonitor::Poll(MonitorData& out)
{
    PollCpu(out);
    PollPdhGpu(out);
    PollNvml(out);
    PollRam(out);
    PollNetwork(out);
}

// ---------------------------------------------------------------------------
// CPU: GetSystemTimes deltas (same idea as psutil.cpu_percent)
// ---------------------------------------------------------------------------
void PerformanceMonitor::PollCpu(MonitorData& out)
{
    FILETIME idleTime;
    FILETIME kernelTime;
    FILETIME userTime;
    if (!GetSystemTimes(&idleTime, &kernelTime, &userTime))
    {
        return;
    }
    const unsigned long long idle = FileTimeToU64(idleTime);
    const unsigned long long kernel = FileTimeToU64(kernelTime);
    const unsigned long long user = FileTimeToU64(userTime);
    if (_cpuPrimed)
    {
        const unsigned long long idleDelta = idle - _cpuPrevIdle;
        const unsigned long long kernelDelta = kernel - _cpuPrevKernel;
        const unsigned long long userDelta = user - _cpuPrevUser;
        const unsigned long long total = kernelDelta + userDelta;   // kernel includes idle
        if (total > 0)
        {
            const double busy = static_cast<double>(total - idleDelta);
            out.cpuPercent = std::max(0.0, std::min(100.0, busy * 100.0 / total));
            out.cpuValid = true;
        }
    }
    _cpuPrevIdle = idle;
    _cpuPrevKernel = kernel;
    _cpuPrevUser = user;
    _cpuPrimed = true;
}

// ---------------------------------------------------------------------------
// GPU engine utilisation via PDH: \GPU Engine(*)\Utilization Percentage
// ---------------------------------------------------------------------------
void PerformanceMonitor::InitPdh()
{
    PDH_HQUERY query = nullptr;
    PDH_STATUS status = PdhOpenQueryW(nullptr, 0, &query);
    if (status != ERROR_SUCCESS)
    {
        LogMsg("[pdh] PdhOpenQuery failed: 0x%08lX", static_cast<unsigned long>(status));
        return;
    }
    _pdhQuery = query;

    PDH_HCOUNTER counter = nullptr;
    status = PdhAddEnglishCounterW(query,
                                   L"\\GPU Engine(*)\\Utilization Percentage",
                                   0, &counter);
    if (status != ERROR_SUCCESS)
    {
        LogMsg("[pdh] PdhAddEnglishCounter failed: 0x%08lX, retrying localised",
               static_cast<unsigned long>(status));
        status = PdhAddCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage",
                                0, &counter);
    }
    if (status != ERROR_SUCCESS)
    {
        LogMsg("[pdh] PdhAddCounter failed: 0x%08lX", static_cast<unsigned long>(status));
        PdhCloseQuery(query);
        _pdhQuery = nullptr;
        return;
    }
    _pdhCounter = counter;
    _pdhStatus = status;

    // Prime the rate counter; the first collection only starts the clock.
    PdhCollectQueryData(query);
    _pdhPrimed = true;
    _pdhReady = true;
    LogMsg("[pdh] GPU Engine counter ready");
}

void PerformanceMonitor::ShutdownPdh()
{
    if (_pdhQuery)
    {
        PdhCloseQuery(static_cast<PDH_HQUERY>(_pdhQuery));
    }
    _pdhQuery = nullptr;
    _pdhCounter = nullptr;
    _pdhReady = false;
}

void PerformanceMonitor::PollPdhGpu(MonitorData& out)
{
    if (!_pdhReady)
    {
        if (_pdhQuery == nullptr)
        {
            InitPdh();
        }
        return;
    }

    PDH_HQUERY query = static_cast<PDH_HQUERY>(_pdhQuery);
    PDH_HCOUNTER counter = static_cast<PDH_HCOUNTER>(_pdhCounter);
    const PDH_STATUS collectStatus = PdhCollectQueryData(query);
    if (collectStatus != ERROR_SUCCESS)
    {
        return;
    }

    DWORD bufferSize = 0;
    DWORD itemCount = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(
        counter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, nullptr);
    if (status != PDH_MORE_DATA || bufferSize == 0)
    {
        return;
    }
    std::vector<unsigned char> buffer(bufferSize);
    status = PdhGetFormattedCounterArrayW(
        counter, PDH_FMT_DOUBLE, &bufferSize, &itemCount,
        reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data()));
    if (status != ERROR_SUCCESS)
    {
        return;
    }

    const PDH_FMT_COUNTERVALUE_ITEM_W* items =
        reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    double max3d = 0.0;
    double maxCuda = 0.0;
    int count3d = 0;
    int countCuda = 0;
    for (DWORD i = 0; i < itemCount; ++i)
    {
        if (items[i].szName == nullptr || items[i].FmtValue.CStatus != 0)
        {
            continue;
        }
        const wchar_t* name = items[i].szName;
        const double value = items[i].FmtValue.doubleValue;
        if (wcsstr(name, L"engtype_3D") != nullptr)
        {
            ++count3d;
            if (value > max3d)
            {
                max3d = value;
            }
        }
        else if (wcsstr(name, L"engtype_Cuda") != nullptr ||
                 wcsstr(name, L"engtype_Compute") != nullptr)
        {
            ++countCuda;
            if (value > maxCuda)
            {
                maxCuda = value;
            }
        }
    }

    if (count3d > 0)
    {
        out.gpu3dPercent = std::max(0.0, std::min(100.0, max3d));
        out.gpu3dValid = true;
    }
    if (countCuda > 0)
    {
        out.gpuCudaPercent = std::max(0.0, std::min(100.0, maxCuda));
        out.gpuCudaValid = true;
    }
}

// ---------------------------------------------------------------------------
// NVML: VRAM + temperature; nvidia-smi fallback when the DLL is missing
// ---------------------------------------------------------------------------
void PerformanceMonitor::InitNvml()
{
    if (_nvmlModule != nullptr)
    {
        return;
    }
    const wchar_t* candidates[] = {
        L"nvml.dll",
        L"C:\\Windows\\System32\\nvml.dll",
        L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll"
    };
    HMODULE module = nullptr;
    for (const wchar_t* path : candidates)
    {
        module = LoadLibraryW(path);
        if (module)
        {
            LogMsg("[nvml] loaded %s", Utf8FromWide(path).c_str());
            break;
        }
    }
    if (!module)
    {
        LogMsg("[nvml] nvml.dll not found; falling back to nvidia-smi");
        return;
    }

    NvmlInitV2 init = reinterpret_cast<NvmlInitV2>(GetProcAddress(module, "nvmlInit_v2"));
    NvmlGetHandleByIndexV2 getHandle = reinterpret_cast<NvmlGetHandleByIndexV2>(
        GetProcAddress(module, "nvmlDeviceGetHandleByIndex_v2"));
    NvmlGetMemoryInfo getMemory = reinterpret_cast<NvmlGetMemoryInfo>(
        GetProcAddress(module, "nvmlDeviceGetMemoryInfo"));
    NvmlGetTemperature getTemp = reinterpret_cast<NvmlGetTemperature>(
        GetProcAddress(module, "nvmlDeviceGetTemperature"));
    NvmlGetName getName = reinterpret_cast<NvmlGetName>(
        GetProcAddress(module, "nvmlDeviceGetName"));
    NvmlGetUtilizationRates getUtilization = reinterpret_cast<NvmlGetUtilizationRates>(
        GetProcAddress(module, "nvmlDeviceGetUtilizationRates"));
    if (!init || !getHandle || !getMemory)
    {
        LogMsg("[nvml] required exports missing");
        FreeLibrary(module);
        return;
    }
    const nvmlReturn_t initResult = init();
    if (initResult != 0)
    {
        LogMsg("[nvml] nvmlInit_v2 failed: %d", initResult);
        FreeLibrary(module);
        return;
    }

    nvmlDevice_t device = nullptr;
    if (getHandle(0, &device) != 0 || device == nullptr)
    {
        LogMsg("[nvml] nvmlDeviceGetHandleByIndex_v2 failed");
        return;
    }

    _nvmlModule = module;
    _nvmlDevice = device;
    _nvmlInit = reinterpret_cast<void*>(init);
    _nvmlGetHandle = reinterpret_cast<void*>(getHandle);
    _nvmlGetMemory = reinterpret_cast<void*>(getMemory);
    _nvmlGetTemp = reinterpret_cast<void*>(getTemp);
    _nvmlGetName = reinterpret_cast<void*>(getName);
    _nvmlGetUtilization = reinterpret_cast<void*>(getUtilization);
    _nvmlReady = true;

    if (getName)
    {
        char name[128] = {};
        if (getName(device, name, sizeof(name)) == 0)
        {
            LogMsg("[nvml] GPU: %s", name);
        }
    }
    LogMsg("[nvml] ready");
}

void PerformanceMonitor::ShutdownNvml()
{
    if (_nvmlModule)
    {
        // Deliberately no nvmlShutdown: the DLL may be shared and the process
        // is exiting anyway.
        FreeLibrary(static_cast<HMODULE>(_nvmlModule));
    }
    _nvmlModule = nullptr;
    _nvmlDevice = nullptr;
    _nvmlReady = false;
}

bool PerformanceMonitor::PollNvidiaSmi(MonitorData& out)
{
    char output[4096] = {};
    if (!RunNvidiaSmi(output, sizeof(output)))
    {
        return false;
    }
    double usedMiB = 0.0;
    double totalMiB = 0.0;
    double tempC = 0.0;
    const int parsed = sscanf_s(output, "%lf, %lf, %lf", &usedMiB, &totalMiB, &tempC);
    if (parsed < 2 || totalMiB <= 0.0)
    {
        return false;
    }
    _smiMemUsedGB = usedMiB / 1024.0;
    _smiMemPercent = std::max(0.0, std::min(100.0, usedMiB / totalMiB * 100.0));
    _smiTempC = tempC;
    _smiValid = true;
    return true;
}

void PerformanceMonitor::PollNvml(MonitorData& out)
{
    if (!_nvmlReady)
    {
        InitNvml();
        if (!_nvmlReady)
        {
            // Only spawn nvidia-smi every third sample to keep CPU low.
            if (_smiTick++ % 3 == 0)
            {
                if (!PollNvidiaSmi(out))
                {
                    return;
                }
            }
            if (_smiValid)
            {
                out.gpuMemValid = true;
                out.gpuMemPercent = _smiMemPercent;
                out.gpuMemUsedGB = _smiMemUsedGB;
                out.gpuTempValid = _smiTempC > 0.0;
                out.gpuTempC = _smiTempC;
            }
            return;
        }
    }

    NvmlGetMemoryInfo getMemory =
        reinterpret_cast<NvmlGetMemoryInfo>(_nvmlGetMemory);
    nvmlMemory_t memory = {};
    if (getMemory && getMemory(static_cast<nvmlDevice_t>(_nvmlDevice), &memory) == 0 &&
        memory.total > 0)
    {
        out.gpuMemPercent = std::max(0.0, std::min(100.0,
            static_cast<double>(memory.used) * 100.0 / static_cast<double>(memory.total)));
        out.gpuMemUsedGB = static_cast<double>(memory.used) / (1024.0 * 1024.0 * 1024.0);
        out.gpuMemValid = true;
    }

    NvmlGetTemperature getTemp = reinterpret_cast<NvmlGetTemperature>(_nvmlGetTemp);
    unsigned int temperature = 0;
    if (getTemp && getTemp(static_cast<nvmlDevice_t>(_nvmlDevice), kNvmlTemperatureGpu,
                           &temperature) == 0)
    {
        out.gpuTempC = static_cast<double>(temperature);
        out.gpuTempValid = true;
    }

    // PDH could not find a 3D/CUDA engine (e.g. old driver): reuse the overall
    // GPU utilisation, exactly like the Python version does.
    if (!out.gpu3dValid && !out.gpuCudaValid)
    {
        NvmlGetUtilizationRates getUtilization =
            reinterpret_cast<NvmlGetUtilizationRates>(_nvmlGetUtilization);
        nvmlUtilization_t utilization = {};
        if (getUtilization &&
            getUtilization(static_cast<nvmlDevice_t>(_nvmlDevice), &utilization) == 0)
        {
            out.gpu3dPercent = std::max(0.0, std::min(100.0,
                static_cast<double>(utilization.gpu)));
            out.gpu3dValid = true;
            out.gpuCudaPercent = out.gpu3dPercent;
            out.gpuCudaValid = true;
        }
    }
}

// ---------------------------------------------------------------------------
// RAM: GlobalMemoryStatusEx
// ---------------------------------------------------------------------------
void PerformanceMonitor::PollRam(MonitorData& out)
{
    MEMORYSTATUSEX status = {};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status))
    {
        return;
    }
    out.ramPercent = static_cast<double>(status.dwMemoryLoad);
    if (status.ullTotalPhys > 0)
    {
        const double used = static_cast<double>(status.ullTotalPhys - status.ullAvailPhys);
        out.ramPercent = used * 100.0 / static_cast<double>(status.ullTotalPhys);
    }
    out.ramPercent = std::max(0.0, std::min(100.0, out.ramPercent));
    out.ramValid = true;
}

// ---------------------------------------------------------------------------
// Network: GetIfTable2, sum all non-loopback operational interfaces
// ---------------------------------------------------------------------------
void PerformanceMonitor::PollNetwork(MonitorData& out)
{
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || table == nullptr)
    {
        return;
    }
    unsigned long long totalIn = 0;
    unsigned long long totalOut = 0;
    for (ULONG i = 0; i < table->NumEntries; ++i)
    {
        const MIB_IF_ROW2& row = table->Table[i];
        if (row.OperStatus != IfOperStatusUp)
        {
            continue;
        }
        if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK)
        {
            continue;
        }
        totalIn += row.InOctets;
        totalOut += row.OutOctets;
    }
    FreeMibTable(table);

    LARGE_INTEGER frequency;
    LARGE_INTEGER now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    if (_netPrimed && _netPrevQpc != 0 && now.QuadPart > _netPrevQpc)
    {
        const double seconds = static_cast<double>(now.QuadPart - _netPrevQpc) /
                               static_cast<double>(frequency.QuadPart);
        if (seconds > 0.05)
        {
            const double up = totalOut >= _netPrevOut
                ? static_cast<double>(totalOut - _netPrevOut) / seconds : 0.0;
            const double down = totalIn >= _netPrevIn
                ? static_cast<double>(totalIn - _netPrevIn) / seconds : 0.0;
            out.netUpBytesPerSec = std::max(0.0, up);
            out.netDownBytesPerSec = std::max(0.0, down);
            out.netValid = true;
        }
    }
    _netPrevIn = totalIn;
    _netPrevOut = totalOut;
    _netPrevQpc = static_cast<unsigned long long>(now.QuadPart);
    _netPrimed = true;
}

// ===========================================================================
// Autostart helpers
// ===========================================================================
bool IsAutostartEnabled()
{
    DWORD type = 0;
    DWORD size = 0;
    const LONG result = RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue,
                                     RRF_RT_REG_SZ, &type, nullptr, &size);
    return result == ERROR_SUCCESS;
}

bool SetAutostart(bool enabled, const std::wstring& command)
{
    HKEY key = nullptr;
    const LONG openResult = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0,
                                          KEY_SET_VALUE, &key);
    if (openResult != ERROR_SUCCESS)
    {
        LogMsg("[autostart] RegOpenKeyEx failed: %ld", openResult);
        return false;
    }
    LONG result = ERROR_SUCCESS;
    if (enabled)
    {
        result = RegSetValueExW(key, kRunValue, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }
    else
    {
        result = RegDeleteValueW(key, kRunValue);
        if (result == ERROR_FILE_NOT_FOUND)
        {
            result = ERROR_SUCCESS;
        }
    }
    RegCloseKey(key);
    if (result != ERROR_SUCCESS)
    {
        LogMsg("[autostart] registry update failed: %ld", result);
        return false;
    }
    LogMsg("[autostart] %s: %s", enabled ? "enabled" : "disabled",
           Utf8FromWide(command).c_str());
    return true;
}

std::wstring BuildAutostartCommand(const std::wstring& modulePath)
{
    if (modulePath.empty())
    {
        return std::wstring();
    }
    // Keep the launch options (e.g. --topmost, --ui-scale) but never persist
    // the one-shot install/remove switches.
    std::wstring command = std::wstring(L"\"") + modulePath + L"\"";
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr)
    {
        return command;
    }
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        if (arg == L"--install-autostart" || arg == L"--remove-autostart")
        {
            continue;
        }
        if (arg.find(L' ') != std::wstring::npos && arg.front() != L'"')
        {
            command += L" \"";
            command += arg;
            command += L"\"";
        }
        else
        {
            command += L" ";
            command += arg;
        }
    }
    LocalFree(argv);
    return command;
}
