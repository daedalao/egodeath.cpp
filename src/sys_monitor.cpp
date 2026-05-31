#include "sys_monitor.hpp"
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <dirent.h>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <dlfcn.h>

namespace fs = std::filesystem;

// NVML structures and function pointers for NVIDIA GPU support loaded dynamically at runtime
typedef int nvmlReturn_t;
typedef struct nvmlDevice_st* nvmlDevice_t;
struct nvmlMemory_t {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};
struct nvmlUtilization_t {
    unsigned int gpu;
    unsigned int memory;
};

typedef nvmlReturn_t (*nvmlInit_t)();
typedef nvmlReturn_t (*nvmlShutdown_t)();
typedef nvmlReturn_t (*nvmlDeviceGetCount_t)(unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetHandleByIndex_t)(unsigned int, nvmlDevice_t*);
typedef nvmlReturn_t (*nvmlDeviceGetMemoryInfo_t)(nvmlDevice_t, nvmlMemory_t*);
typedef nvmlReturn_t (*nvmlDeviceGetUtilizationRates_t)(nvmlDevice_t, nvmlUtilization_t*);
typedef nvmlReturn_t (*nvmlDeviceGetName_t)(nvmlDevice_t, char*, unsigned int);

namespace egodeath {

// Helper function to lookup PCI device name in /usr/share/hwdata/pci.ids
static std::string lookup_pci_device(const std::string& vendor_id, const std::string& device_id) {
    std::ifstream pci_file("/usr/share/hwdata/pci.ids");
    if (!pci_file) {
        pci_file.open("/usr/share/misc/pci.ids");
    }
    if (!pci_file) return "";
    
    std::string line;
    std::string target_vendor = vendor_id;
    std::string target_device = device_id;
    std::transform(target_vendor.begin(), target_vendor.end(), target_vendor.begin(), ::tolower);
    std::transform(target_device.begin(), target_device.end(), target_device.begin(), ::tolower);
    
    bool in_vendor = false;
    while (std::getline(pci_file, line)) {
        if (line.empty() || line[0] == '#') continue;
        
        if (line[0] != '\t') {
            size_t space_pos = line.find("  ");
            if (space_pos != std::string::npos) {
                std::string v_id = line.substr(0, space_pos);
                std::transform(v_id.begin(), v_id.end(), v_id.begin(), ::tolower);
                if (v_id == target_vendor) {
                    in_vendor = true;
                } else {
                    in_vendor = false;
                }
            }
        } else if (in_vendor && line[1] != '\t') {
            std::string sub = line.substr(1); // strip tab
            size_t space_pos = sub.find("  ");
            if (space_pos != std::string::npos) {
                std::string d_id = sub.substr(0, space_pos);
                std::transform(d_id.begin(), d_id.end(), d_id.begin(), ::tolower);
                if (d_id == target_device) {
                    return sub.substr(space_pos + 2);
                }
            }
        }
    }
    return "";
}

static std::string get_amd_gpu_name(const std::string& base_path) {
    std::ifstream v_file(base_path + "vendor");
    std::ifstream d_file(base_path + "device");
    std::string vendor, device;
    if (v_file && d_file) {
        v_file >> vendor;
        d_file >> device;
        if (vendor.rfind("0x", 0) == 0 && vendor.length() > 2) vendor = vendor.substr(2);
        if (device.rfind("0x", 0) == 0 && device.length() > 2) device = device.substr(2);
        
        std::string lookup = lookup_pci_device(vendor, device);
        if (!lookup.empty()) return lookup;
    }
    return "AMD GPU";
}

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
    
    // 1. Try querying NVIDIA GPUs via NVML (loaded dynamically at runtime to avoid static linking issues)
    void* nvml_handle = dlopen("libnvidia-ml.so.1", RTLD_LAZY);
    if (!nvml_handle) {
        nvml_handle = dlopen("libnvidia-ml.so", RTLD_LAZY);
    }
    
