#include "hub_pipeline.h"

#include "hub_consoles.h"
#include "hub_export.h"
#include "hub_platform.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <system_error>

namespace hub {

std::string Job::step_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex);
    return step;
}

std::vector<std::string> Job::log_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex);
    return log;
}

std::string Job::error_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex);
    return error;
}

std::string Job::result_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex);
    return result_path;
}

std::vector<Job::StepTime> Job::step_times_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex);
    return step_times;
}

namespace {

double seconds_since(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

// Appends under the lock the caller already holds.
void push_log(std::vector<std::string>& log, const std::string& line) {
    log.push_back(line);
    if (log.size() > 20000) log.erase(log.begin(), log.begin() + 5000);
}

// The traces in <game_dir>/profile/, which every build reads.
std::set<std::string> profile_files(const std::string& game_dir) {
    std::set<std::string> files;
    const std::string dir = platform::join(game_dir, "profile");
    if (platform::is_directory(dir)) {
        for (const auto& trace : platform::list_files_recursive(dir, {".trace"})) files.insert(trace);
    }
    return files;
}

bool new_profile_file(const std::string& game_dir, const std::set<std::string>& before) {
    for (const auto& file : profile_files(game_dir)) {
        if (!before.count(file)) return true;
    }
    return false;
}

}  // namespace

std::string format_duration(double seconds) {
    if (seconds < 0) seconds = 0;
    char text[32];
    if (seconds < 60.0) {
        std::snprintf(text, sizeof(text), "%.1f s", seconds);
    } else if (seconds < 3600.0) {
        const long long whole = static_cast<long long>(seconds + 0.5);
        std::snprintf(text, sizeof(text), "%lldm %02llds", whole / 60, whole % 60);
    } else {
        const long long whole = static_cast<long long>(seconds + 0.5);
        std::snprintf(text, sizeof(text), "%lldh %02lldm", whole / 3600, (whole / 60) % 60);
    }
    return text;
}

double Job::step_elapsed_seconds() const {
    std::lock_guard<std::mutex> lock(mutex);
    return step.empty() ? 0.0 : seconds_since(step_started);
}

double Job::total_elapsed_seconds() const {
    if (state == JobState::Running) {
        std::lock_guard<std::mutex> lock(mutex);
        return seconds_since(job_started);
    }
    return total_seconds;
}

void Job::load_history_log() {
    if (!log_on_disk.exchange(false)) return;
    std::vector<std::string> lines = JobHistory(history_dir).load_log(record);
    std::lock_guard<std::mutex> lock(mutex);
    if (lines.empty()) lines.push_back("[hub] this job's log is no longer on disk");
    log = std::move(lines);
}

const char* job_kind_key(JobKind kind) {
    switch (kind) {
        case JobKind::Build: return "build";
        case JobKind::CaptureCover: return "cover";
        case JobKind::Export: return "export";
        case JobKind::Profile: return "profile";
        case JobKind::DataTree: return "tree";
    }
    return "build";
}

const char* job_state_key(JobState state) {
    switch (state) {
        case JobState::Queued: return "queued";
        case JobState::Running: return "running";
        case JobState::Succeeded: return "succeeded";
        case JobState::Failed: return "failed";
        case JobState::Cancelled: return "cancelled";
    }
    return "failed";
}

void Job::append_log(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex);
    push_log(log, line);
}

void Job::start_timing() {
    std::lock_guard<std::mutex> lock(mutex);
    job_started = step_started = std::chrono::steady_clock::now();
    step_times.clear();
    step.clear();
}

void Job::set_step(const std::string& s) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto now = std::chrono::steady_clock::now();
    if (!step.empty() && step != "done") {
        const double took = std::chrono::duration<double>(now - step_started).count();
        step_times.push_back({step, took});
        push_log(log, "[hub] " + step + " took " + format_duration(took));
    }
    step = s;
    step_started = now;
}

