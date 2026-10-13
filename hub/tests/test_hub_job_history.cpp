// Unit tests for the hub's job history: what a finished job leaves on disk,
// what comes back in the next run, and what Clear history deletes.
#include "hub_job_history.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace hub;
namespace fs = std::filesystem;

namespace {

int failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

JobRecord record(const std::string& title, const std::string& state, long long finished) {
    JobRecord r;
    r.kind = "build";
    r.game_id = "game_" + std::to_string(finished);
    r.game_title = title;
    r.family_id = "gb";
    r.state = state;
    r.step = "step 3/3: compiling native executable";
    r.started_at = finished - 90;
    r.finished_at = finished;
    r.total_seconds = 90.5;
    return r;
}

size_t count_logs(const fs::path& dir) {
    size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".log") ++n;
    }
    return n;
}

void test_line_round_trip() {
    JobRecord r = record("Quotes \" and \\ and \xC3\xA9", "failed", 1700000000);
    r.error = "ninja: build stopped: subcommand failed.";
    r.result_path = "C:\\games\\a b\\game.exe";
    r.log_file = "job-1700000000-0.log";
    const std::string line = job_record_to_line(r);
    CHECK(line.find('\n') == std::string::npos);  // one job, one line
    JobRecord back;
    CHECK(job_record_from_line(line, back));
    CHECK(back.kind == r.kind && back.game_id == r.game_id && back.game_title == r.game_title);
    CHECK(back.family_id == r.family_id && back.state == r.state && back.step == r.step);
    CHECK(back.error == r.error && back.result_path == r.result_path && back.log_file == r.log_file);
    CHECK(back.started_at == r.started_at && back.finished_at == r.finished_at);
    CHECK(back.total_seconds == r.total_seconds);

    // Only finished jobs are history; a damaged line is not a job.
    JobRecord bad;
    CHECK(!job_record_from_line("", bad));
    CHECK(!job_record_from_line("not json", bad));
    CHECK(!job_record_from_line("[1, 2]", bad));
    r.state = "running";
    CHECK(!job_record_from_line(job_record_to_line(r), bad));
    r.state = "succeeded";
    r.kind.clear();
    CHECK(!job_record_from_line(job_record_to_line(r), bad));

    // A log name is a file in the history folder and nothing else: a record
    // that names a path loses its log rather than reaching outside.
    r = record("Escape", "succeeded", 5);
    for (const char* name : {"..\\..\\library.json", "../settings.json", "C:\\Windows\\win.ini", "..", "sub/job.log"}) {
        r.log_file = name;
        CHECK(job_record_from_line(job_record_to_line(r), back));
        CHECK(back.log_file.empty());
    }
}

