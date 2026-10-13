// Background job queue: builds, exports and cover captures off the UI thread.
#pragma once

#include "hub_edited_data.h"
#include "hub_job_history.h"
#include "hub_settings.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hub {

enum class JobKind { Build, CaptureCover, Export, Profile, DataTree };
enum class JobState { Queued, Running, Succeeded, Failed, Cancelled };

struct Job {
    int id = 0;
    JobKind kind = JobKind::Build;
    std::string game_id;
    std::string game_title;
    std::string family_id;
    std::string game_dir;
    std::string rom_path;
    std::string rom_original_path;  // Build: where the ROM was imported from
    std::string exe_path;      // CaptureCover input
    std::string covers_dir;    // CaptureCover output directory
    int capture_frame = 600;
    std::string export_targets;  // Export: "windows,linux,macos"
    std::string export_dest;     // Export: destination folder
    std::string tree_dir;        // DataTree: where the tree is made
    edited_data::Tool data_tool; // DataTree: Python and gb_data.py
    Settings settings;         // snapshot taken when the job was queued

    std::atomic<JobState> state{JobState::Queued};
    std::atomic<bool> cancel{false};

    mutable std::mutex mutex;
    std::string step;
    std::vector<std::string> log;
    std::string error;
    std::string result_path;   // executable, cover image, or export destination on success
    std::vector<std::string> outputs;  // Export: files written
    long long started_at = 0;
    long long finished_at = 0;

    bool consumed = false;     // UI thread only: library already updated from this result

    // Set once the job is in the history on disk (hub_job_history.h): when
    // it finished, or when it was read back from an earlier run of the hub.
    // A job read back has its log still on disk until load_history_log.
    bool recorded = false;             // under the queue's lock
    JobRecord record;                  // under the queue's lock
    std::string history_dir;           // where record.log_file is
    std::atomic<bool> log_on_disk{false};
    // Reads the log of a job from an earlier run, the first time it is asked
    // for. Does nothing for a job of this run, whose log is already here.
    void load_history_log();
    // CaptureCover and Profile: the job left a profile file
    // (<game_dir>/profile/*.trace) that was not there when it started, so a
    // rebuild would compile more of the game (hub_app queues one once the
    // game is not running).
    std::atomic<bool> profile_created{false};

    // How long each step took, in the order they ran. A step ends when the
    // next one starts (set_step) or the job finishes (finish_timing); each
    // ending is logged as "[hub] <step> took <time>".
    struct StepTime {
        std::string step;
        double seconds = 0.0;
    };
    std::vector<StepTime> step_times;
    std::chrono::steady_clock::time_point step_started;
    std::chrono::steady_clock::time_point job_started;
    double total_seconds = 0.0;  // set by finish_timing

    std::string step_snapshot() const;
    std::vector<std::string> log_snapshot() const;
    std::string error_snapshot() const;
    std::string result_snapshot() const;
    std::vector<StepTime> step_times_snapshot() const;
    double step_elapsed_seconds() const;   // the step in progress
    double total_elapsed_seconds() const;  // the whole job, finished or not
    void append_log(const std::string& line);
    void set_step(const std::string& s);
    void start_timing();
    // Closes the last step and logs every step's time and the total; returns
    // those summary lines so a caller can also write them to a file.
    std::vector<std::string> finish_timing();
};

// "0.4 s", "42.1 s", "7m 42s", "1h 03m".
std::string format_duration(double seconds);

// The names jobs are recorded under: "build", "cover", "export", "profile", "tree",
// and "queued", "running", "succeeded", "failed", "cancelled".
const char* job_kind_key(JobKind kind);
const char* job_state_key(JobState state);

class JobQueue {
public:
    JobQueue();
    ~JobQueue();

    std::shared_ptr<Job> enqueue_build(const std::string& game_id,
                                       const std::string& game_title,
                                       const std::string& family_id,
                                       const std::string& game_dir,
                                       const std::string& rom_path,
                                       const std::string& rom_original_path,
                                       const Settings& settings);
    std::shared_ptr<Job> enqueue_cover_capture(const std::string& game_id,
                                               const std::string& game_title,
                                               const std::string& family_id,
                                               const std::string& game_dir,
                                               const std::string& exe_path,
                                               const std::string& covers_dir,
                                               int frame,
                                               const Settings& settings);

    // Runs the backend's profile_runs against the built game (exe_path).
    std::shared_ptr<Job> enqueue_profile(const std::string& game_id,
                                         const std::string& game_title,
                                         const std::string& family_id,
                                         const std::string& game_dir,
                                         const std::string& exe_path,
                                         const Settings& settings);

    // Compiles a ROM for other computers into dest_dir (see hub_export.h).
    std::shared_ptr<Job> enqueue_export(const std::string& game_id,
                                        const std::string& game_title,
                                        const std::string& family_id,
                                        const std::string& rom_path,
                                        const std::string& targets,
                                        const std::string& dest_dir,
                                        const Settings& settings);

    // Takes a Game Boy game's ROM apart into a data tree in tree_dir
    // (hub_edited_data.h, make_tree), with the options a build of it uses.
    std::shared_ptr<Job> enqueue_data_tree(const std::string& game_id,
                                           const std::string& game_title,
                                           const std::string& family_id,
                                           const std::string& game_dir,
                                           const std::string& rom_path,
                                           const std::string& tree_dir,
                                           const edited_data::Tool& tool,
                                           const Settings& settings);

    std::vector<std::shared_ptr<Job>> jobs() const;
    std::shared_ptr<Job> find(int id) const;
    void cancel(int id);
    bool has_pending_for(const std::string& game_id) const;  // builds, cover captures and profiles
    bool busy() const;

    // From now on every job that finishes is added to the history in `dir`.
    // With `load`, the jobs earlier runs left there (the newest
    // kHistoryShown of them) are put in the list first, finished and
    // consumed, so the Jobs view shows them; a --batch run records without
    // loading, since it reports on the jobs it ran itself.
    static constexpr size_t kHistoryShown = 200;
    void set_history_dir(const std::string& dir, bool load);
    // Forgets every finished job the library has been updated from, in the
    // list and on disk, logs included. Jobs queued or running stay.
    void clear_history();

private:
    void worker_loop();
    void run_build(Job& job);
    void run_cover_capture(Job& job);
    void run_profile(Job& job);
    void run_export(Job& job);
    void run_data_tree(Job& job);
    void record_finished(Job& job);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::shared_ptr<Job>> jobs_;
    std::thread worker_;
    bool stop_ = false;
    int next_id_ = 1;
    JobHistory history_;
};

}  // namespace hub
