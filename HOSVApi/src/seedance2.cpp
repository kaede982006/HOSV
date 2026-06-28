#include "seedance2/seedance2.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <thread>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace seedance2 {
namespace {

using json = nlohmann::json;

std::string trim_trailing_slash(std::string value) {
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

std::string nested_string(const json& j, const std::vector<std::string>& path) {
    const json* current = &j;
    for (const auto& key : path) {
        if (!current->is_object() || !current->contains(key)) return {};
        current = &(*current)[key];
    }
    if (current->is_string()) return current->get<std::string>();
    if (current->is_number()) {
        std::ostringstream os;
        os << current->get<double>();
        return os.str();
    }
    return {};
}

int nested_int(const json& j, const std::vector<std::string>& path) {
    const json* current = &j;
    for (const auto& key : path) {
        if (!current->is_object() || !current->contains(key)) return 0;
        current = &(*current)[key];
    }
    if (current->is_number()) return current->get<int>();
    if (current->is_string()) return std::atoi(current->get<std::string>().c_str());
    return 0;
}

std::vector<std::string> collect_urls_from_json(const json& j) {
    std::vector<std::string> urls;
    auto collect_array = [&urls](const json& parent, const std::string& key) {
        if (!parent.is_object() || !parent.contains(key)) return;
        const auto& val = parent[key];
        if (!val.is_array()) return;
        for (const auto& item : val) {
            if (item.is_string()) {
                urls.push_back(item.get<std::string>());
            } else if (item.is_object()) {
                if (item.contains("url") && item["url"].is_string()) {
                    urls.push_back(item["url"].get<std::string>());
                } else if (item.contains("video_url") && item["video_url"].is_string()) {
                    urls.push_back(item["video_url"].get<std::string>());
                }
            }
        }
    };

    collect_array(j, "video_urls");
    collect_array(j, "videos");
    collect_array(j, "results");
    if (j.is_object() && j.contains("data") && j["data"].is_object()) {
        collect_array(j["data"], "video_urls");
        collect_array(j["data"], "videos");
        collect_array(j["data"], "results");
    }
    if (j.is_object() && j.contains("result") && j["result"].is_object()) {
        collect_array(j["result"], "video_urls");
        collect_array(j["result"], "videos");
    }
    std::sort(urls.begin(), urls.end());
    urls.erase(std::unique(urls.begin(), urls.end()), urls.end());
    return urls;
}

ErrorInfo parse_error_info(const json& j) {
    const json* source = &j;
    if (j.is_object() && j.contains("error") && j["error"].is_object()) {
        source = &j["error"];
    }
    ErrorInfo info;
    if (source->is_object()) {
        if (source->contains("code")) {
            auto& val = (*source)["code"];
            if (val.is_string()) info.code = val.get<std::string>();
            else if (val.is_number()) info.code = std::to_string(val.get<int>());
        }
        if (source->contains("message") && (*source)["message"].is_string()) {
            info.message = (*source)["message"].get<std::string>();
        }
        if (source->contains("type") && (*source)["type"].is_string()) {
            info.type = (*source)["type"].get<std::string>();
        }
    }
    if (info.message.empty() && source != &j && j.is_object() && j.contains("message") && j["message"].is_string()) {
        info.message = j["message"].get<std::string>();
    }
    return info;
}

GenerationTask parse_task(const std::string& body) {
    json root = json::parse(body);
    const json* source = &root;
    if (root.is_object() && root.contains("data") && root["data"].is_object()) source = &root["data"];
    if (root.is_object() && root.contains("task") && root["task"].is_object()) source = &root["task"];

    GenerationTask result;
    result.raw_json = body;
    result.id = nested_string(*source, {"id"});
    if (result.id.empty()) result.id = nested_string(*source, {"task_id"});
    if (result.id.empty()) result.id = nested_string(*source, {"taskId"});
    if (result.id.empty()) result.id = nested_string(root, {"id"});
    if (result.id.empty()) result.id = nested_string(root, {"task_id"});
    if (result.id.empty()) result.id = nested_string(root, {"taskId"});

    result.status = task_status_from_string(nested_string(*source, {"status"}));
    if (result.status == TaskStatus::Unknown) result.status = task_status_from_string(nested_string(root, {"status"}));

    result.output_url = nested_string(*source, {"output_url"});
    if (result.output_url.empty()) result.output_url = nested_string(*source, {"video_url"});
    if (result.output_url.empty()) result.output_url = nested_string(root, {"output_url"});
    if (result.output_url.empty()) result.output_url = nested_string(root, {"video_url"});

    result.created_at = nested_string(*source, {"created_at"});
    result.updated_at = nested_string(*source, {"updated_at"});
    result.model = nested_string(*source, {"model"});
    result.billing_status = nested_string(*source, {"billing_status"});
    result.failed_reason = nested_string(*source, {"failed_reason"});
    if (result.failed_reason.empty()) result.failed_reason = nested_string(*source, {"data", "failed_reason"});
    result.video_expires_at = nested_string(*source, {"data", "video_expires_at"});
    result.last_frame_url = nested_string(*source, {"data", "last_frame_url"});
    result.credits = nested_int(*source, {"credits"});
    if (result.credits == 0) result.credits = nested_int(root, {"credits"});
    result.processing_time_seconds = nested_int(*source, {"data", "processing_time"});
    result.video_urls = collect_urls_from_json(root);
    if (!result.output_url.empty() && std::find(result.video_urls.begin(), result.video_urls.end(), result.output_url) == result.video_urls.end()) {
        result.video_urls.push_back(result.output_url);
    }

    if ((root.is_object() && root.contains("error")) || (source->is_object() && source->contains("error"))) {
        result.error = parse_error_info(source->is_object() && source->contains("error") ? *source : root);
    }
    return result;
}

std::string text_request_body(const TextToVideoRequest& request) {
    json input = {
        {"prompt", request.prompt},
        {"generation_type", "text-to-video"},
        {"resolution", to_string(request.options.resolution)},
        {"aspect_ratio", to_string(request.options.aspect_ratio)},
        {"duration", request.options.duration_seconds},
        {"seed", request.options.seed}
    };
    if (request.options.camera_fixed != CameraFixed::Unset) {
        input["camera_fixed"] = request.options.camera_fixed == CameraFixed::On ? "on" : "off";
    }
    if (request.options.generate_audio.has_value()) {
        input["generate_audio"] = *request.options.generate_audio;
    }
    if (request.options.watermark.has_value()) {
        input["watermark"] = *request.options.watermark;
    }
    if (request.options.web_search.has_value()) {
        input["web_search"] = *request.options.web_search;
    }
    if (request.options.return_last_frame.has_value()) {
        input["return_last_frame"] = *request.options.return_last_frame;
    }

    json body = {
        {"model", to_string(request.options.model)},
        {"input", input}
    };
    if (!request.options.callback_url.empty()) {
        body["callback_url"] = request.options.callback_url;
    }
    return body.dump();
}

std::string image_request_body(const ImageToVideoRequest& request) {
    json input = {
        {"prompt", request.prompt},
        {"generation_type", "image-to-video"},
        {"resolution", to_string(request.options.resolution)},
        {"aspect_ratio", to_string(request.options.aspect_ratio)},
        {"duration", request.options.duration_seconds},
        {"seed", request.options.seed}
    };
    if (request.options.camera_fixed != CameraFixed::Unset) {
        input["camera_fixed"] = request.options.camera_fixed == CameraFixed::On ? "on" : "off";
    }
    if (request.options.generate_audio.has_value()) {
        input["generate_audio"] = *request.options.generate_audio;
    }
    if (request.options.watermark.has_value()) {
        input["watermark"] = *request.options.watermark;
    }
    if (request.options.web_search.has_value()) {
        input["web_search"] = *request.options.web_search;
    }
    if (request.options.return_last_frame.has_value()) {
        input["return_last_frame"] = *request.options.return_last_frame;
    }

    std::vector<std::string> urls = request.image_urls;
    if (urls.empty() && !request.image_url.empty()) urls.push_back(request.image_url);
    if (!urls.empty()) {
        input["image_urls"] = urls;
    }

    json body = {
        {"model", to_string(request.options.model)},
        {"input", input}
    };
    if (!request.options.callback_url.empty()) {
        body["callback_url"] = request.options.callback_url;
    }
    return body.dump();
}

std::string multi_image_request_body(const MultiImageRequest& request) {
    ImageToVideoRequest image_request;
    image_request.prompt = request.prompt;
    image_request.image_urls = request.image_urls;
    image_request.options = request.options;
    return image_request_body(image_request);
}

std::string reference_request_body(const ReferenceToVideoRequest& request) {
    json input = {
        {"prompt", request.prompt},
        {"generation_type", "reference-to-video"},
        {"resolution", to_string(request.options.resolution)},
        {"aspect_ratio", to_string(request.options.aspect_ratio)},
        {"duration", request.options.duration_seconds},
        {"seed", request.options.seed}
    };
    if (request.options.camera_fixed != CameraFixed::Unset) {
        input["camera_fixed"] = request.options.camera_fixed == CameraFixed::On ? "on" : "off";
    }
    if (request.options.generate_audio.has_value()) {
        input["generate_audio"] = *request.options.generate_audio;
    }
    if (request.options.watermark.has_value()) {
        input["watermark"] = *request.options.watermark;
    }
    if (request.options.web_search.has_value()) {
        input["web_search"] = *request.options.web_search;
    }
    if (request.options.return_last_frame.has_value()) {
        input["return_last_frame"] = *request.options.return_last_frame;
    }

    if (!request.image_urls.empty()) input["image_urls"] = request.image_urls;
    if (!request.video_urls.empty()) input["video_urls"] = request.video_urls;
    if (!request.audio_urls.empty()) input["audio_urls"] = request.audio_urls;

    json body = {
        {"model", to_string(request.options.model)},
        {"input", input}
    };
    if (!request.options.callback_url.empty()) {
        body["callback_url"] = request.options.callback_url;
    }
    return body.dump();
}

struct HttpResponse {
    long status = 0;
    std::string body;
};

std::size_t write_body(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    const std::size_t bytes = size * nmemb;
    body->append(ptr, bytes);
    return bytes;
}

bool valid_url_like(const std::string& value) {
    return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0;
}

void validate_common(const GenerationOptions& options) {
    if (!options.callback_url.empty() && options.callback_url.rfind("https://", 0) != 0) {
        throw std::invalid_argument("callback_url must start with https://");
    }
    if (options.duration_seconds < 4 || options.duration_seconds > 15) {
        throw std::invalid_argument("duration_seconds must be between 4 and 15");
    }
    if (options.seed < -1 || options.seed > 4294967295LL) {
        throw std::invalid_argument("seed must be -1 or between 0 and 4294967295");
    }
}

} // namespace

ApiException::ApiException(int status, ErrorInfo error_info, std::string body)
    : std::runtime_error(error_info.message.empty() ? body : error_info.message),
      http_status(status),
      error(std::move(error_info)),
      response_body(std::move(body)) {}

std::string to_string(VideoModel value) {
    switch (value) {
        case VideoModel::Seedance20: return "seedance-2-0";
        case VideoModel::Seedance20Fast: return "seedance-2-0-fast";
    }
    return "seedance-2-0";
}

std::string to_string(Resolution value) {
    switch (value) {
        case Resolution::R480p: return "480p";
        case Resolution::R720p: return "720p";
        case Resolution::R1080p: return "1080p";
        case Resolution::R4k: return "4k";
    }
    return "720p";
}

std::string to_string(AspectRatio value) {
    switch (value) {
        case AspectRatio::Ratio16x9: return "16:9";
        case AspectRatio::Ratio9x16: return "9:16";
        case AspectRatio::Ratio1x1: return "1:1";
        case AspectRatio::Ratio4x3: return "4:3";
        case AspectRatio::Ratio3x4: return "3:4";
        case AspectRatio::Ratio21x9: return "21:9";
        case AspectRatio::Adaptive: return "adaptive";
    }
    return "16:9";
}

std::string to_string(TaskStatus value) {
    switch (value) {
        case TaskStatus::Submitted: return "submitted";
        case TaskStatus::Queued: return "queued";
        case TaskStatus::Running: return "running";
        case TaskStatus::Processing: return "processing";
        case TaskStatus::Succeeded: return "succeeded";
        case TaskStatus::Failed: return "failed";
        case TaskStatus::Cancelled: return "cancelled";
        case TaskStatus::Unknown: return "unknown";
    }
    return "unknown";
}

TaskStatus task_status_from_string(const std::string& value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (char c : value) {
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (normalized == "submitted" || normalized == "created") return TaskStatus::Submitted;
    if (normalized == "queued" || normalized == "pending") return TaskStatus::Queued;
    if (normalized == "running" || normalized == "generating") return TaskStatus::Running;
    if (normalized == "processing" || normalized == "in_progress") return TaskStatus::Processing;
    if (normalized == "succeeded" || normalized == "success" || normalized == "completed" || normalized == "done") return TaskStatus::Succeeded;
    if (normalized == "failed" || normalized == "error") return TaskStatus::Failed;
    if (normalized == "cancelled" || normalized == "canceled") return TaskStatus::Cancelled;
    return TaskStatus::Unknown;
}

bool is_terminal(TaskStatus status) {
    return status == TaskStatus::Succeeded || status == TaskStatus::Failed || status == TaskStatus::Cancelled;
}

void validate(const TextToVideoRequest& request) {
    if (request.prompt.empty()) throw std::invalid_argument("prompt is required");
    validate_common(request.options);
}

void validate(const ImageToVideoRequest& request) {
    if (request.prompt.empty()) throw std::invalid_argument("prompt is required");
    std::vector<std::string> urls = request.image_urls;
    if (urls.empty() && !request.image_url.empty()) urls.push_back(request.image_url);
    if (urls.empty()) throw std::invalid_argument("image_urls must contain one or two URLs");
    if (urls.size() > 2) throw std::invalid_argument("image-to-video accepts at most two image URLs");
    for (const auto& url : urls) {
        if (!valid_url_like(url)) throw std::invalid_argument("every image URL must start with http:// or https://");
    }
    validate_common(request.options);
}

void validate(const MultiImageRequest& request) {
    if (request.prompt.empty()) throw std::invalid_argument("prompt is required");
    if (request.image_urls.empty()) throw std::invalid_argument("image_urls must contain at least one URL");
    if (request.image_urls.size() > 2) throw std::invalid_argument("multi-image image-to-video accepts at most two image URLs");
    for (const auto& url : request.image_urls) {
        if (!valid_url_like(url)) throw std::invalid_argument("every image URL must start with http:// or https://");
    }
    validate_common(request.options);
}

void validate(const ReferenceToVideoRequest& request) {
    if (request.prompt.empty()) throw std::invalid_argument("prompt is required");
    if (request.image_urls.empty() && request.video_urls.empty()) {
        throw std::invalid_argument("reference-to-video requires at least one image or video URL");
    }
    if (request.image_urls.size() > 9) throw std::invalid_argument("reference-to-video accepts at most nine image URLs");
    if (request.video_urls.size() > 3) throw std::invalid_argument("reference-to-video accepts at most three video URLs");
    if (request.audio_urls.size() > 3) throw std::invalid_argument("reference-to-video accepts at most three audio URLs");
    for (const auto& url : request.image_urls) {
        if (!valid_url_like(url)) throw std::invalid_argument("every image URL must start with http:// or https://");
    }
    for (const auto& url : request.video_urls) {
        if (!valid_url_like(url)) throw std::invalid_argument("every video URL must start with http:// or https://");
    }
    for (const auto& url : request.audio_urls) {
        if (!valid_url_like(url)) throw std::invalid_argument("every audio URL must start with http:// or https://");
    }
    validate_common(request.options);
}

struct Client::Impl {
    explicit Impl(ClientOptions opts) : options(std::move(opts)) {
        if (options.api_key.empty()) throw std::invalid_argument("api_key is required");
        options.http.base_url = trim_trailing_slash(options.http.base_url);
        static const int curl_initialized = [] {
            return static_cast<int>(curl_global_init(CURL_GLOBAL_DEFAULT));
        }();
        if (curl_initialized != 0) throw std::runtime_error("curl_global_init failed");
    }

    ~Impl() = default;

    HttpResponse request(const std::string& method, const std::string& path, const std::string& body = {}) const {
        CURL* curl = curl_easy_init();
        if (!curl) throw std::runtime_error("curl_easy_init failed");

        std::string response_body;
        struct curl_slist* headers = nullptr;
        std::string auth = "Authorization: Bearer " + options.api_key;
        headers = curl_slist_append(headers, auth.c_str());
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Accept: application/json");
        for (const auto& kv : options.http.extra_headers) {
            std::string header = kv.first + ": " + kv.second;
            headers = curl_slist_append(headers, header.c_str());
        }

        std::string url = options.http.base_url + path;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(options.http.connect_timeout.count()));
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(options.http.request_timeout.count()));
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        if (method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        } else if (method != "GET") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        }

