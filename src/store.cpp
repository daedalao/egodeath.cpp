#include "store.hpp"

#include <sqlite3.h>
#include <ctime>
#include <fmt/format.h>

namespace egodeath {

static std::string now_iso() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

static std::string col_text(sqlite3_stmt* st, int i) {
    const unsigned char* p = sqlite3_column_text(st, i);
    return p ? reinterpret_cast<const char*>(p) : std::string();
}

json Item::to_json() const {
    json j = {{"id", id}, {"kind", kind}, {"title", title}, {"status", status}};
    if (!notes.empty())    j["notes"] = notes;
    if (!priority.empty()) j["priority"] = priority;
    if (!due.empty())      j["due"] = due;
    if (!start_ts.empty()) j["start"] = start_ts;
    if (!end_ts.empty())   j["end"] = end_ts;
    j["scope"] = project.empty() ? "global" : "project";
    return j;
}

std::string Item::one_line() const {
    std::string box = status == "done" ? "[x]" : status == "cancelled" ? "[-]" : "[ ]";
    std::string pri = priority == "high" ? " !hi" : priority == "med" ? " ~md" : priority == "low" ? " .lo" : "";
    std::string w = when();
    std::string tail;
    if (kind == "event") tail = w.empty() ? "" : "  @" + w;
    else                 tail = w.empty() ? "" : "  due " + w;
    return fmt::format("#{} {} {}{}{}", id, box, title, pri, tail);
}

Store::Store(std::filesystem::path db_path) {
    std::error_code ec;
    std::filesystem::create_directories(db_path.parent_path(), ec);
    if (sqlite3_open(db_path.string().c_str(), &db_) != SQLITE_OK) {
        last_error_ = db_ ? sqlite3_errmsg(db_) : "cannot open database";
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return;
    }
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA busy_timeout=3000;");
    exec("CREATE TABLE IF NOT EXISTS items ("
         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "kind TEXT NOT NULL DEFAULT 'task',"
         "title TEXT NOT NULL,"
         "notes TEXT NOT NULL DEFAULT '',"
         "status TEXT NOT NULL DEFAULT 'open',"
         "priority TEXT NOT NULL DEFAULT '',"
         "due TEXT NOT NULL DEFAULT '',"
         "start_ts TEXT NOT NULL DEFAULT '',"
         "end_ts TEXT NOT NULL DEFAULT '',"
         "created_ts TEXT NOT NULL DEFAULT '',"
         "updated_ts TEXT NOT NULL DEFAULT ''"
         ");");
    exec("ALTER TABLE items ADD COLUMN project TEXT NOT NULL DEFAULT '';");  // ignored if present
    last_error_.clear();
    exec("CREATE INDEX IF NOT EXISTS idx_items_when ON items(status, kind, due, start_ts);");
}

Store::~Store() { if (db_) sqlite3_close(db_); }

void Store::exec(const char* sql) {
    if (!db_) return;
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK && err) {
        last_error_ = err;
        sqlite3_free(err);
    }
}

long long Store::add(const Item& it, std::string& err) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) { err = "store unavailable"; return -1; }
    if (it.title.empty()) { err = "title is required"; return -1; }
    const char* sql = "INSERT INTO items"
        "(kind,title,notes,status,priority,due,start_ts,end_ts,created_ts,updated_ts,project)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?);";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) { err = sqlite3_errmsg(db_); return -1; }
    std::string now = now_iso();
    std::string kind = it.kind == "event" ? "event" : "task";
    std::string status = it.status.empty() ? "open" : it.status;
    auto T = [&](int i, const std::string& v) { sqlite3_bind_text(st, i, v.c_str(), -1, SQLITE_TRANSIENT); };
    T(1, kind); T(2, it.title); T(3, it.notes); T(4, status); T(5, it.priority);
    T(6, it.due); T(7, it.start_ts); T(8, it.end_ts); T(9, now); T(10, now); T(11, it.project);
    if (sqlite3_step(st) != SQLITE_DONE) { err = sqlite3_errmsg(db_); sqlite3_finalize(st); return -1; }
    sqlite3_finalize(st);
    return sqlite3_last_insert_rowid(db_);
}

bool Store::set_status(long long id, const std::string& status, std::string& err) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) { err = "store unavailable"; return false; }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE items SET status=?, updated_ts=? WHERE id=?;", -1, &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_); return false;
    }
    std::string now = now_iso();
    sqlite3_bind_text(st, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    int changed = sqlite3_changes(db_);
    sqlite3_finalize(st);
    if (!ok) { err = sqlite3_errmsg(db_); return false; }
    if (changed == 0) { err = "no item with id " + std::to_string(id); return false; }
    return true;
}

