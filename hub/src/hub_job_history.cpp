#include "hub_job_history.h"

#include "hub_json.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <system_error>

namespace fs = std::filesystem;

namespace hub {

namespace {

const char* const kListName = "history.jsonl";

fs::path to_path(const std::string& utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.c_str()), utf8.size()));
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        start = end + 1;
    }
    return lines;
}

// A log's name is the only part of a record that names a file: it must be a
// plain name in the history folder, never a path out of it.
bool plain_file_name(const std::string& name) {
    return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\:") == std::string::npos;
}

}  // namespace

std::string job_record_to_line(const JobRecord& record) {
    Json j = Json::make_object();
    j.set("kind", record.kind);
    j.set("game_id", record.game_id);
    j.set("game_title", record.game_title);
    j.set("family_id", record.family_id);
    j.set("state", record.state);
    j.set("step", record.step);
    j.set("error", record.error);
    j.set("result_path", record.result_path);
    j.set("started_at", record.started_at);
    j.set("finished_at", record.finished_at);
    j.set("total_seconds", record.total_seconds);
    j.set("log_file", record.log_file);
    return j.dump(0);
}

bool job_record_from_line(const std::string& line, JobRecord& record) {
    Json j;
    std::string error;
    if (line.empty() || !Json::parse(line, j, error) || !j.is_object()) return false;
    record = JobRecord{};
    record.kind = j.get_string("kind");
    record.game_id = j.get_string("game_id");
    record.game_title = j.get_string("game_title");
    record.family_id = j.get_string("family_id");
    record.state = j.get_string("state");
    record.step = j.get_string("step");
    record.error = j.get_string("error");
    record.result_path = j.get_string("result_path");
    record.started_at = j.get_int("started_at");
    record.finished_at = j.get_int("finished_at");
    record.total_seconds = j.get_number("total_seconds");
    record.log_file = j.get_string("log_file");
    if (!plain_file_name(record.log_file)) record.log_file.clear();
    // A job that is not finished has no place in the list.
    return !record.kind.empty() &&
           (record.state == "succeeded" || record.state == "failed" || record.state == "cancelled");
}

bool JobHistory::append(JobRecord& record, const std::vector<std::string>& log, std::string& error) const {
    if (!enabled()) return true;
    std::error_code ec;
    const fs::path dir = to_path(dir_);
    fs::create_directories(dir, ec);
    if (ec) {
        error = "cannot create " + dir_ + ": " + ec.message();
        return false;
    }
    record.log_file.clear();
    if (!log.empty()) {
        // Named for when the job finished; the number tells apart two that
        // finished in the same second.
        std::string name;
        for (int n = 0; n < 10000; ++n) {
            name = "job-" + std::to_string(record.finished_at) + "-" + std::to_string(n) + ".log";
            if (!fs::exists(dir / name, ec)) break;
        }
        std::ofstream out(dir / name, std::ios::binary | std::ios::trunc);
        for (const auto& line : log) out << line << '\n';
        out.flush();
        if (out) record.log_file = name;
    }
    std::ofstream list(dir / kListName, std::ios::binary | std::ios::app);
    list << job_record_to_line(record) << '\n';
    list.flush();
    if (!list) {
        error = "cannot write " + dir_ + "/" + kListName;
        return false;
    }
    return true;
}

std::vector<JobRecord> JobHistory::load(size_t limit) const {
    std::vector<JobRecord> records;
    if (!enabled()) return records;
    std::string text;
    if (!read_text_file(dir_ + "/" + kListName, text)) return records;
    for (const auto& line : split_lines(text)) {
        JobRecord record;
        if (job_record_from_line(line, record)) records.push_back(std::move(record));
    }
    if (limit && records.size() > limit) records.erase(records.begin(), records.end() - static_cast<std::ptrdiff_t>(limit));
    return records;
}

std::vector<std::string> JobHistory::load_log(const JobRecord& record) const {
    std::string text;
    if (!enabled() || !plain_file_name(record.log_file) || !read_text_file(dir_ + "/" + record.log_file, text)) return {};
    return split_lines(text);
}

bool JobHistory::rewrite(const std::vector<JobRecord>& keep, std::string& error) const {
    if (!enabled()) return true;
    std::error_code ec;
    const fs::path dir = to_path(dir_);
    if (!fs::is_directory(dir, ec)) return true;
    std::set<std::string> kept_logs;
    std::string text;
    for (const auto& record : keep) {
        if (plain_file_name(record.log_file)) kept_logs.insert(record.log_file);
        text += job_record_to_line(record) + "\n";
    }
    // Only this folder's own files: the list and the logs it names
    // (job-*.log). Anything else somebody put here is left alone.
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::u8string u8 = entry.path().filename().u8string();
        const std::string name(reinterpret_cast<const char*>(u8.c_str()), u8.size());
        const bool is_log = name.size() > 8 && name.compare(0, 4, "job-") == 0 && name.compare(name.size() - 4, 4, ".log") == 0;
        if (is_log && !kept_logs.count(name)) fs::remove(entry.path(), ec);
    }
    if (keep.empty()) {
        fs::remove(dir / kListName, ec);
        fs::remove(dir, ec);  // only when nothing else is in it
        return true;
    }
    return write_text_file_atomic(dir_ + "/" + kListName, text, error);
}

bool JobHistory::trim(size_t limit) const {
    if (!enabled() || !limit) return false;
    std::vector<JobRecord> all = load(0);
    if (all.size() <= limit * 2) return false;
    all.erase(all.begin(), all.end() - static_cast<std::ptrdiff_t>(limit));
    std::string error;
    return rewrite(all, error);
}

}  // namespace hub
