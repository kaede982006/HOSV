#include "seedance2/seedance2.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <thread>

#include <curl/curl.h>

namespace seedance2 {
namespace {

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Json> array;
    std::map<std::string, Json> object;

    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }
    bool is_string() const { return type == Type::String; }

    const Json* get(const std::string& key) const {
        if (!is_object()) return nullptr;
        auto it = object.find(key);
        return it == object.end() ? nullptr : &it->second;
    }

    std::string str_or_empty() const {
        if (type == Type::String) return string;
        if (type == Type::Number) {
            std::ostringstream os;
            os << number;
            return os.str();
        }
        return {};
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& input) : input_(input) {}

    Json parse() {
        skip_ws();
        Json value = parse_value();
        skip_ws();
        if (pos_ != input_.size()) throw std::runtime_error("Unexpected trailing JSON content");
        return value;
    }

private:
    Json parse_value() {
        skip_ws();
        if (pos_ >= input_.size()) throw std::runtime_error("Unexpected end of JSON");
        const char c = input_[pos_];
        if (c == 'n') return parse_literal("null", Json{});
        if (c == 't') {
            Json j;
            j.type = Json::Type::Bool;
            j.boolean = true;
            return parse_literal("true", j);
        }
        if (c == 'f') {
            Json j;
            j.type = Json::Type::Bool;
            j.boolean = false;
            return parse_literal("false", j);
        }
        if (c == '"') return parse_string_json();
        if (c == '[') return parse_array();
        if (c == '{') return parse_object();
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parse_number();
        throw std::runtime_error("Invalid JSON value");
    }

    Json parse_literal(const char* literal, Json value) {
        const auto len = std::strlen(literal);
        if (input_.compare(pos_, len, literal) != 0) {
            throw std::runtime_error("Invalid JSON literal");
        }
        pos_ += len;
        return value;
    }