bool Store::update(long long id, const json& fields, std::string& err) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) { err = "store unavailable"; return false; }
    static const std::vector<std::string> cols =
        {"title", "notes", "priority", "due", "start_ts", "end_ts", "kind", "status"};
    std::string sql = "UPDATE items SET updated_ts=?";
    std::vector<std::string> vals;
    for (const auto& c : cols) {
        std::string key = c == "start_ts" ? "start" : c == "end_ts" ? "end" : c;
        if (fields.contains(key) && fields[key].is_string()) {
            sql += ", " + c + "=?";
            vals.push_back(fields[key].get<std::string>());
        }
    }
    sql += " WHERE id=?;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) { err = sqlite3_errmsg(db_); return false; }
    std::string now = now_iso();
    int bi = 1;
    sqlite3_bind_text(st, bi++, now.c_str(), -1, SQLITE_TRANSIENT);
    for (const auto& v : vals) sqlite3_bind_text(st, bi++, v.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, bi, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    int changed = sqlite3_changes(db_);
    sqlite3_finalize(st);
    if (!ok) { err = sqlite3_errmsg(db_); return false; }
    if (changed == 0) { err = "no item with id " + std::to_string(id); return false; }
    return true;
}

bool Store::remove(long long id, std::string& err) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) { err = "store unavailable"; return false; }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM items WHERE id=?;", -1, &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_); return false;
    }
    sqlite3_bind_int64(st, 1, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    int changed = sqlite3_changes(db_);
    sqlite3_finalize(st);
    if (!ok) { err = sqlite3_errmsg(db_); return false; }
    if (changed == 0) { err = "no item with id " + std::to_string(id); return false; }
    return true;
}

std::vector<Item> Store::list(const std::string& kind, const std::string& status,
                              const std::string& from, const std::string& to, int limit,
                              const std::vector<std::string>& projects) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<Item> out;
    if (!db_) return out;
    // The chronological anchor: event start, else task due.
    const std::string whencol = "(CASE WHEN kind='event' THEN start_ts ELSE due END)";
    std::string sql = "SELECT id,kind,title,notes,status,priority,due,start_ts,end_ts,created_ts,updated_ts,project"
                      " FROM items WHERE 1=1";
    std::vector<std::string> binds;
    if (!kind.empty() && kind != "all")     { sql += " AND kind=?";   binds.push_back(kind); }
    if (!status.empty() && status != "all") { sql += " AND status=?"; binds.push_back(status); }
    if (!from.empty()) { sql += " AND " + whencol + " <> '' AND " + whencol + " >= ?"; binds.push_back(from); }
    if (!to.empty())   { sql += " AND " + whencol + " <> '' AND " + whencol + " <= ?"; binds.push_back(to); }
    if (!projects.empty()) {
        sql += " AND project IN (";
        for (size_t i = 0; i < projects.size(); ++i) { sql += (i ? ",?" : "?"); binds.push_back(projects[i]); }
        sql += ")";
    }
    sql += " ORDER BY (" + whencol + " = '') ASC, " + whencol + " ASC,"
           " CASE priority WHEN 'high' THEN 0 WHEN 'med' THEN 1 WHEN 'low' THEN 2 ELSE 3 END, id ASC";
    if (limit > 0) sql += " LIMIT " + std::to_string(limit);
    sql += ";";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
    for (size_t i = 0; i < binds.size(); ++i)
        sqlite3_bind_text(st, (int)i + 1, binds[i].c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        Item it;
        it.id = sqlite3_column_int64(st, 0);
        it.kind = col_text(st, 1); it.title = col_text(st, 2); it.notes = col_text(st, 3);
        it.status = col_text(st, 4); it.priority = col_text(st, 5); it.due = col_text(st, 6);
        it.start_ts = col_text(st, 7); it.end_ts = col_text(st, 8);
        it.created_ts = col_text(st, 9); it.updated_ts = col_text(st, 10); it.project = col_text(st, 11);
        out.push_back(std::move(it));
    }
    sqlite3_finalize(st);
    return out;
}

std::optional<Item> Store::get(long long id) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!db_) return std::nullopt;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
        "SELECT id,kind,title,notes,status,priority,due,start_ts,end_ts,created_ts,updated_ts,project"
        " FROM items WHERE id=?;", -1, &st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(st, 1, id);
    std::optional<Item> res;
    if (sqlite3_step(st) == SQLITE_ROW) {
        Item it;
        it.id = sqlite3_column_int64(st, 0);
        it.kind = col_text(st, 1); it.title = col_text(st, 2); it.notes = col_text(st, 3);
        it.status = col_text(st, 4); it.priority = col_text(st, 5); it.due = col_text(st, 6);
        it.start_ts = col_text(st, 7); it.end_ts = col_text(st, 8);
        it.created_ts = col_text(st, 9); it.updated_ts = col_text(st, 10); it.project = col_text(st, 11);
        res = std::move(it);
    }
    sqlite3_finalize(st);
    return res;
}

} // namespace egodeath
