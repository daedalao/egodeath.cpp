#pragma once
#include "common.hpp"

namespace egodeath {

class Tools {
public:
    Tools(std::filesystem::path root);
    json schema();
    std::string dispatch(const std::string& name, const json& args);

    static std::string read_file(const std::filesystem::path& p, int offset = 0, int limit = 0);
    static std::string write_file(const std::filesystem::path& p, const std::string& c);
    static std::string edit_file(const std::filesystem::path& p, const std::string& old_s,
                                 const std::string& new_s, bool replace_all);
    static std::string make_diff(const std::string& before, const std::string& after);
    std::string preview(const std::string& name, const json& args);
    std::string grep_search(const std::string& pat);
    static std::string exec_shell(const std::string& cmd);
    // Run `command` as root via sudo, feeding `password` on stdin (never argv/env/disk).
    static std::string exec_sudo(const std::string& command, const std::string& password, int& exit_code);
    static std::string list_directory(const std::filesystem::path& p);
    std::string glob_search(const std::string& pattern);

private:
    std::filesystem::path root_;
    std::set<std::string> ignored_patterns_;
};

} // namespace egodeath
