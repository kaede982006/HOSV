#include "SeedanceService.hpp"
#include <curl/curl.h>
#include <stdexcept>
#include <sstream>
#include <thread>
#include <algorithm>
#include <cstdlib>

namespace {
size_t write_string_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

seedance2::ClientOptions get_client_options(std::shared_ptr<IHttpTransport> transport) {
    seedance2::ClientOptions opts;
    opts.api_key = transport->get_api_key();
    opts.http.base_url = "https://api.seedance2.ai";
    if (const char* env_url = std::getenv("SEEDANCE2_API_URL")) {
        opts.http.base_url = env_url;
    }
    return opts;
}
}

CurlHttpTransport::CurlHttpTransport(const std::string& api_key, const std::string& base_url)
    : api_key_(api_key), base_url_(base_url) {}

std::string CurlHttpTransport::get_api_key() const { return api_key_; }

std::string CurlHttpTransport::request(const std::string& method, const std::string& path, const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("Failed to init curl");

    std::string response;
    struct curl_slist* headers = nullptr;
    std::string auth = "Authorization: Bearer " + api_key_;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");

    std::string url = base_url_ + path;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    } else if (method != "GET") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error(curl_easy_strerror(res));
    }

    if (http_code < 200 || http_code >= 300) {
        throw std::runtime_error("HTTP status " + std::to_string(http_code) + ": " + response);
    }

    return response;
}

AppStateTransport::AppStateTransport(std::shared_ptr<AppState> app)
    : app_(app) {}

std::string AppStateTransport::get_api_key() const {
    return app_->fields[0].value;
}

std::string AppStateTransport::request(const std::string& method, const std::string& path, const std::string& body) {
    std::string base_url = "https://api.seedance2.ai";
    if (const char* env_url = std::getenv("SEEDANCE2_API_URL")) {
        base_url = env_url;
    }
    CurlHttpTransport curl_transport(get_api_key(), base_url);
    return curl_transport.request(method, path, body);
}

SeedanceService::SeedanceService(std::shared_ptr<IHttpTransport> transport)
    : transport_(transport) {}

seedance2::GenerationTask SeedanceService::submit_text(const SceneInput& input) {
    auto opts = get_client_options(transport_);
    seedance2::Client client(opts);

    seedance2::GenerationOptions gen_opts;
    gen_opts.model = input.model_index == 1 ? seedance2::VideoModel::Seedance20Fast : seedance2::VideoModel::Seedance20;
    
    if (input.resolution_index == 0) gen_opts.resolution = seedance2::Resolution::R480p;
    else if (input.resolution_index == 1) gen_opts.resolution = seedance2::Resolution::R720p;
    else if (input.resolution_index == 2) gen_opts.resolution = seedance2::Resolution::R1080p;
    else gen_opts.resolution = seedance2::Resolution::R4k;

    if (input.aspect_index == 0) gen_opts.aspect_ratio = seedance2::AspectRatio::Ratio16x9;
    else if (input.aspect_index == 1) gen_opts.aspect_ratio = seedance2::AspectRatio::Ratio9x16;
    else if (input.aspect_index == 2) gen_opts.aspect_ratio = seedance2::AspectRatio::Ratio1x1;
    else if (input.aspect_index == 3) gen_opts.aspect_ratio = seedance2::AspectRatio::Ratio4x3;
    else if (input.aspect_index == 4) gen_opts.aspect_ratio = seedance2::AspectRatio::Ratio3x4;
    else if (input.aspect_index == 5) gen_opts.aspect_ratio = seedance2::AspectRatio::Ratio21x9;
    else gen_opts.aspect_ratio = seedance2::AspectRatio::Adaptive;

    gen_opts.duration_seconds = input.duration;
    gen_opts.generate_audio = input.audio;
    gen_opts.watermark = input.watermark;

    std::vector<std::string> ref_urls;
    for (const auto& p : input.local_images) {
        ref_urls.push_back(p.string());
    }

    if (ref_urls.empty()) {
        seedance2::TextToVideoRequest request;
        request.prompt = input.prompt;
        request.options = gen_opts;
        return client.create_text_to_video(request);
    } else if (ref_urls.size() <= 2) {
        seedance2::ImageToVideoRequest request;
        request.prompt = input.prompt;
        request.image_urls = ref_urls;
        request.options = gen_opts;
        return client.create_image_to_video(request);
    } else {
        seedance2::ReferenceToVideoRequest request;
        request.prompt = input.prompt;
        request.image_urls = ref_urls;
        request.options = gen_opts;
        return client.create_reference_to_video(request);
    }
}

seedance2::GenerationTask SeedanceService::lookup(const std::string& task_id) {
    auto opts = get_client_options(transport_);
    seedance2::Client client(opts);
    return client.get_task(task_id);
}

seedance2::GenerationTask SeedanceService::wait(const std::string& task_id,
                                               std::chrono::milliseconds interval,
                                               std::stop_token stop,
                                               std::function<void(const seedance2::GenerationTask&)> on_progress) {
    auto opts = get_client_options(transport_);
    seedance2::Client client(opts);

    while (true) {
        if (stop.stop_requested()) {
            throw std::runtime_error("Cancelled");
        }
        auto task = client.get_task(task_id);
        if (on_progress) {
            on_progress(task);
        }
        if (seedance2::is_terminal(task.status)) {
            return task;
        }

        auto sleep_remaining = interval;
        auto chunk = std::chrono::milliseconds(100);
        while (sleep_remaining > std::chrono::milliseconds(0)) {
            if (stop.stop_requested()) {
                throw std::runtime_error("Cancelled");
            }
            std::this_thread::sleep_for(std::min(chunk, sleep_remaining));
            sleep_remaining -= chunk;
        }
    }
}
