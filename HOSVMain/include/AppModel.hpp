#pragma once

#include <string>
#include <vector>
#include <array>
#include <memory>
#include <mutex>
#include <functional>
#include <thread>
#include <filesystem>

namespace fs = std::filesystem;

enum class JobPhase {
    Idle,
    Validating,
    Submitting,
    Polling,
    Downloading,
    Succeeded,
    Failed,
    Cancelling,
    Cancelled
};

inline std::string to_string(JobPhase phase) {
    switch (phase) {
        case JobPhase::Idle: return "Idle";
        case JobPhase::Validating: return "Validating";
        case JobPhase::Submitting: return "Submitting";
        case JobPhase::Polling: return "Polling";
        case JobPhase::Downloading: return "Downloading";
        case JobPhase::Succeeded: return "Succeeded";
        case JobPhase::Failed: return "Failed";
        case JobPhase::Cancelling: return "Cancelling";
        case JobPhase::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct RenderControlsLayout {
    int grid_y = 0;
    int metadata_y = 0;
    int attached_label_x = 0;
    int attached_label_y = 0;
    int attached_value_x = 0;
    int attached_value_y = 0;
    int duration_text_x = 0;
    int duration_text_y = 0;
};

struct Layout {
    int pad = 24;
    int gap = 16;
    int left_w = 0;
    int side_w = 0;
    int side_x = 0;
    int side_y = 96;
    int side_h = 0;

    Rect logo{};
    Rect composition_panel{};
    Rect render_panel{};
    Rect studio_panel{};
    Rect preview{};
    Rect progress{};

    int title_x = 0;
    int title_y = 0;
    int tagline_x = 0;
    int tagline_y = 0;

    int pipeline_label_y = 0;
    int pipeline_status_y = 0;
    int status_label_y = 0;
    int status_text_y = 0;
    int output_label_y = 0;
    int output_text_y = 0;

    RenderControlsLayout render_controls;
};

struct LogoImage {
    std::vector<unsigned char> pixels;
    int width = 0;
    int height = 0;
    bool loaded = false;
    std::string source_path;
};

struct Field {
    std::string label;
    std::string value;
    Rect rect;
    int label_y = 0;
    int cursor = 0;
    int selection_start = -1;
    int selection_end = -1;
    bool multiline = false;
    bool secret = false;
    bool readonly = false;
};

struct Button {
    std::string label;
    Rect rect;
};

struct FileEntry {
    std::string name;
    std::string path;
    bool directory = false;
    Rect rect;
};

struct SceneInput {
    std::string api_key;
    std::string prompt;
    std::string save_path;
    std::string task_id_lookup;
    std::vector<std::string> reference_urls;
    int model_index = 0;
    int resolution_index = 1;
    int aspect_index = 0;
    int duration = 5;
    bool audio = true;
    bool watermark = false;
};

struct AppState {
    int width = 980;
    int height = 846;
    Layout layout;
    LogoImage logo;
    std::vector<Field> fields;
    std::vector<Button> buttons;
    std::vector<FileEntry> file_entries;
    std::vector<std::string> reference_urls;
    int focus = 1;
    bool cursor_blink_on = true;
    int model = 0;
    int resolution = 1;
    int aspect = 0;
    int duration = 5;
    bool audio = true;
    bool watermark = false;
    std::string task_id;
    std::string status = "Ready. Compose a scene, paste remote reference URLs, then render through BytePlus ModelArk.";
    std::string output;
    std::string file_browser_dir;
    int file_page = 0;
    int file_cursor = 0;
    bool file_browser_show_hidden = false;
    bool file_browser_open = false;
    bool busy = false;
    bool should_close = false;
    bool frame_dirty = false;
    bool has_error = false;
    std::function<void()> request_redraw;

    JobPhase phase = JobPhase::Idle;
    std::unique_ptr<std::jthread> worker;
};
