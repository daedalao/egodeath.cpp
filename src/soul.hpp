#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <mutex>

// The agent's "soul": its always-loaded core identity. Persisted as soul.md with
// `name:` (everyday name), `fullname:` (optional formal name) and `core:`
// frontmatter, followed by the body. The body may be split into `## section`
// blocks: the sections named in `core:` are injected into the system prompt in
// full every turn; the rest are listed by name and fetched on demand
// (recall_soul). A flat body with no `##` headers is one always-on block.
//
// {name} placeholders are replaced with the everyday name and {fullname} with the
// formal name (or the everyday name if no formal one is set), so a rename
// propagates everywhere. If the user gives a long/formal name, the everyday name
// can be a short handle derived from it.
class Soul {
public:
    bool load(std::filesystem::path file, const std::string& default_name, const std::string& default_body);

    std::string name() const;       // everyday name (short)
    std::string full_name() const;  // formal name (== name when none set)
    std::string body() const;
    std::string prompt() const;

    std::string section(const std::string& name) const;
    std::vector<std::string> section_names() const;

    void set_name(const std::string& n);        // everyday name
    void set_full_name(const std::string& f);   // formal name
    void set_body(const std::string& b);
    void reload();

    std::filesystem::path path() const;

    // Derive a short, friendly everyday name from a long/formal one, e.g.
    // "Sir James Edelbrock the Eighteenth" -> "James". Strips honorifics and
    // trailing ordinal/suffix words; returns the first real given name.
    static std::string derive_short(const std::string& full);

private:
    struct Section { std::string name, body; };

    std::filesystem::path file_;
    std::string name_ = "egodeath";
    std::string full_name_;                  // empty == same as name_
    std::string raw_body_;
    std::string preamble_;
    std::vector<Section> sections_;
    std::vector<std::string> core_{"identity"};

    mutable std::mutex mtx_;

    void parse(const std::string& content);
    void set_raw(const std::string& body);
    void save_locked() const;
    std::string subst(std::string s) const;
    bool is_core(const std::string& n) const;
};
