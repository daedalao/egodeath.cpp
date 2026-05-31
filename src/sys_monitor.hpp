#pragma once
#include <string>
#include <vector>
#include <chrono>
#include <csignal>

namespace egodeath {

struct GPUStats {
    int busy_percent = 0;
    double vram_total_gb = 0.0;
    double vram_used_gb = 0.0;
    double vram_percent = 0.0;
};

struct SystemStats {
    double ram_total_gb = 0.0;
    double ram_used_gb = 0.0;
    double ram_percent = 0.0;
    double llama_cpu_percent = 0.0;
    std::vector<GPUStats> gpus;
};

class SysMonitor {
public:
    SysMonitor();
    SystemStats get_stats();

private:
    void _update_llama_pid();
    void _update_ram();
    void _update_llama_cpu();
    void _update_gpus();
    
    SystemStats stats_;
    int llama_pid_ = -1;
    
    long last_total_time_ = 0;
    long last_process_time_ = 0;
};

} // namespace egodeath
