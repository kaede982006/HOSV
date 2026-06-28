#include "JobController.hpp"
#include <chrono>
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <curl/curl.h>

namespace {
void log_info(const std::string& message) {
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm buf;
    localtime_r(&now, &buf);
    char time_str[64];
    std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &buf);
    std::fprintf(stdout, "[%s] [INFO] %s\n", time_str, message.c_str());
    std::fflush(stdout);
}

void log_error(const std::string& message) {
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm buf;
    localtime_r(&now, &buf);
    char time_str[64];
    std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &buf);
    std::fprintf(stderr, "[%s] [ERROR] %s\n", time_str, message.c_str());
    std::fflush(stderr);
}

size_t write_file_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out_file = static_cast<std::ostream*>(userdata);
    const size_t bytes = size * nmemb;
    out_file->write(ptr, static_cast<std::streamsize>(bytes));
    return bytes;
}

struct DownloadProgressData {
    std::stop_token st;
    std::ostream* out_file;
};

int download_progress_callback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    auto* data = static_cast<DownloadProgressData*>(clientp);
    if (data->st.stop_requested()) {
        return 1;
    }
    return 0;
}

bool download_file(const std::string& url, const std::string& path, std::stop_token st, std::string& err_msg) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        err_msg = "Failed to initialize curl";
        return false;
    }

    std::ofstream out_file(path, std::ios::binary);
    if (!out_file) {
        err_msg = "Failed to open output file: " + path;
        curl_easy_cleanup(curl);
        return false;
    }

    DownloadProgressData progress_data{st, &out_file};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out_file);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, download_progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress_data);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_easy_cleanup(curl);
    out_file.close();

    if (res != CURLE_OK) {
        err_msg = std::string("Curl download failed: ") + curl_easy_strerror(res);
        std::error_code ec;
        fs::remove(path, ec);
        return false;
    }

    if (http_code < 200 || http_code >= 300) {
        err_msg = "HTTP download returned status code " + std::to_string(http_code);
        std::error_code ec;
        fs::remove(path, ec);
        return false;
    }

    return true;
}

std::string resolve_target_file_path(const std::string& input_path, const std::string& url, const std::string& task_id) {
    fs::path target(input_path.empty() ? "." : input_path);
    std::error_code ec;
    bool is_dir = fs::is_directory(target, ec) || input_path.empty() || input_path.back() == '/' || input_path.back() == '\\';
    if (is_dir) {
        std::string filename = "video_" + task_id + ".mp4";
        std::size_t slash = url.find_last_of('/');
        if (slash != std::string::npos && slash + 1 < url.size()) {
            std::string url_file = url.substr(slash + 1);
            std::size_t q = url_file.find('?');
            if (q != std::string::npos) {
                url_file = url_file.substr(0, q);
            }
            if (!url_file.empty()) {
                filename = url_file;
            }
        }
        target /= filename;
    }
    return target.string();
}

std::string task_summary(const seedance2::GenerationTask& task) {
    std::ostringstream out;
    out << "Task " << task.id << " is " << seedance2::to_string(task.status);
    if (!task.failed_reason.empty()) out << ". " << task.failed_reason;
    return out.str();
}

std::string output_summary(const seedance2::GenerationTask& task) {
    std::ostringstream out;
    if (!task.output_url.empty()) out << task.output_url;
    for (const auto& url : task.video_urls) {
        if (url != task.output_url) {
            if (out.tellp() > 0) out << "\n";
            out << url;
        }
    }
    return out.str();
}
}

JobController::JobController(std::shared_ptr<SeedanceService> service, std::shared_ptr<AppState> model, std::shared_ptr<std::recursive_mutex> mutex)
    : service_(service), model_(model), mutex_(mutex) {}

JobController::~JobController() {
    cancel();
}

JobPhase JobController::phase() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(*mutex_);
    return model_->phase;
}

void JobController::set_phase(JobPhase p, const std::string& msg) {
    std::lock_guard<std::recursive_mutex> lock(*mutex_);
    model_->phase = p;
    model_->status = msg;
    model_->frame_dirty = true;
    if (model_->request_redraw) {
        model_->request_redraw();
    }
}

void JobController::update_status(const std::string& status, const std::string& output) {
    std::lock_guard<std::recursive_mutex> lock(*mutex_);
    model_->status = status;
    if (!output.empty()) {
        model_->output = output;
    }
    model_->frame_dirty = true;
    if (model_->request_redraw) {
        model_->request_redraw();
    }
}

void JobController::mark_idle() {
    std::lock_guard<std::recursive_mutex> lock(*mutex_);
    model_->busy = false;
    model_->phase = JobPhase::Idle;
    model_->frame_dirty = true;
    if (model_->request_redraw) {
        model_->request_redraw();
    }
}

void JobController::set_cancelled() {
    set_phase(JobPhase::Cancelled, "Job was cancelled.");
}

void JobController::fail(const std::string& msg) {
    std::lock_guard<std::recursive_mutex> lock(*mutex_);
    model_->has_error = true;
    model_->phase = JobPhase::Failed;
    model_->status = msg;
    model_->frame_dirty = true;
    if (model_->request_redraw) {
        model_->request_redraw();
    }
}

