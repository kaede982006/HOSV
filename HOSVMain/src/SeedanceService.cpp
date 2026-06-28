#include "SeedanceService.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string url_path_without_query(std::string url) {
    const auto query = url.find_first_of("?#");
    if (query != std::string::npos) {
        url.resize(query);
    }
    return lower_copy(std::move(url));
}

bool has_suffix(const std::string& value, const std::vector<std::string>& suffixes) {
    return std::any_of(suffixes.begin(), suffixes.end(), [&](const std::string& suffix) {
        return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    });
}

std::string resolution_from_index(int index) {
    if (index == 0) return "480p";
    if (index == 2) return "1080p";
    if (index == 3) return "4k";
    return "720p";
}

std::string ratio_from_index(int index) {
    if (index == 1) return "9:16";
    if (index == 2) return "1:1";
    if (index == 3) return "4:3";
    if (index == 4) return "3:4";
    if (index == 5) return "21:9";
    if (index == 6) return "adaptive";
    return "16:9";
}

void classify_refs(const std::vector<std::string>& refs, VideoGenerationRequest& request) {
    for (const auto& url : refs) {
        const auto path = url_path_without_query(url);
        if (has_suffix(path, {".mp4", ".mov", ".webm", ".m4v"})) {
            request.video_urls.push_back(url);
        } else if (has_suffix(path, {".mp3", ".wav", ".m4a", ".aac", ".flac", ".ogg"})) {
            request.audio_urls.push_back(url);
        } else {
            request.image_urls.push_back(url);
        }
    }
    if (!request.video_urls.empty() || !request.audio_urls.empty() || request.image_urls.size() > 2) {
        request.mode = "reference_to_video";
    } else if (!request.image_urls.empty()) {
        request.mode = "image_to_video";
    } else {
        request.mode = "text_to_video";
    }
}
}

SeedanceService::SeedanceService(std::shared_ptr<IVideoProvider> provider)
    : provider_(std::move(provider)) {}

VideoGenerationTask SeedanceService::submit_text(const SceneInput& input) {
    VideoGenerationRequest request;
    request.prompt = input.prompt;
    request.resolution = resolution_from_index(input.resolution_index);
    request.ratio = ratio_from_index(input.aspect_index);
    request.duration_seconds = input.duration;
    request.fast_mode = input.model_index == 1;
    request.watermark = input.watermark;
    request.output_dir = input.save_path;
    classify_refs(input.reference_urls, request);

    auto created = provider_->createTask(request);
    if (!created.ok) {
        throw std::runtime_error(created.result.error_message.empty() ? created.result.error_code : created.result.error_message);
    }
    return created.task;
}

VideoGenerationResult SeedanceService::lookup(const std::string& task_id) {
    auto result = provider_->getTask(task_id);
    if (!result.ok && result.result.error_message.empty()) {
        result.result.error_message = result.result.error_code;
    }
    return result.result;
}

VideoGenerationResult SeedanceService::wait(const std::string& task_id,
                                           std::chrono::milliseconds interval,
                                           std::stop_token stop,
                                           std::function<void(const VideoGenerationResult&)> on_progress) {
    PollOptions options;
    options.initial_interval = interval;
    while (!stop.stop_requested()) {
        auto result = provider_->getTask(task_id).result;
        if (on_progress) {
            on_progress(result);
        }
        if (is_terminal(result.status)) {
            return result;
        }
        auto sleep_remaining = interval;
        const auto chunk = std::chrono::milliseconds(100);
        while (sleep_remaining > std::chrono::milliseconds(0)) {
            if (stop.stop_requested()) {
                throw std::runtime_error("Cancelled");
            }
            const auto nap = std::min(chunk, sleep_remaining);
            std::this_thread::sleep_for(nap);
            sleep_remaining -= nap;
        }
    }
    throw std::runtime_error("Cancelled");
}
