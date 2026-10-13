// The jobs that have finished, kept on disk so the Jobs view still lists them
// after the hub is closed and opened again.
//
// One folder: history.jsonl has a line for each finished job, oldest first,
// and each job's log is a text file of its own beside it, read only when the
// job is looked at. A line is appended when a job finishes, so a hub window
// and a --batch run that finish jobs at the same time both keep theirs.
// Nothing here is needed to build or run a game: deleting the folder only
// forgets the list.
#pragma once

#include <string>
#include <vector>

namespace hub {

struct JobRecord {
    std::string kind;   // "build", "cover", "export", "profile"
    std::string game_id;
    std::string game_title;
    std::string family_id;
    std::string state;  // "succeeded", "failed", "cancelled"
    std::string step;   // the step it ended in
    std::string error;
    std::string result_path;
    long long started_at = 0;   // Unix seconds; 0 when it never started
    long long finished_at = 0;
    double total_seconds = 0.0;
    std::string log_file;       // a file name in the history folder, "" when no log was kept
};

class JobHistory {
public:
    JobHistory() = default;
    explicit JobHistory(std::string dir) : dir_(std::move(dir)) {}

    bool enabled() const { return !dir_.empty(); }
    const std::string& dir() const { return dir_; }

    // Writes the log to a new file, names it in the record, and appends the
    // record. A job with an empty log gets no file.
    bool append(JobRecord& record, const std::vector<std::string>& log, std::string& error) const;

    // The newest `limit` records, oldest first (0: all of them). Lines that
    // do not parse are skipped.
    std::vector<JobRecord> load(size_t limit = 0) const;

    // A record's log, empty when it has none or the file is gone.
    std::vector<std::string> load_log(const JobRecord& record) const;

    // Keeps exactly these records, in this order, and deletes every other
    // job's log. With none to keep, the list and all logs are deleted.
    bool rewrite(const std::vector<JobRecord>& keep, std::string& error) const;

    // When more than twice `limit` records are kept, drops all but the newest
    // `limit` of them and their logs. Returns whether anything was dropped.
    bool trim(size_t limit) const;

private:
    std::string dir_;
};

std::string job_record_to_line(const JobRecord& record);
bool job_record_from_line(const std::string& line, JobRecord& record);

}  // namespace hub
