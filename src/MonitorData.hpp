#pragma once

#include <string>

// One sampled frame of the system performance data shown in the side panels.
// Every field has a "valid" flag so the UI can print "--" instead of a bogus
// number when a source (e.g. NVML) is unavailable.
struct MonitorData
{
    bool   cpuValid = false;
    double cpuPercent = 0.0;        // 0..100, all cores (GetSystemTimes)

    bool   gpu3dValid = false;
    double gpu3dPercent = 0.0;      // max over \GPU Engine(*)\engtype_3D
    bool   gpuCudaValid = false;
    double gpuCudaPercent = 0.0;    // max over \GPU Engine(*)\engtype_Cuda

    bool   gpuMemValid = false;
    double gpuMemPercent = 0.0;     // 0..100
    double gpuMemUsedGB = 0.0;

    bool   gpuTempValid = false;
    double gpuTempC = 0.0;

    bool   ramValid = false;
    double ramPercent = 0.0;        // 0..100

    bool   netValid = false;
    double netUpBytesPerSec = 0.0;
    double netDownBytesPerSec = 0.0;
};

// Samples every source once per Poll(). Must be called about once per second
// (CPU and network are computed from deltas); the first call only primes them.
class PerformanceMonitor
{
public:
    PerformanceMonitor();
    ~PerformanceMonitor();

    void Poll(MonitorData& out);

private:
    void InitPdh();
    void ShutdownPdh();
    void PollPdhGpu(MonitorData& out);
    void InitNvml();
    void ShutdownNvml();
    void PollNvml(MonitorData& out);
    bool PollNvidiaSmi(MonitorData& out);
    void PollCpu(MonitorData& out);
    void PollRam(MonitorData& out);
    void PollNetwork(MonitorData& out);

    // PDH
    void* _pdhQuery = nullptr;      // PDH_HQUERY
    void* _pdhCounter = nullptr;    // PDH_HCOUNTER
    bool  _pdhReady = false;
    long  _pdhStatus = 0;
    long  _pdhPrimed = false;

    // NVML
    void* _nvmlModule = nullptr;    // HMODULE
    void* _nvmlDevice = nullptr;    // nvmlDevice_t
    bool  _nvmlReady = false;
    void* _nvmlInit = nullptr;
    void* _nvmlGetHandle = nullptr;
    void* _nvmlGetMemory = nullptr;
    void* _nvmlGetTemp = nullptr;
    void* _nvmlGetName = nullptr;
    void* _nvmlGetUtilization = nullptr;
    int   _smiTick = 0;
    bool  _smiValid = false;
    double _smiMemUsedGB = 0.0;
    double _smiMemPercent = 0.0;
    double _smiTempC = 0.0;

    // CPU
    unsigned long long _cpuPrevIdle = 0;
    unsigned long long _cpuPrevKernel = 0;
    unsigned long long _cpuPrevUser = 0;
    bool _cpuPrimed = false;

    // Network
    unsigned long long _netPrevIn = 0;
    unsigned long long _netPrevOut = 0;
    unsigned long long _netPrevQpc = 0;
    bool _netPrimed = false;

    bool _nvmlPollLogged = false;
};

// --- autostart (HKCU\...\Run) ---------------------------------------------
bool IsAutostartEnabled();
bool SetAutostart(bool enabled, const std::wstring& command);
std::wstring BuildAutostartCommand(const std::wstring& modulePath);
