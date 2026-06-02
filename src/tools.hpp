#pragma once
#include "common.hpp"

namespace egodeath {

class Tools {
public:
    Tools(std::filesystem::path root);
    json schema();
    std::string dispatch(const std::string& name, const json& args);

    static std::string read_file(const std::filesystem::path& p);
    static std::string write_file(const std::filesystem::path& p, const std::string& c);
    std::string grep_search(const std::string& pat);
    static std::string exec_shell(const std::string& cmd);
    static std::string list_directory(const std::filesystem::path& p);
    std::string glob_search(const std::string& pattern);

private:
    std::filesystem::path root_;
    std::set<std::string> ignored_patterns_;
};

} // namespace egodeath
