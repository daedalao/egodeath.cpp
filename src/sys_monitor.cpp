#include "sys_monitor.hpp"
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <dirent.h>
#include <cstring>
#include <algorithm>

namespace egodeath {

SysMonitor::SysMonitor() {
    _update_llama_pid();
}

void SysMonitor::_update_llama_pid() {
    DIR* dir = opendir("/proc");
    if (!dir) return;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] >= '0' && ent->d_name[0] <= '9') {
            std::string path = std::string("/proc/") + ent->d_name + "/comm";
            std::ifstream comm(path);
            std::string name;
            if (std::getline(comm, name) && (name == "llama-server" || name == "llama-cli")) {
                llama_pid_ = std::stoi(ent->d_name);
                closedir(dir);
                return;
            }
        }
    }
    llama_pid_ = -1;
    closedir(dir);
}

void SysMonitor::_update_ram() {
    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    long total = 0, avail = 0;
    while (std::getline(meminfo, line)) {
        if (line.compare(0, 8, "MemTotal") == 0) std::sscanf(line.c_str(), "MemTotal: %ld", &total);
        if (line.compare(0, 12, "MemAvailable") == 0) std::sscanf(line.c_str(), "MemAvailable: %ld", &avail);
    }
    stats_.ram_total_gb = total / 1024.0 / 1024.0;
    stats_.ram_used_gb = (total - avail) / 1024.0 / 1024.0;
    stats_.ram_percent = (total > 0) ? (100.0 * (total - avail) / total) : 0.0;
}

void SysMonitor::_update_llama_cpu() {
    if (llama_pid_ == -1 || kill(llama_pid_, 0) != 0) {
        _update_llama_pid();
        if (llama_pid_ == -1) {
            stats_.llama_cpu_percent = 0.0;
            return;
        }
    }
    
    std::ifstream stat("/proc/stat");
    std::string line;
    long user, nice, system, idle, iowait, irq, softirq, steal;
    std::getline(stat, line);
    std::sscanf(line.c_str(), "cpu  %ld %ld %ld %ld %ld %ld %ld %ld", 
              &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
    
    long total_time = user + nice + system + idle + iowait + irq + softirq + steal;
    
    std::ifstream pstat("/proc/" + std::to_string(llama_pid_) + "/stat");
    std::string ignore;
    long utime, stime;
    if (pstat) {
        for(int i=0; i<13; ++i) pstat >> ignore;
        pstat >> utime >> stime;
        long process_time = utime + stime;
        
        if (last_total_time_ > 0) {
            long delta_total = total_time - last_total_time_;
            long delta_proc = process_time - last_process_time_;
            if (delta_total > 0) {
                stats_.llama_cpu_percent = (100.0 * delta_proc) / delta_total;
            }
        }
        last_total_time_ = total_time;
        last_process_time_ = process_time;
    }
}

void SysMonitor::_update_gpus() {
    stats_.gpus.clear();
    // Check card1 and card2 (standard for the user's MI25x2 setup)
    for (int i = 1; i <= 2; ++i) {
        std::string base = "/sys/class/drm/card" + std::to_string(i) + "/device/";
        GPUStats gs;
        
        std::ifstream busy_file(base + "gpu_busy_percent");
        if (busy_file) busy_file >> gs.busy_percent;

        std::ifstream total_file(base + "mem_info_vram_total");
        long total = 0;
        if (total_file) total_file >> total;

        std::ifstream used_file(base + "mem_info_vram_used");
        long used = 0;
        if (used_file) used_file >> used;

        if (total > 0) {
            gs.vram_total_gb = total / 1024.0 / 1024.0 / 1024.0;
            gs.vram_used_gb = used / 1024.0 / 1024.0 / 1024.0;
            gs.vram_percent = (100.0 * used) / total;
        }
        stats_.gpus.push_back(gs);
    }
}

SystemStats SysMonitor::get_stats() {
    _update_ram();
    _update_llama_cpu();
    _update_gpus();
    return stats_;
}

} // namespace egodeath