void test_append_load_clear(const fs::path& root) {
    const fs::path dir = root / "job-history";
    JobHistory history(dir.string());
    CHECK(history.enabled());
    CHECK(history.load().empty());  // no folder yet: no jobs, no error

    std::string error;
    JobRecord a = record("First", "succeeded", 1000);
    CHECK(history.append(a, {"line one", "line two", ""}, error));
    CHECK(!a.log_file.empty());
    JobRecord b = record("Second", "failed", 1000);  // the same second as the first
    b.error = "compile error";
    CHECK(history.append(b, {"it broke"}, error));
    CHECK(!b.log_file.empty() && b.log_file != a.log_file);
    JobRecord c = record("Third", "cancelled", 1002);
    CHECK(history.append(c, {}, error));
    CHECK(c.log_file.empty());  // nothing logged, no file
    CHECK(count_logs(dir) == 2);

    // A second object, as the next run of the hub is.
    JobHistory again(dir.string());
    std::vector<JobRecord> all = again.load();
    CHECK(all.size() == 3);
    if (all.size() == 3) {
        CHECK(all[0].game_title == "First" && all[1].game_title == "Second" && all[2].game_title == "Third");
        CHECK(all[1].state == "failed" && all[1].error == "compile error");
        CHECK(all[2].state == "cancelled");
        const std::vector<std::string> log = again.load_log(all[0]);
        CHECK(log.size() == 3 && log[0] == "line one" && log[1] == "line two" && log[2].empty());
        CHECK(again.load_log(all[1]) == std::vector<std::string>{"it broke"});
        CHECK(again.load_log(all[2]).empty());
    }
    std::vector<JobRecord> newest = again.load(2);
    CHECK(newest.size() == 2 && newest[0].game_title == "Second" && newest[1].game_title == "Third");

    // A line cut short by a crash does not hide the jobs around it.
    {
        std::ofstream out(dir / "history.jsonl", std::ios::binary | std::ios::app);
        out << "{\"kind\": \"build\", \"state\": \"succ\n";
    }
    JobRecord d = record("Fourth", "succeeded", 1003);
    CHECK(history.append(d, {"after the damage"}, error));
    all = again.load();
    CHECK(all.size() == 4 && all.back().game_title == "Fourth");

    // Keeping some: the others' logs go, the kept ones' stay, order kept.
    {
        std::ofstream stray(dir / "notes.txt");
        stray << "not the hub's";
    }
    CHECK(again.rewrite({all[1], all[3]}, error));
    all = again.load();
    CHECK(all.size() == 2 && all[0].game_title == "Second" && all[1].game_title == "Fourth");
    CHECK(count_logs(dir) == 2);
    CHECK(again.load_log(all[0]) == std::vector<std::string>{"it broke"});
    CHECK(fs::exists(dir / "notes.txt"));

    // Clear history: the list and every log, and nothing that is not theirs.
    CHECK(again.rewrite({}, error));
    CHECK(again.load().empty());
    CHECK(count_logs(dir) == 0);
    CHECK(!fs::exists(dir / "history.jsonl"));
    CHECK(fs::exists(dir / "notes.txt"));
    fs::remove(dir / "notes.txt");

    // And jobs can be kept again afterwards, the folder gone or not.
    CHECK(again.rewrite({}, error));
    JobRecord e = record("Fifth", "succeeded", 2000);
    CHECK(history.append(e, {"again"}, error));
    CHECK(history.load().size() == 1);
    CHECK(history.rewrite({}, error));
    CHECK(!fs::exists(dir));  // empty once cleared, so removed
}

void test_trim(const fs::path& root) {
    const fs::path dir = root / "trim";
    JobHistory history(dir.string());
    std::string error;
    for (int i = 0; i < 10; ++i) {
        JobRecord r = record("Job " + std::to_string(i), "succeeded", 3000 + i);
        CHECK(history.append(r, {"log " + std::to_string(i)}, error));
    }
    CHECK(!history.trim(5));  // ten is not more than twice five: left alone
    CHECK(history.load().size() == 10 && count_logs(dir) == 10);
    JobRecord r = record("Job 10", "succeeded", 3010);
    CHECK(history.append(r, {"log 10"}, error));
    CHECK(history.trim(5));
    const std::vector<JobRecord> left = history.load();
    CHECK(left.size() == 5 && count_logs(dir) == 5);
    if (left.size() == 5) {
        CHECK(left.front().game_title == "Job 6" && left.back().game_title == "Job 10");
        CHECK(history.load_log(left.back()) == std::vector<std::string>{"log 10"});
    }
}

void test_disabled() {
    JobHistory none;
    CHECK(!none.enabled());
    std::string error;
    JobRecord r = record("Nowhere", "succeeded", 1);
    CHECK(none.append(r, {"x"}, error));  // nothing kept, nothing wrong
    CHECK(none.load().empty());
    CHECK(none.rewrite({}, error));
    CHECK(!none.trim(1));
}

}  // namespace

int main() {
    std::error_code ec;
    const fs::path root = fs::temp_directory_path() / "native_console_hub_job_history_test";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    test_line_round_trip();
    test_append_load_clear(root);
    test_trim(root);
    test_disabled();
    fs::remove_all(root, ec);
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("hub job history: ok\n");
    return 0;
}