    Json parse_string_json() {
        Json j;
        j.type = Json::Type::String;
        j.string = parse_string();
        return j;
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (pos_ < input_.size()) {
            char c = input_[pos_++];
            if (c == '"') return out;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= input_.size()) throw std::runtime_error("Invalid JSON escape");
            char e = input_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u':
                    append_unicode_escape(out);
                    break;
                default:
                    throw std::runtime_error("Unsupported JSON escape");
            }
        }
        throw std::runtime_error("Unterminated JSON string");
    }

    void append_unicode_escape(std::string& out) {
        if (pos_ + 4 > input_.size()) throw std::runtime_error("Invalid unicode escape");
        unsigned int code = 0;
        for (int i = 0; i < 4; ++i) {
            char c = input_[pos_++];
            code <<= 4;
            if (c >= '0' && c <= '9') code += c - '0';
            else if (c >= 'a' && c <= 'f') code += 10 + c - 'a';
            else if (c >= 'A' && c <= 'F') code += 10 + c - 'A';
            else throw std::runtime_error("Invalid unicode escape");
        }
        if (code <= 0x7F) {
            out.push_back(static_cast<char>(code));
        } else if (code <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | ((code >> 6) & 0x1F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | ((code >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    Json parse_array() {
        Json j;
        j.type = Json::Type::Array;
        expect('[');
        skip_ws();
        if (consume(']')) return j;
        while (true) {
            j.array.push_back(parse_value());
            skip_ws();
            if (consume(']')) return j;
            expect(',');
        }
    }

    Json parse_object() {
        Json j;
        j.type = Json::Type::Object;
        expect('{');
        skip_ws();
        if (consume('}')) return j;
        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            expect(':');
            j.object.emplace(std::move(key), parse_value());
            skip_ws();
            if (consume('}')) return j;
            expect(',');
        }
    }

    Json parse_number() {
        const std::size_t start = pos_;
        if (input_[pos_] == '-') ++pos_;
        while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        if (pos_ < input_.size() && input_[pos_] == '.') {
            ++pos_;
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        }
        Json j;
        j.type = Json::Type::Number;
        j.number = std::strtod(input_.c_str() + start, nullptr);
        return j;
    }

    void skip_ws() {
        while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) ++pos_;
    }

    bool consume(char c) {
        if (pos_ < input_.size() && input_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    void expect(char c) {
        skip_ws();
        if (!consume(c)) throw std::runtime_error("Unexpected JSON character");
    }

    const std::string& input_;
    std::size_t pos_ = 0;
};

std::string escape_json(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    for (char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 0x0F]);
                    out.push_back(hex[c & 0x0F]);
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

void add_string_field(std::ostringstream& os, bool& first, const std::string& key, const std::string& value) {
    if (value.empty()) return;
    if (!first) os << ',';
    first = false;
    os << '"' << key << "\":\"" << escape_json(value) << '"';
}

void add_required_string_field(std::ostringstream& os, bool& first, const std::string& key, const std::string& value) {
    if (!first) os << ',';
    first = false;
    os << '"' << key << "\":\"" << escape_json(value) << '"';
}

void add_int_field(std::ostringstream& os, bool& first, const std::string& key, std::int64_t value) {
    if (!first) os << ',';
    first = false;
    os << '"' << key << "\":" << value;
}

void add_bool_field(std::ostringstream& os, bool& first, const std::string& key, const std::optional<bool>& value) {
    if (!value.has_value()) return;
    if (!first) os << ',';
    first = false;
    os << '"' << key << "\":" << (*value ? "true" : "false");
}

void add_string_array_field(std::ostringstream& os, bool& first, const std::string& key, const std::vector<std::string>& values) {
    if (values.empty()) return;
    if (!first) os << ',';
    first = false;
    os << '"' << key << "\":[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) os << ',';
        os << '"' << escape_json(values[i]) << '"';
    }
    os << ']';
}

void add_input_options(std::ostringstream& os, bool& first, const GenerationOptions& options) {
    add_required_string_field(os, first, "resolution", to_string(options.resolution));
    add_required_string_field(os, first, "aspect_ratio", to_string(options.aspect_ratio));
    add_int_field(os, first, "duration", options.duration_seconds);
    if (options.camera_fixed != CameraFixed::Unset) {
        add_required_string_field(os, first, "camera_fixed", options.camera_fixed == CameraFixed::On ? "on" : "off");
    }
    add_bool_field(os, first, "generate_audio", options.generate_audio);
    add_bool_field(os, first, "watermark", options.watermark);
    add_bool_field(os, first, "web_search", options.web_search);
    add_bool_field(os, first, "return_last_frame", options.return_last_frame);
    add_int_field(os, first, "seed", options.seed);
}

void begin_generation_body(std::ostringstream& os, bool& top_first, const GenerationOptions& options) {
    os << '{';
    top_first = true;
    add_required_string_field(os, top_first, "model", to_string(options.model));
    add_string_field(os, top_first, "callback_url", options.callback_url);
    if (!top_first) os << ',';
    top_first = false;
    os << "\"input\":{";
}

std::string text_request_body(const TextToVideoRequest& request) {
    std::ostringstream os;
    bool top_first = true;
    begin_generation_body(os, top_first, request.options);
    bool first = true;
    add_required_string_field(os, first, "prompt", request.prompt);
    add_required_string_field(os, first, "generation_type", "text-to-video");
    add_input_options(os, first, request.options);
    os << "}}";
    return os.str();
}

std::string image_request_body(const ImageToVideoRequest& request) {
    std::ostringstream os;
    bool top_first = true;
    begin_generation_body(os, top_first, request.options);
    bool first = true;
    add_required_string_field(os, first, "prompt", request.prompt);
    add_required_string_field(os, first, "generation_type", "image-to-video");
    std::vector<std::string> urls = request.image_urls;
    if (urls.empty() && !request.image_url.empty()) urls.push_back(request.image_url);
    add_string_array_field(os, first, "image_urls", urls);
    add_input_options(os, first, request.options);
    os << "}}";
    return os.str();
}

std::string multi_image_request_body(const MultiImageRequest& request) {
    ImageToVideoRequest image_request;
    image_request.prompt = request.prompt;
    image_request.image_urls = request.image_urls;
    image_request.options = request.options;
    return image_request_body(image_request);
}

std::string reference_request_body(const ReferenceToVideoRequest& request) {
    std::ostringstream os;
    bool top_first = true;
    begin_generation_body(os, top_first, request.options);
    bool first = true;
    add_required_string_field(os, first, "prompt", request.prompt);
    add_required_string_field(os, first, "generation_type", "reference-to-video");
    add_string_array_field(os, first, "image_urls", request.image_urls);
    add_string_array_field(os, first, "video_urls", request.video_urls);
    add_string_array_field(os, first, "audio_urls", request.audio_urls);
    add_input_options(os, first, request.options);
    os << "}}";
    return os.str();
}

std::string trim_trailing_slash(std::string value) {
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

std::string nested_string(const Json& json, const std::vector<std::string>& path) {
    const Json* current = &json;
    for (const auto& key : path) {
        current = current->get(key);
        if (!current) return {};
    }
    return current->str_or_empty();
}

int nested_int(const Json& json, const std::vector<std::string>& path) {
    const Json* current = &json;
    for (const auto& key : path) {
        current = current->get(key);
        if (!current) return 0;
    }
    if (current->type == Json::Type::Number) return static_cast<int>(current->number);
    if (current->type == Json::Type::String) return std::atoi(current->string.c_str());
    return 0;
}

std::vector<std::string> collect_urls_from_json(const Json& json) {
    std::vector<std::string> urls;
    auto collect_array = [&urls](const Json* value) {
        if (!value || !value->is_array()) return;
        for (const auto& item : value->array) {
            if (item.is_string()) {
                urls.push_back(item.string);
            } else if (item.is_object()) {
                if (auto url = item.get("url"); url && url->is_string()) urls.push_back(url->string);
                else if (auto video_url = item.get("video_url"); video_url && video_url->is_string()) urls.push_back(video_url->string);
            }
        }
    };

    collect_array(json.get("video_urls"));
    collect_array(json.get("videos"));
    collect_array(json.get("results"));
    if (auto data = json.get("data")) {
        collect_array(data->get("video_urls"));
        collect_array(data->get("videos"));
        collect_array(data->get("results"));
    }
    if (auto result = json.get("result")) {
        collect_array(result->get("video_urls"));
        collect_array(result->get("videos"));
    }
    std::sort(urls.begin(), urls.end());
    urls.erase(std::unique(urls.begin(), urls.end()), urls.end());
    return urls;
}

ErrorInfo parse_error_info(const Json& json) {
    const Json* source = &json;
    if (auto e = json.get("error"); e && e->is_object()) source = e;
    ErrorInfo info;
    if (auto code = source->get("code")) info.code = code->str_or_empty();
    if (auto message = source->get("message")) info.message = message->str_or_empty();
    if (auto type = source->get("type")) info.type = type->str_or_empty();
    if (info.message.empty() && source != &json) {
        if (auto error_message = json.get("message")) info.message = error_message->str_or_empty();
    }
    return info;
}

GenerationTask parse_task(const std::string& body) {
    Json root = JsonParser(body).parse();
    const Json* source = &root;
    if (auto data = root.get("data"); data && data->is_object()) source = data;
    if (auto task = root.get("task"); task && task->is_object()) source = task;

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

    if (root.get("error") || source->get("error")) {
        result.error = parse_error_info(source->get("error") ? *source : root);
    }
    return result;
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
                error = parse_error_info(JsonParser(response.body).parse());
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