std::vector<std::string> Job::finish_timing() {
    std::lock_guard<std::mutex> lock(mutex);
    const auto now = std::chrono::steady_clock::now();
    if (!step.empty() && step != "done") {
        const double took = std::chrono::duration<double>(now - step_started).count();
        step_times.push_back({step, took});
        push_log(log, "[hub] " + step + " took " + format_duration(took));
        step_started = now;
    }
    total_seconds = std::chrono::duration<double>(now - job_started).count();
    // One step has already said how long it took; a table would repeat it.
    if (step_times.size() < 2) return {};

    // A table, widest step name first, so the slow step stands out.
    size_t width = 5;
    for (const auto& t : step_times) width = std::max(width, t.step.size());
    std::vector<std::string> summary = {"[hub] time per step:"};
    for (const auto& t : step_times) {
        const int percent = total_seconds > 0 ? static_cast<int>(t.seconds * 100.0 / total_seconds + 0.5) : 0;
        char line[512];
        std::snprintf(line, sizeof(line), "[hub]   %-*s  %9s  %3d%%", static_cast<int>(width), t.step.c_str(),
                      format_duration(t.seconds).c_str(), percent);
        summary.push_back(line);
    }
    char line[512];
    std::snprintf(line, sizeof(line), "[hub]   %-*s  %9s", static_cast<int>(width), "total",
                  format_duration(total_seconds).c_str());
    summary.push_back(line);
    for (const auto& s : summary) push_log(log, s);
    return summary;
}

JobQueue::JobQueue() : worker_([this] { worker_loop(); }) {}

JobQueue::~JobQueue() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        for (auto& j : jobs_) j->cancel = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

std::shared_ptr<Job> JobQueue::enqueue_build(const std::string& game_id,
                                             const std::string& game_title,
                                             const std::string& family_id,
                                             const std::string& game_dir,
                                             const std::string& rom_path,
                                             const std::string& rom_original_path,
                                             const Settings& settings) {
    auto job = std::make_shared<Job>();
    job->kind = JobKind::Build;
    job->game_id = game_id;
    job->game_title = game_title;
    job->family_id = family_id;
    job->game_dir = game_dir;
    job->rom_path = rom_path;
    job->rom_original_path = rom_original_path;
    job->settings = settings;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job->id = next_id_++;
        jobs_.push_back(job);
    }
    cv_.notify_all();
    return job;
}

std::shared_ptr<Job> JobQueue::enqueue_cover_capture(const std::string& game_id,
                                                     const std::string& game_title,
                                                     const std::string& family_id,
                                                     const std::string& game_dir,
                                                     const std::string& exe_path,
                                                     const std::string& covers_dir,
                                                     int frame,
                                                     const Settings& settings) {
    auto job = std::make_shared<Job>();
    job->kind = JobKind::CaptureCover;
    job->game_id = game_id;
    job->game_title = game_title;
    job->family_id = family_id;
    job->game_dir = game_dir;
    job->exe_path = exe_path;
    job->covers_dir = covers_dir;
    job->capture_frame = frame;
    job->settings = settings;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job->id = next_id_++;
        jobs_.push_back(job);
    }
    cv_.notify_all();
    return job;
}

std::shared_ptr<Job> JobQueue::enqueue_profile(const std::string& game_id,
                                               const std::string& game_title,
                                               const std::string& family_id,
                                               const std::string& game_dir,
                                               const std::string& exe_path,
                                               const Settings& settings) {
    auto job = std::make_shared<Job>();
    job->kind = JobKind::Profile;
    job->game_id = game_id;
    job->game_title = game_title;
    job->family_id = family_id;
    job->game_dir = game_dir;
    job->exe_path = exe_path;
    job->settings = settings;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job->id = next_id_++;
        jobs_.push_back(job);
    }
    cv_.notify_all();
    return job;
}

std::shared_ptr<Job> JobQueue::enqueue_export(const std::string& game_id,
                                              const std::string& game_title,
                                              const std::string& family_id,
                                              const std::string& rom_path,
                                              const std::string& targets,
                                              const std::string& dest_dir,
                                              const Settings& settings) {
    auto job = std::make_shared<Job>();
    job->kind = JobKind::Export;
    job->game_id = game_id;
    job->game_title = game_title;
    job->family_id = family_id;
    job->rom_path = rom_path;
    job->export_targets = targets;
    job->export_dest = dest_dir;
    job->settings = settings;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job->id = next_id_++;
        jobs_.push_back(job);
    }
    cv_.notify_all();
    return job;
}

