#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  if defined(SEEDANCE2_BUILDING_LIBRARY)
#    define SEEDANCE2_API __declspec(dllexport)
#  else
#    define SEEDANCE2_API __declspec(dllimport)
#  endif
#else
#  define SEEDANCE2_API __attribute__((visibility("default")))
#endif

namespace seedance2 {

enum class VideoModel {
    Seedance20,
    Seedance20Fast
};

enum class Resolution {
    R480p,
    R720p,
    R1080p,
    R4k
};

enum class AspectRatio {
    Ratio16x9,
    Ratio9x16,
    Ratio1x1,
    Ratio4x3,
    Ratio3x4,
    Ratio21x9,
    Adaptive
};

enum class CameraFixed {
    Unset,
    On,
    Off
};

enum class TaskStatus {
    Unknown,
    Submitted,
    Queued,
    Running,
    Processing,
    Succeeded,
    Failed,
    Cancelled
};

struct SEEDANCE2_API ErrorInfo {
    std::string code;
    std::string message;
    std::string type;
};

struct SEEDANCE2_API ApiException : public std::runtime_error {
    int http_status = 0;
    ErrorInfo error;
    std::string response_body;

    ApiException(int status, ErrorInfo error_info, std::string body);
};

struct SEEDANCE2_API HttpOptions {
    std::string base_url = "https://api.seedance2.ai";
    std::chrono::milliseconds connect_timeout{10000};
    std::chrono::milliseconds request_timeout{120000};
    std::map<std::string, std::string> extra_headers;
};

struct SEEDANCE2_API ClientOptions {
    std::string api_key;
    HttpOptions http;
};

struct SEEDANCE2_API GenerationOptions {
    VideoModel model = VideoModel::Seedance20;
    Resolution resolution = Resolution::R720p;
    AspectRatio aspect_ratio = AspectRatio::Ratio16x9;
    int duration_seconds = 5;
    CameraFixed camera_fixed = CameraFixed::Unset;
    std::int64_t seed = -1;
    std::optional<bool> generate_audio;
    std::optional<bool> watermark;
    std::optional<bool> web_search;
    std::optional<bool> return_last_frame;
    std::string callback_url;
};

struct SEEDANCE2_API TextToVideoRequest {
    std::string prompt;
    GenerationOptions options;
};

struct SEEDANCE2_API ImageToVideoRequest {
    std::string prompt;
    std::vector<std::string> image_urls;
    std::string image_url;
    GenerationOptions options;
};

struct SEEDANCE2_API MultiImageRequest {
    std::string prompt;
    std::vector<std::string> image_urls;
    GenerationOptions options;
};

struct SEEDANCE2_API ReferenceToVideoRequest {
    std::string prompt;
    std::vector<std::string> image_urls;
    std::vector<std::string> video_urls;
    std::vector<std::string> audio_urls;
    GenerationOptions options;
};

struct SEEDANCE2_API GenerationTask {
    std::string id;
    TaskStatus status = TaskStatus::Unknown;
    std::vector<std::string> video_urls;
    std::string output_url;
    std::string last_frame_url;
    std::string video_expires_at;
    std::string created_at;
    std::string updated_at;
    std::string model;
    std::string billing_status;
    int credits = 0;
    int processing_time_seconds = 0;
    std::string failed_reason;
    std::optional<ErrorInfo> error;
    std::string raw_json;
};

struct SEEDANCE2_API WebhookEvent {
    std::string task_id;
    TaskStatus status = TaskStatus::Unknown;
    std::vector<std::string> video_urls;
    std::string output_url;
    std::optional<ErrorInfo> error;
    std::string raw_json;
};

struct SEEDANCE2_API PollOptions {
    std::chrono::milliseconds interval{10000};
    std::chrono::milliseconds timeout{0};
};

using ProgressCallback = std::function<void(const GenerationTask&)>;

SEEDANCE2_API std::string to_string(VideoModel value);
SEEDANCE2_API std::string to_string(Resolution value);
SEEDANCE2_API std::string to_string(AspectRatio value);
SEEDANCE2_API std::string to_string(TaskStatus value);

SEEDANCE2_API TaskStatus task_status_from_string(const std::string& value);

class SEEDANCE2_API Client {
public:
    explicit Client(ClientOptions options);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;

    GenerationTask create_text_to_video(const TextToVideoRequest& request) const;
    GenerationTask create_image_to_video(const ImageToVideoRequest& request) const;
    GenerationTask create_multi_image_video(const MultiImageRequest& request) const;
    GenerationTask create_reference_to_video(const ReferenceToVideoRequest& request) const;

    GenerationTask get_task(const std::string& task_id) const;
    GenerationTask wait_for_task(
        const std::string& task_id,
        PollOptions options = {},
        ProgressCallback on_progress = {}) const;

    WebhookEvent parse_webhook(const std::string& payload) const;

private:
    struct Impl;
    Impl* impl_;
};

SEEDANCE2_API bool is_terminal(TaskStatus status);
SEEDANCE2_API void validate(const TextToVideoRequest& request);
SEEDANCE2_API void validate(const ImageToVideoRequest& request);
SEEDANCE2_API void validate(const MultiImageRequest& request);
SEEDANCE2_API void validate(const ReferenceToVideoRequest& request);

} // namespace seedance2