    if (nvml_handle) {
        auto nvmlInit = (nvmlInit_t)dlsym(nvml_handle, "nvmlInit");
        auto nvmlShutdown = (nvmlShutdown_t)dlsym(nvml_handle, "nvmlShutdown");
        auto nvmlDeviceGetCount = (nvmlDeviceGetCount_t)dlsym(nvml_handle, "nvmlDeviceGetCount");
        auto nvmlDeviceGetHandleByIndex = (nvmlDeviceGetHandleByIndex_t)dlsym(nvml_handle, "nvmlDeviceGetHandleByIndex");
        auto nvmlDeviceGetMemoryInfo = (nvmlDeviceGetMemoryInfo_t)dlsym(nvml_handle, "nvmlDeviceGetMemoryInfo");
        auto nvmlDeviceGetUtilizationRates = (nvmlDeviceGetUtilizationRates_t)dlsym(nvml_handle, "nvmlDeviceGetUtilizationRates");
        auto nvmlDeviceGetName = (nvmlDeviceGetName_t)dlsym(nvml_handle, "nvmlDeviceGetName");
        
        if (nvmlInit && nvmlShutdown && nvmlDeviceGetCount && 
            nvmlDeviceGetHandleByIndex && nvmlDeviceGetMemoryInfo && nvmlDeviceGetUtilizationRates) {
            
            if (nvmlInit() == 0) { // NVML_SUCCESS is 0
                unsigned int device_count = 0;
                if (nvmlDeviceGetCount(&device_count) == 0 && device_count > 0) {
                    for (unsigned int i = 0; i < device_count; ++i) {
                        nvmlDevice_t dev;
                        if (nvmlDeviceGetHandleByIndex(i, &dev) == 0) {
                            GPUStats gs;
                            
                            // Get VRAM
                            nvmlMemory_t mem;
                            if (nvmlDeviceGetMemoryInfo(dev, &mem) == 0) {
                                gs.vram_total_gb = mem.total / 1024.0 / 1024.0 / 1024.0;
                                gs.vram_used_gb = mem.used / 1024.0 / 1024.0 / 1024.0;
                                gs.vram_percent = (100.0 * mem.used) / mem.total;
                            }
                            
                            // Get Util
                            nvmlUtilization_t util;
                            if (nvmlDeviceGetUtilizationRates(dev, &util) == 0) {
                                gs.busy_percent = util.gpu;
                            }
                            
                            // Get Name
                            char name_buf[96] = {0};
                            if (nvmlDeviceGetName && nvmlDeviceGetName(dev, name_buf, sizeof(name_buf)) == 0) {
                                gs.name = name_buf;
                            } else {
                                gs.name = "NVIDIA GPU";
                            }
                            
                            stats_.gpus.push_back(gs);
                        }
                    }
                    nvmlShutdown();
                    dlclose(nvml_handle);
                    return; // Successfully loaded and queried NVIDIA GPUs
                }
                nvmlShutdown();
            }
        }
        dlclose(nvml_handle);
    }
    
    // 2. Fallback: AMD GPU sysfs discovery
    std::vector<std::string> card_paths;
    if (fs::exists("/sys/class/drm")) {
        for (const auto& entry : fs::directory_iterator("/sys/class/drm")) {
            std::string name = entry.path().filename().string();
            // Match card followed only by digits (e.g., card0, card1)
            if (name.rfind("card", 0) == 0 && name.length() > 4) {
                bool digits_only = true;
                for (size_t i = 4; i < name.length(); ++i) {
                    if (!std::isdigit(name[i])) {
                        digits_only = false;
                        break;
                    }
                }
                if (digits_only) {
                    std::string total_path = entry.path().string() + "/device/mem_info_vram_total";
                    if (fs::exists(total_path)) {
                        card_paths.push_back(entry.path().string() + "/device/");
                    }
                }
            }
        }
    }
    
    // Sort to keep card ordering consistent
    std::sort(card_paths.begin(), card_paths.end());
    
    for (const auto& base : card_paths) {
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
        
        gs.name = get_amd_gpu_name(base);
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