std::shared_ptr<Job> JobQueue::enqueue_data_tree(const std::string& game_id,
                                                 const std::string& game_title,
                                                 const std::string& family_id,
                                                 const std::string& game_dir,
                                                 const std::string& rom_path,
                                                 const std::string& tree_dir,
                                                 const edited_data::Tool& tool,
                                                 const Settings& settings) {
    auto job = std::make_shared<Job>();
    job->kind = JobKind::DataTree;
    job->game_id = game_id;
    job->game_title = game_title;
    job->family_id = family_id;
    job->game_dir = game_dir;
    job->rom_path = rom_path;
    job->tree_dir = tree_dir;
    job->data_tool = tool;
    job->settings = settings;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job->id = next_id_++;
        jobs_.push_back(job);
    }
    cv_.notify_all();
    return job;
}

std::vector<std::shared_ptr<Job>> JobQueue::jobs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_;
}

std::shared_ptr<Job> JobQueue::find(int id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& j : jobs_) {
        if (j->id == id) return j;
    }
    return nullptr;
}

void JobQueue::cancel(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& j : jobs_) {
        if (j->id == id) {
            j->cancel = true;
            // One the worker has not picked up ends here; one it is running
            // ends when its step notices (run_capture ends the step's
            // processes), and the worker records it then.
            if (j->state == JobState::Queued) {
                j->state = JobState::Cancelled;
                j->finished_at = platform::now_unix();
                j->append_log("[hub] cancelled before it started");
                record_finished(*j);
            }
        }
    }
}

void JobQueue::set_history_dir(const std::string& dir, bool load) {
    std::lock_guard<std::mutex> lock(mutex_);
    history_ = JobHistory(dir);
    if (!load) return;
    history_.trim(kHistoryShown);
    std::vector<std::shared_ptr<Job>> old;
    for (auto& record : history_.load(kHistoryShown)) {
        auto job = std::make_shared<Job>();
        job->kind = record.kind == "cover" ? JobKind::CaptureCover
                  : record.kind == "export" ? JobKind::Export
                  : record.kind == "profile" ? JobKind::Profile
                  : record.kind == "tree" ? JobKind::DataTree
                  : JobKind::Build;
        job->game_id = record.game_id;
        job->game_title = record.game_title;
        job->family_id = record.family_id;
        job->state = record.state == "succeeded" ? JobState::Succeeded
                   : record.state == "cancelled" ? JobState::Cancelled
                   : JobState::Failed;
        job->step = record.step;
        job->error = record.error;
        job->result_path = record.result_path;
        job->started_at = record.started_at;
        job->finished_at = record.finished_at;
        job->total_seconds = record.total_seconds;
        // The library was updated from it when it finished, in the run it
        // belongs to: it must not be applied again.
        job->consumed = true;
        job->recorded = true;
        job->history_dir = dir;
        job->log_on_disk = !record.log_file.empty();
        job->record = std::move(record);
        old.push_back(std::move(job));
    }
    // Before anything queued already, and numbered from 1 in the order they
    // finished; this run's jobs follow.
    for (auto& j : jobs_) j->id += static_cast<int>(old.size());
    for (size_t i = 0; i < old.size(); ++i) old[i]->id = static_cast<int>(i) + 1;
    next_id_ += static_cast<int>(old.size());
    jobs_.insert(jobs_.begin(), old.begin(), old.end());
}

// Under the queue's lock. A finished job goes into the history once.
void JobQueue::record_finished(Job& job) {
    if (!history_.enabled() || job.recorded) return;
    JobRecord record;
    record.kind = job_kind_key(job.kind);
    record.game_id = job.game_id;
    record.game_title = job.game_title;
    record.family_id = job.family_id;
    record.state = job_state_key(job.state);
    record.step = job.step_snapshot();
    record.error = job.error_snapshot();
    record.result_path = job.result_snapshot();
    record.started_at = job.started_at;
    record.finished_at = job.finished_at;
    record.total_seconds = job.total_seconds;
    std::string error;
    if (!history_.append(record, job.log_snapshot(), error)) {
        job.append_log("[hub] this job could not be added to the job history: " + error);
        return;
    }
    job.record = std::move(record);
    job.history_dir = history_.dir();
    job.recorded = true;
}

bool JobQueue::has_pending_for(const std::string& game_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& j : jobs_) {
        JobState s = j->state;
        if (j->kind != JobKind::Export && j->game_id == game_id && (s == JobState::Queued || s == JobState::Running)) return true;
    }
    return false;
}

