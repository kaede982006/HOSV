#include "providers/BytePlusArkVideoProvider.hpp"

#include <cstdlib>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unistd.h>
#include <utility>

namespace {
using json = nlohmann::json;

std::string default_bridge_path() {
    if (const char* path = std::getenv("BYTEPLUS_ARK_BRIDGE_PATH")) {
        if (*path) return path;
    }
    if (std::filesystem::exists("./arkbridge")) {
        return "./arkbridge";
    }
    char exe_path[4096] = {};
    const ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        const auto beside_exe = std::filesystem::path(exe_path).parent_path() / "arkbridge";
        if (std::filesystem::exists(beside_exe)) {
            return beside_exe.string();
        }
    }
    return "./arkbridge";
}

VideoTaskStatus parse_status(const std::string& value) {
    if (value == "queued") return VideoTaskStatus::Queued;
    if (value == "pending") return VideoTaskStatus::Pending;
    if (value == "running") return VideoTaskStatus::Running;
    if (value == "processing") return VideoTaskStatus::Processing;
    if (value == "succeeded") return VideoTaskStatus::Succeeded;
    if (value == "completed") return VideoTaskStatus::Completed;
    if (value == "failed") return VideoTaskStatus::Failed;
    if (value == "cancelled") return VideoTaskStatus::Cancelled;
    if (value == "expired") return VideoTaskStatus::Expired;
    if (value == "timeout") return VideoTaskStatus::Timeout;
    return VideoTaskStatus::Unknown;
}

VideoGenerationResult parse_result(const std::string& stdout_text) {
    json root = json::parse(stdout_text.empty() ? "{}" : stdout_text);
    VideoGenerationResult result;
    result.raw_provider_response = stdout_text;
    result.task_id = root.value("task_id", "");
    result.status = parse_status(root.value("status", "unknown"));
    result.video_url = root.value("video_url", "");
    result.error_code = root.value("error_code", "");
    result.error_message = root.value("error_message", "");
    result.retryable = root.value("retryable", false);
    if (root.contains("progress") && root["progress"].is_number_integer()) {
        result.progress = root["progress"].get<int>();
    }
    return result;
}

bool ok_response(const std::string& stdout_text) {
    try {
        return json::parse(stdout_text).value("ok", false);
    } catch (...) {
        return false;
    }
}

void throw_if_deprecated_provider(const std::string& provider) {
    if (provider == "seedance2.ai" || provider == "api.seedance2.ai" || provider == "seedance2") {
        throw std::runtime_error("The unofficial seedance2.ai provider has been disabled. Use the official BytePlus ModelArk provider.");
    }
}
}

BytePlusArkVideoProvider::BytePlusArkVideoProvider(std::string bridge_path, std::chrono::milliseconds timeout)
    : bridge_path_(bridge_path.empty() ? default_bridge_path() : std::move(bridge_path)), timeout_(timeout) {
    if (const char* provider = std::getenv("HOSV_PROVIDER")) {
        throw_if_deprecated_provider(provider);
    }
}

CreateTaskResult BytePlusArkVideoProvider::createTask(const VideoGenerationRequest& request) {
    if (request.prompt.empty()) {
        throw std::invalid_argument("prompt is required");
    }
    json body = {
        {"mode", request.mode},
        {"prompt", request.prompt},
        {"image_urls", request.image_urls},
        {"video_urls", request.video_urls},
        {"audio_urls", request.audio_urls},
        {"ratio", request.ratio},
        {"resolution", request.resolution},
        {"duration_seconds", request.duration_seconds},
        {"model", request.model},
        {"fast_mode", request.fast_mode},
        {"watermark", request.watermark},
        {"output_dir", request.output_dir},
        {"metadata", request.metadata}
    };
    if (!request.negative_prompt.empty()) body["negative_prompt"] = request.negative_prompt;
    if (request.fps) body["fps"] = *request.fps;
    if (request.seed) body["seed"] = *request.seed;

    const auto bridge = runner_.run(bridge_path_, "create", body.dump(), timeout_);
    CreateTaskResult out;
    out.ok = bridge.exit_code == 0 && !bridge.timed_out && ok_response(bridge.stdout_text);
    out.result = parse_result(bridge.stdout_text);
    out.task.id = out.result.task_id;
    out.task.raw_provider_response = bridge.stdout_text;
    if (bridge.timed_out) {
        out.result.error_code = "polling_timeout";
        out.result.error_message = "arkbridge create timed out";
    } else if (!out.ok && out.result.error_message.empty()) {
        out.result.error_code = "unknown_error";
        out.result.error_message = bridge.stderr_text.empty() ? "arkbridge create failed" : bridge.stderr_text;
    }
    return out;
}

TaskStatusResult BytePlusArkVideoProvider::getTask(const std::string& taskId) {
    return runTaskCommand("get", taskId);
}

TaskStatusResult BytePlusArkVideoProvider::waitTask(const std::string& taskId, PollOptions options) {
    return runTaskCommand("wait", taskId, &options);
}

CancelTaskResult BytePlusArkVideoProvider::cancelTask(const std::string& taskId) {
    const auto status = runTaskCommand("cancel", taskId);
    return CancelTaskResult{status.ok, status.result};
}

TaskStatusResult BytePlusArkVideoProvider::runTaskCommand(const std::string& command, const std::string& taskId, const PollOptions* poll) {
    json body = {{"task_id", taskId}};
    if (poll) {
        body["poll_initial_ms"] = static_cast<int>(poll->initial_interval.count());
        body["poll_max_ms"] = static_cast<int>(poll->max_interval.count());
        body["poll_timeout_ms"] = static_cast<int>(poll->timeout.count());
    }
    const auto bridge = runner_.run(bridge_path_, command, body.dump(), poll ? poll->timeout + std::chrono::seconds(30) : timeout_);
    TaskStatusResult out;
    out.ok = bridge.exit_code == 0 && !bridge.timed_out && ok_response(bridge.stdout_text);
    out.result = parse_result(bridge.stdout_text);
    if (bridge.timed_out) {
        out.result.error_code = "polling_timeout";
        out.result.error_message = "arkbridge " + command + " timed out";
    } else if (!out.ok && out.result.error_message.empty()) {
        out.result.error_code = "unknown_error";
        out.result.error_message = bridge.stderr_text.empty() ? "arkbridge " + command + " failed" : bridge.stderr_text;
    }
    return out;
}
