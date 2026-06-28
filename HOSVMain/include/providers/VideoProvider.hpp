#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

enum class VideoTaskStatus {
    Queued,
    Pending,
    Running,
    Processing,
    Succeeded,
    Completed,
    Failed,
    Cancelled,
    Expired,
    Timeout,
    Unknown
};

inline std::string to_string(VideoTaskStatus status) {
    switch (status) {
        case VideoTaskStatus::Queued: return "queued";
        case VideoTaskStatus::Pending: return "pending";
        case VideoTaskStatus::Running: return "running";
        case VideoTaskStatus::Processing: return "processing";
        case VideoTaskStatus::Succeeded: return "succeeded";
        case VideoTaskStatus::Completed: return "completed";
        case VideoTaskStatus::Failed: return "failed";
        case VideoTaskStatus::Cancelled: return "cancelled";
        case VideoTaskStatus::Expired: return "expired";
        case VideoTaskStatus::Timeout: return "timeout";
        case VideoTaskStatus::Unknown: return "unknown";
    }
    return "unknown";
}

inline bool is_terminal(VideoTaskStatus status) {
    return status == VideoTaskStatus::Succeeded || status == VideoTaskStatus::Completed ||
           status == VideoTaskStatus::Failed || status == VideoTaskStatus::Cancelled ||
           status == VideoTaskStatus::Expired || status == VideoTaskStatus::Timeout;
}

struct PollOptions {
    std::chrono::milliseconds initial_interval{3000};
    std::chrono::milliseconds max_interval{15000};
    std::chrono::milliseconds timeout{900000};
};

struct VideoGenerationRequest {
    std::string mode = "text_to_video";
    std::string prompt;
    std::string negative_prompt;
    std::vector<std::string> image_urls;
    std::vector<std::string> video_urls;
    std::vector<std::string> audio_urls;
    std::string ratio = "16:9";
    std::string resolution = "720p";
    int duration_seconds = 5;
    std::optional<int> fps;
    std::optional<long long> seed;
    std::string model;
    bool fast_mode = false;
    bool watermark = false;
    std::string output_dir;
    std::map<std::string, std::string> metadata;
};

struct VideoGenerationTask {
    std::string id;
    std::string provider = "byteplus_modelark";
    std::string created_at;
    std::string raw_provider_response;
};

struct VideoGenerationResult {
    std::string task_id;
    VideoTaskStatus status = VideoTaskStatus::Unknown;
    std::optional<int> progress;
    std::string video_url;
    std::string output_path;
    std::string error_code;
    std::string error_message;
    bool retryable = false;
    std::string raw_provider_response;
};

struct CreateTaskResult {
    bool ok = false;
    VideoGenerationTask task;
    VideoGenerationResult result;
};

struct TaskStatusResult {
    bool ok = false;
    VideoGenerationResult result;
};

struct CancelTaskResult {
    bool ok = false;
    VideoGenerationResult result;
};

class IVideoProvider {
public:
    virtual ~IVideoProvider() = default;
    virtual CreateTaskResult createTask(const VideoGenerationRequest& request) = 0;
    virtual TaskStatusResult getTask(const std::string& taskId) = 0;
    virtual TaskStatusResult waitTask(const std::string& taskId, PollOptions options) = 0;
    virtual CancelTaskResult cancelTask(const std::string& taskId) = 0;
};