bool JobQueue::busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& j : jobs_) {
        JobState s = j->state;
        if (s == JobState::Queued || s == JobState::Running) return true;
    }
    return false;
}

void JobQueue::clear_history() {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [](const std::shared_ptr<Job>& j) {
                    JobState s = j->state;
                    return j->consumed && (s == JobState::Succeeded || s == JobState::Failed || s == JobState::Cancelled);
                }),
                jobs_.end());
    // On disk too: what is left of the list is whatever finished a moment ago
    // and has not reached the library yet, which is usually nothing. Records
    // an earlier run left beyond the ones shown go with the rest.
    std::vector<JobRecord> keep;
    for (const auto& j : jobs_) {
        if (j->recorded) keep.push_back(j->record);
    }
    std::string error;
    history_.rewrite(keep, error);
}

void JobQueue::worker_loop() {
    while (true) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                if (stop_) return true;
                for (const auto& j : jobs_) {
                    if (j->state == JobState::Queued) return true;
                }
                return false;
            });
            if (stop_) return;
            for (const auto& j : jobs_) {
                if (j->state == JobState::Queued) {
                    job = j;
                    break;
                }
            }
        }
        if (!job) continue;
        {
            // Under the lock, so that a Cancel either finds the job queued and
            // ends it there, or finds it running and leaves it to its step.
            std::lock_guard<std::mutex> lock(mutex_);
            if (job->state != JobState::Queued) continue;
            if (job->cancel) {
                job->state = JobState::Cancelled;
                job->finished_at = platform::now_unix();
                record_finished(*job);
                continue;
            }
            job->started_at = platform::now_unix();
            job->start_timing();
            job->state = JobState::Running;
        }
        if (job->kind == JobKind::Build) run_build(*job);
        else if (job->kind == JobKind::Export) run_export(*job);
        else if (job->kind == JobKind::Profile) run_profile(*job);
        else if (job->kind == JobKind::DataTree) run_data_tree(*job);
        else run_cover_capture(*job);
        const std::vector<std::string> timing = job->finish_timing();
        job->finished_at = platform::now_unix();
        if (job->kind == JobKind::Build) {
            // After the script's own output, so build.log ends with where the time went.
            std::ofstream log_file(platform::to_path(platform::join(job->game_dir, "build.log")), std::ios::app);
            for (const auto& line : timing) log_file << line << '\n';
        }
        {
            // Unless Clear history took it out of the list in the moment
            // between its last step and here: then it is not to come back.
            std::lock_guard<std::mutex> lock(mutex_);
            if (std::find(jobs_.begin(), jobs_.end(), job) != jobs_.end()) record_finished(*job);
        }
    }
}

