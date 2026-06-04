#include "shell_jobs.hpp"

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <ctime>
#include <cstring>

ShellJobs::~ShellJobs() { shutdown(); }

void ShellJobs::reap(Job* j, int readfd) {
    char buf[4096];
    ssize_t n;
    while ((n = read(readfd, buf, sizeof(buf))) > 0) {
        std::lock_guard<std::mutex> lk(j->m);
        j->output.append(buf, (size_t)n);
        if (j->output.size() > kCap)
            j->output.erase(0, j->output.size() - kCap);  // keep the tail
    }
    close(readfd);
    int status = 0;
    waitpid(j->pid, &status, 0);
    if (WIFEXITED(status)) j->exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) j->exit_code = 128 + WTERMSIG(status);
    j->running.store(false);
}

int ShellJobs::start(const std::string& cmd) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }
    if (pid == 0) {
        // New session so the whole job is its own process group (killable as -pid)
        // and can never grab the controlling terminal.
        setsid();
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        int dn = open("/dev/null", O_RDONLY);
        if (dn >= 0) { dup2(dn, STDIN_FILENO); close(dn); }
        close(pipefd[0]); close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
        _exit(127);
    }

    close(pipefd[1]);
    std::lock_guard<std::mutex> lk(map_m_);
    int id = next_id_++;
    auto j = std::make_unique<Job>();
    j->id = id;
    j->pid = pid;
    j->command = cmd;
    j->started = (long long)std::time(nullptr);
    Job* jp = j.get();
    int rfd = pipefd[0];
    jobs_[id] = std::move(j);
    jp->reader = std::thread([this, jp, rfd]() { reap(jp, rfd); });
    return id;
}

std::string ShellJobs::output(int id, size_t max_tail) {
    std::lock_guard<std::mutex> lk(map_m_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return "";
    Job* j = it->second.get();
    std::string body;
    {
        std::lock_guard<std::mutex> jl(j->m);
        body = j->output;
    }
    if (body.size() > max_tail) body = "...[" + std::to_string(body.size() - max_tail) + " earlier bytes]\n"
                                        + body.substr(body.size() - max_tail);
    std::string head = "[job " + std::to_string(j->id) + "] ";
    head += j->running.load() ? "running" : ("exited(" + std::to_string(j->exit_code) + ")");
    head += "  $ " + j->command + "\n";
    return head + (body.empty() ? "(no output yet)" : body);
}

bool ShellJobs::stop(int id) {
    std::lock_guard<std::mutex> lk(map_m_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return false;
    Job* j = it->second.get();
    if (j->running.load() && j->pid > 0) {
        kill(-j->pid, SIGTERM);  // negative: the whole process group
    }
    return true;
}

std::vector<ShellJobs::Summary> ShellJobs::list() {
    std::lock_guard<std::mutex> lk(map_m_);
    std::vector<Summary> out;
    for (auto& [id, j] : jobs_) {
        std::lock_guard<std::mutex> jl(j->m);
        out.push_back(Summary{j->id, j->command, j->running.load(), j->exit_code, j->output.size(), j->started});
    }
    return out;
}

void ShellJobs::shutdown() {
    std::lock_guard<std::mutex> lk(map_m_);
    for (auto& [id, j] : jobs_) {
        if (j->running.load() && j->pid > 0) {
            kill(-j->pid, SIGTERM);
        }
    }
    for (auto& [id, j] : jobs_) {
        if (j->reader.joinable()) {
            // Nudge with SIGKILL if it ignored SIGTERM, so the reader's read() ends.
            if (j->running.load() && j->pid > 0) kill(-j->pid, SIGKILL);
            j->reader.join();
        }
    }
    jobs_.clear();
}