        CURLcode rc = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (rc != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(rc));
        }

        HttpResponse response{status, std::move(response_body)};
        if (status < 200 || status >= 300) {
            ErrorInfo error;
            try {
                error = parse_error_info(json::parse(response.body));
            } catch (...) {
                error.message = response.body;
            }
            throw ApiException(static_cast<int>(status), std::move(error), response.body);
        }
        return response;
    }

    ClientOptions options;
};

Client::Client(ClientOptions options) : impl_(new Impl(std::move(options))) {}

Client::~Client() {
    delete impl_;
}

Client::Client(Client&& other) noexcept : impl_(other.impl_) {
    other.impl_ = nullptr;
}

Client& Client::operator=(Client&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

GenerationTask Client::create_text_to_video(const TextToVideoRequest& request) const {
    validate(request);
    return parse_task(impl_->request("POST", "/v1/videos/generations", text_request_body(request)).body);
}

GenerationTask Client::create_image_to_video(const ImageToVideoRequest& request) const {
    validate(request);
    return parse_task(impl_->request("POST", "/v1/videos/generations", image_request_body(request)).body);
}

GenerationTask Client::create_multi_image_video(const MultiImageRequest& request) const {
    validate(request);
    return parse_task(impl_->request("POST", "/v1/videos/generations", multi_image_request_body(request)).body);
}

GenerationTask Client::create_reference_to_video(const ReferenceToVideoRequest& request) const {
    validate(request);
    return parse_task(impl_->request("POST", "/v1/videos/generations", reference_request_body(request)).body);
}

GenerationTask Client::get_task(const std::string& task_id) const {
    if (task_id.empty()) throw std::invalid_argument("task_id is required");
    return parse_task(impl_->request("GET", "/v1/tasks/" + task_id).body);
}

GenerationTask Client::wait_for_task(
    const std::string& task_id,
    PollOptions options,
    ProgressCallback on_progress) const {
    const auto start = std::chrono::steady_clock::now();
    while (true) {
        GenerationTask task = get_task(task_id);
        if (on_progress) on_progress(task);
        if (is_terminal(task.status)) return task;
        if (options.timeout.count() > 0 && std::chrono::steady_clock::now() - start >= options.timeout) {
            throw std::runtime_error("Timed out waiting for Seedance task");
        }
        std::this_thread::sleep_for(options.interval);
    }
}

WebhookEvent Client::parse_webhook(const std::string& payload) const {
    GenerationTask task = parse_task(payload);
    WebhookEvent event;
    event.task_id = task.id;
    event.status = task.status;
    event.video_urls = std::move(task.video_urls);
    event.output_url = std::move(task.output_url);
    event.error = std::move(task.error);
    event.raw_json = payload;
    return event;
}

} // namespace seedance2