void JobQueue::run_build(Job& job) {
    auto fail = [&](const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(job.mutex);
            job.error = message;
        }
        job.append_log("[hub] FAILED: " + message);
        job.state = job.cancel ? JobState::Cancelled : JobState::Failed;
    };

    const ConsoleBackend* backend = backend_for_family(job.family_id);
    if (!backend) return fail("no backend for family '" + job.family_id + "'");
    if (!backend->supported()) return fail(backend->unsupported_reason());

    job.set_step("preparing");
    std::string err;
    if (!platform::ensure_directory(job.game_dir, err)) return fail(err);
    const std::string install_dir = platform::join(job.game_dir, "game");
    if (!platform::ensure_directory(install_dir, err)) return fail(err);

    // Only the latest build and its save data are kept. Rescue persistence files
    // from any previous build location, then wipe the old trees.
    auto rescue_from = [&](const std::string& dir) {
        if (!platform::is_directory(dir)) return;
        for (const auto& file : platform::list_files_recursive(dir, {})) {
            std::string name = platform::file_name(file);
            if (!backend->is_persistence_file(job.game_id, name)) continue;
            std::string dest = platform::join(install_dir, name);
            if (platform::exists(dest)) continue;  // the installed copy is newer
            std::string copy_err;
            if (platform::copy_file(file, dest, copy_err)) job.append_log("[hub] kept " + name);
            else job.append_log("[hub] warning: " + copy_err);
        }
    };
    const std::string legacy_src = platform::join(job.game_dir, "src");
    const std::string work_dir = platform::join(job.game_dir, "work");
    rescue_from(platform::join(legacy_src, "build"));
    rescue_from(work_dir);
    for (const std::string& stale : {legacy_src, work_dir}) {
        if (platform::is_directory(stale)) {
            job.append_log("[hub] removing previous build tree " + stale);
            std::string rm_err;
            if (!platform::remove_tree(stale, rm_err)) return fail(rm_err);
        }
    }

    BuildRequest request;
    request.game_id = job.game_id;
    request.game_dir = job.game_dir;
    request.rom_path = job.rom_path;
    request.rom_original_path = job.rom_original_path;
    request.settings = &job.settings;
    BuildPlan plan;
    if (!backend->prepare_build(request, plan, err)) return fail(err);

    job.append_log("[hub] build script: " + plan.script_path);
    std::ofstream log_file(platform::to_path(platform::join(job.game_dir, "build.log")), std::ios::trunc);

    job.set_step("starting the build script");
    platform::RunResult result = platform::run_capture(
        platform::script_command(plan.script_path), job.game_dir,
        [&](const std::string& line) {
            job.append_log(line);
            if (log_file) log_file << line << '\n';
            if (line.rfind("[hub] step", 0) == 0) job.set_step(line.substr(6));
        },
        &job.cancel);
    if (log_file) log_file.flush();

    if (job.cancel) return fail("cancelled");
    if (!result.started) return fail(result.error);
    if (result.exit_code != 0) return fail("build script exited with code " + std::to_string(result.exit_code) + " (see build.log)");
    if (!platform::exists(plan.expected_executable)) return fail("build finished but " + plan.expected_executable + " is missing");

    // Install the executable and its runtime libraries, then drop the scratch tree.
    job.set_step("installing");
    const std::string built_dir = platform::parent_path(plan.expected_executable);
    const std::string installed_exe = platform::join(install_dir, platform::file_name(plan.expected_executable));
    for (const auto& file : platform::list_files_recursive(built_dir, {})) {
        if (platform::parent_path(file) != built_dir) continue;
        std::string ext = platform::file_extension_lower(file);
        bool is_exe = file == plan.expected_executable;
        if (!is_exe && ext != ".dll" && ext != ".so" && ext != ".dylib") continue;
        if (!platform::copy_file(file, platform::join(install_dir, platform::file_name(file)), err)) return fail(err);
    }
    if (!platform::exists(installed_exe)) return fail("failed to install " + installed_exe);
    // Linked rather than copied where the volume allows: a GameCube disc is over
    // a gigabyte and the library already keeps it in rom/. A hard link is still a
    // whole file, so the game folder stays self-contained.
    for (const auto& [source, name] : backend->install_files(request)) {
        bool linked = false;
        if (!platform::link_or_copy_file(source, platform::join(install_dir, name), linked, err)) return fail(err);
        job.append_log("[hub] installed " + name + (linked ? " (a link to the library's copy)" : ""));
    }
    if (job.settings.compress_executables && platform::supports_transparent_compression()) {
        job.set_step("compressing");
        for (const auto& file : platform::list_files_recursive(install_dir, {})) {
            std::string ext = platform::file_extension_lower(file);
            if (ext != ".exe" && ext != ".dll") continue;
            long long before = platform::file_size(file);
            std::string c_err;
            if (!platform::compress_file_transparent(file, c_err)) {
                job.append_log("[hub] warning: could not compress " + platform::file_name(file) + ": " + c_err);
                continue;
            }
            long long after = platform::file_size_on_disk(file);
            job.append_log("[hub] compressed " + platform::file_name(file) + ": " + std::to_string(before / 1024) +
                           " KiB -> " + std::to_string(after / 1024) + " KiB on disk");
        }
    }
    if (!plan.work_dir.empty()) {
        std::string rm_err;
        if (!platform::remove_tree(plan.work_dir, rm_err)) job.append_log("[hub] warning: " + rm_err);
        else job.append_log("[hub] removed build scratch " + plan.work_dir);
    }

    {
        std::lock_guard<std::mutex> lock(job.mutex);
        job.result_path = installed_exe;
    }
    job.set_step("done");
    job.state = JobState::Succeeded;
}

