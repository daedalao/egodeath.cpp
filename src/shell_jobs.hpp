#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <sys/types.h>

// Runs shell commands in the background so long-lived processes (dev servers,
// watchers, builds, training runs) don't block a turn. Each job gets its own
// process group (setsid) and a reader thread that drains its combined
// stdout/stderr into a capped buffer; the agent polls output, lists, and stops
// jobs by id. Mirrors how a coding agent backgrounds `npm run dev` and tails it.
class ShellJobs {
public:
    struct Summary {
        int id;
        std::string command;
        bool running;
        int exit_code;
        size_t out_bytes;
        long long started;
    };

    ~ShellJobs();

    int start(const std::string& cmd);                  // returns job id, -1 on failure
    std::string output(int id, size_t max_tail = 8000); // status header + tail of output; "" if no job
    bool stop(int id);                                   // SIGTERM the process group
    std::vector<Summary> list();
    void shutdown();                                     // kill + join everything

private:
    struct Job {
        int id = 0;
        pid_t pid = -1;
        std::string command;
        std::string output;
        std::atomic<bool> running{true};
        int exit_code = 0;
        long long started = 0;
        std::thread reader;
        std::mutex m;          // guards output
    };

    static constexpr size_t kCap = 256 * 1024;  // keep at most this many bytes (tail)

    std::map<int, std::unique_ptr<Job>> jobs_;
    std::mutex map_m_;
    int next_id_ = 1;

    void reap(Job* j, int readfd);
};