void JobController::cancel() {
    if (model_->worker && model_->worker->joinable()) {
        set_phase(JobPhase::Cancelling, "Cancelling job...");
        model_->worker->request_stop();
        model_->worker->join();
        model_->worker.reset();
    }
}

void JobController::start_generate(SceneInput snapshot) {
    cancel();
    {
        std::lock_guard<std::recursive_mutex> lock(*mutex_);
        model_->busy = true;
        model_->has_error = false;
        model_->output.clear();
    }
    set_phase(JobPhase::Validating, "Validating inputs...");

    model_->worker = std::make_unique<std::jthread>([this, snapshot](std::stop_token st) {
        run_generate(snapshot, st);
    });
}

void JobController::start_lookup(SceneInput snapshot) {
    cancel();
    {
        std::lock_guard<std::recursive_mutex> lock(*mutex_);
        model_->busy = true;
        model_->has_error = false;
        model_->output.clear();
    }
    set_phase(JobPhase::Validating, "Validating task ID...");

    model_->worker = std::make_unique<std::jthread>([this, snapshot](std::stop_token st) {
        run_lookup(snapshot, st);
    });
}

void JobController::run_generate(SceneInput snapshot, std::stop_token st) {
    try {
        if (st.stop_requested()) { set_cancelled(); return; }

        set_phase(JobPhase::Submitting, "Submitting generation request...");
        auto task = service_->submit_text(snapshot);

        if (st.stop_requested()) { set_cancelled(); return; }

        set_phase(JobPhase::Polling, "Polling task status (" + task.id + ")...");

        auto complete = service_->wait(task.id, std::chrono::milliseconds(5000), st, [this](const seedance2::GenerationTask& t) {
            update_status(task_summary(t));
        });

        if (st.stop_requested()) { set_cancelled(); return; }

        if (complete.status == seedance2::TaskStatus::Succeeded) {
            std::string download_url = complete.output_url.empty() ? (complete.video_urls.empty() ? "" : complete.video_urls[0]) : complete.output_url;
            if (!download_url.empty()) {
                std::string target_file = resolve_target_file_path(snapshot.save_path, download_url, complete.id);
                set_phase(JobPhase::Downloading, "Downloading video to " + target_file + "...");

                std::string err;
                if (download_file(download_url, target_file, st, err)) {
                    set_phase(JobPhase::Succeeded, "Downloaded to " + target_file);
                    update_status("Downloaded to " + target_file, output_summary(complete));
                } else {
                    if (st.stop_requested()) { set_cancelled(); return; }
                    log_error("Download failed: " + err);
                    fail("Download failed: " + err);
                    update_status("Download failed: " + err, output_summary(complete));
                }
            } else {
                fail("Success, but video URL is empty.");
                update_status("Success, but video URL is empty.", output_summary(complete));
            }
        } else {
            fail(task_summary(complete));
            update_status(task_summary(complete), output_summary(complete));
        }
    } catch (const seedance2::ApiException& e) {
        if (st.stop_requested()) { set_cancelled(); return; }
        std::string status_msg = "Error: ApiException (status=" + std::to_string(e.http_status) + ", code=" + e.error.code + ")";
        fail(status_msg);
        update_status(status_msg, e.error.message);
    } catch (const std::exception& e) {
        if (st.stop_requested()) { set_cancelled(); return; }
        fail(std::string("Error: ") + e.what());
    }
    mark_idle();
}

void JobController::run_lookup(SceneInput snapshot, std::stop_token st) {
    try {
        if (st.stop_requested()) { set_cancelled(); return; }

        set_phase(JobPhase::Submitting, "Checking task status...");
        auto task = service_->lookup(snapshot.task_id_lookup);

        if (st.stop_requested()) { set_cancelled(); return; }

        if (task.status == seedance2::TaskStatus::Succeeded) {
            std::string download_url = task.output_url.empty() ? (task.video_urls.empty() ? "" : task.video_urls[0]) : task.output_url;
            if (!download_url.empty()) {
                std::string target_file = resolve_target_file_path(snapshot.save_path, download_url, task.id);
                set_phase(JobPhase::Downloading, "Downloading video to " + target_file + "...");

                std::string err;
                if (download_file(download_url, target_file, st, err)) {
                    set_phase(JobPhase::Succeeded, "Downloaded to " + target_file);
                    update_status("Downloaded to " + target_file, output_summary(task));
                } else {
                    if (st.stop_requested()) { set_cancelled(); return; }
                    log_error("Download failed: " + err);
                    fail("Download failed: " + err);
                    update_status("Download failed: " + err, output_summary(task));
                }
            } else {
                fail("Success, but video URL is empty.");
                update_status("Success, but video URL is empty.", output_summary(task));
            }
        } else {
            fail(task_summary(task));
            update_status(task_summary(task), output_summary(task));
        }
    } catch (const seedance2::ApiException& e) {
        if (st.stop_requested()) { set_cancelled(); return; }
        std::string status_msg = "Error: ApiException (status=" + std::to_string(e.http_status) + ", code=" + e.error.code + ")";
        fail(status_msg);
        update_status(status_msg, e.error.message);
    } catch (const std::exception& e) {
        if (st.stop_requested()) { set_cancelled(); return; }
        fail(std::string("Error: ") + e.what());
    }
    mark_idle();
}