void JobQueue::run_cover_capture(Job& job) {
    auto fail = [&](const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(job.mutex);
            job.error = message;
        }
        job.append_log("[hub] FAILED: " + message);
        job.state = job.cancel ? JobState::Cancelled : JobState::Failed;
    };

    const ConsoleBackend* backend = backend_for_family(job.family_id);
    if (!backend || !backend->supports_cover_capture()) return fail("cover capture is not supported for this console");
    if (!platform::exists(job.exe_path)) return fail("game executable is missing; build it first");

    std::string err;
    if (!platform::ensure_directory(job.covers_dir, err)) return fail(err);
    if (!platform::ensure_directory(platform::join(job.game_dir, "scratch-save"), err)) return fail(err);

    const std::string prefix = platform::join(job.covers_dir, job.game_id + "_capture");
    const int frame = backend->cover_capture_frame(job.capture_frame);
    std::vector<std::string> args = backend->cover_capture_args(job.game_dir, frame, prefix);
    std::string command = platform::quote_arg(job.exe_path);
    for (const auto& a : args) command += " " + platform::quote_arg(a);

    // The profile files there before the capture, to tell whether it made one.
    const std::set<std::string> profiles_before = profile_files(job.game_dir);

    job.set_step("running game headless to frame " + std::to_string(frame));
    job.append_log("[hub] " + command);
    platform::RunResult result = platform::run_capture(command, platform::parent_path(job.exe_path),
                                                       [&](const std::string& line) { job.append_log(line); },
                                                       &job.cancel);
    if (job.cancel) return fail("cancelled");
    if (!result.started) return fail(result.error);

    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "_%05d.ppm", frame);
    const std::string frame_path = prefix + suffix;
    if (!platform::exists(frame_path)) return fail("the game did not produce " + frame_path);

    platform::Image image;
    if (!platform::load_image(frame_path, image, err)) return fail(err);

    std::string final_path = platform::join(job.covers_dir, job.game_id + ".png");
    if (!platform::save_image_png(final_path, image, err)) {
        // No PNG encoder on this host: keep the PPM as the cover.
        final_path = platform::join(job.covers_dir, job.game_id + ".ppm");
        if (!platform::copy_file(frame_path, final_path, err)) return fail(err);
    }
    std::string ignore;
    platform::remove_tree(frame_path, ignore);

    {
        std::lock_guard<std::mutex> lock(job.mutex);
        job.result_path = final_path;
    }
    job.profile_created = new_profile_file(job.game_dir, profiles_before);
    job.set_step("done");
    job.state = JobState::Succeeded;
}

// A backend's profile runs (ConsoleBackend::profile_runs), after the game
// was rebuilt with its cover's trace: a run of a traced build interprets only
// what that trace missed, several times faster than the first build. A run
// that fails is logged and dropped; the job still succeeds.
void JobQueue::run_profile(Job& job) {
    auto fail = [&](const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(job.mutex);
            job.error = message;
        }
        job.append_log("[hub] FAILED: " + message);
        job.state = job.cancel ? JobState::Cancelled : JobState::Failed;
    };

    const ConsoleBackend* backend = backend_for_family(job.family_id);
    if (!backend) return fail("no backend for family '" + job.family_id + "'");
    if (!platform::exists(job.exe_path)) return fail("game executable is missing; build it first");

    const std::set<std::string> profiles_before = profile_files(job.game_dir);
    std::string ignore;
    int failed = 0;
    for (const auto& run : backend->profile_runs(job.game_dir)) {
        std::string run_err;
        platform::remove_tree(run.scratch, ignore);
        if (!platform::ensure_directory(platform::parent_path(run.scratch), run_err) ||
            (platform::exists(run.output) && !platform::copy_file(run.output, run.scratch, run_err))) {
            job.append_log("[hub] warning: skipped " + run.label + ": " + run_err);
            failed++;
            continue;
        }
        std::string run_command = platform::quote_arg(job.exe_path);
        for (const auto& a : run.args) run_command += " " + platform::quote_arg(a);
        job.set_step(run.label);
        job.append_log("[hub] " + run_command);
        platform::RunResult run_result = platform::run_capture(
            run_command, platform::parent_path(job.exe_path), [&](const std::string& line) { job.append_log(line); },
            &job.cancel);
        if (job.cancel) {
            platform::remove_tree(run.scratch, ignore);
            return fail("cancelled");
        }
        std::error_code ec;
        if (run_result.started && run_result.exit_code == 0 && platform::exists(run.scratch)) {
            std::filesystem::rename(platform::to_path(run.scratch), platform::to_path(run.output), ec);
            if (!ec) continue;
        }
        const std::string why = !run_result.started ? run_result.error
                                : run_result.exit_code != 0 ? "exit code " + std::to_string(run_result.exit_code)
                                : ec ? ec.message()
                                     : "no output";
        job.append_log("[hub] warning: " + run.label + " failed (" + why + "); the build keeps the profile it had");
        platform::remove_tree(run.scratch, ignore);
        failed++;
    }
    job.profile_created = new_profile_file(job.game_dir, profiles_before);
    if (failed) {
        std::lock_guard<std::mutex> lock(job.mutex);
        job.result_path = "";
        job.error = std::to_string(failed) + " profile run(s) failed; see the log";
    }
    job.set_step("done");
    job.state = JobState::Succeeded;
}

// A Game Boy game's ROM taken apart into files to edit (DATA_FILES.md): the
// recompiler describes the ROM with the options a build uses, and
// tools/gb_data.py splits it. Seconds, where a build is a minute.
void JobQueue::run_data_tree(Job& job) {
    auto fail = [&](const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(job.mutex);
            job.error = message;
        }
        job.append_log("[hub] FAILED: " + message);
        job.state = job.cancel ? JobState::Cancelled : JobState::Failed;
    };

    const ConsoleBackend* backend = backend_for_family(job.family_id);
    if (!backend || job.family_id != "gb") return fail("only a Game Boy game has a data tree");
    // The command a build recompiles the ROM with. It is given a folder to
    // write C to, as a build's is, and with --lift-export writes none.
    const std::string work_dir = platform::join(job.game_dir, "work");
    const auto steps = backend->generate_steps(job.settings, job.rom_path, job.game_id, platform::join(work_dir, "src"),
                                               platform::join(job.game_dir, "profile"));
    const std::vector<std::string> recompile = steps.empty() ? std::vector<std::string>{} : steps.front().argv;

    job.set_step("describing the ROM and splitting it into files");
    const edited_data::Runner run = [&](const std::vector<std::string>& argv, std::vector<std::string>& output) {
        std::string command;
        for (const auto& a : argv) command += (command.empty() ? "" : " ") + platform::quote_arg(a);
        job.append_log("[hub] " + command);
        const platform::RunResult result = platform::run_capture(
            command, "",
            [&](const std::string& line) {
                job.append_log(line);
                output.push_back(line);
            },
            &job.cancel);
        if (!result.started) {
            output.push_back(result.error);
            return -1;
        }
        return result.exit_code;
    };
    std::vector<std::string> output;
    const std::string error = edited_data::make_tree(recompile, job.rom_path, job.tree_dir, job.data_tool, run, output);
    if (job.cancel) return fail("cancelled");
    if (!error.empty()) return fail(error);
    {
        std::lock_guard<std::mutex> lock(job.mutex);
        job.result_path = job.tree_dir;
    }
    job.set_step("done");
    job.state = JobState::Succeeded;
}

void JobQueue::run_export(Job& job) {
    ExportRequest request;
    request.game_id = job.game_id;
    request.title = job.game_title;
    request.family_id = job.family_id;
    request.rom_path = job.rom_path;
    request.targets = parse_export_targets(job.export_targets);
    request.dest_dir = job.export_dest;
    // Scratch next to the library, kept after a failure for inspection. Short:
    // MSVC fails on object and PDB paths beyond 260 characters.
    request.work_dir = platform::join(platform::join(job.settings.data_dir, "exports"), job.game_id.substr(0, 16));

    ExportCallbacks callbacks;
    callbacks.log = [&](const std::string& line) { job.append_log(line); };
    callbacks.step = [&](const std::string& s) { job.set_step(s); };
    callbacks.cancel = &job.cancel;

    job.append_log("[export] " + job.game_title + " -> " + job.export_dest + " (" + job.export_targets + ")");
    std::vector<std::string> outputs;
    std::string error;
    const bool ok = hub::run_export(request, job.settings, callbacks, outputs, error);
    {
        std::lock_guard<std::mutex> lock(job.mutex);
        job.outputs = outputs;
        if (ok) job.result_path = job.export_dest;
        else job.error = error;
    }
    if (!ok) {
        job.append_log("[export] FAILED: " + error);
        job.state = job.cancel ? JobState::Cancelled : JobState::Failed;
        return;
    }
    job.set_step("done");
    job.state = JobState::Succeeded;
}

}  // namespace hub
