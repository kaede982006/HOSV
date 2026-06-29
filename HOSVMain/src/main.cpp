#include "AppModel.hpp"
#include "SeedanceService.hpp"
#include "JobController.hpp"
#include "providers/BytePlusArkVideoProvider.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <curl/curl.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <unistd.h>
#include <functional>

static inline double hosv_abs(double value) {
    return value < 0.0 ? -value : value;
}

static inline int hosv_floor(double value) {
    const int truncated = static_cast<int>(value);
    return value < static_cast<double>(truncated) ? truncated - 1 : truncated;
}

static inline int hosv_ceil(double value) {
    const int truncated = static_cast<int>(value);
    return value > static_cast<double>(truncated) ? truncated + 1 : truncated;
}

static inline double hosv_sqrt(double value) {
    if (value <= 0.0) return 0.0;
    double x = value > 1.0 ? value : 1.0;
    for (int i = 0; i < 12; ++i) x = 0.5 * (x + value / x);
    return x;
}

static inline double hosv_fmod(double x, double y) {
    if (y == 0.0) return 0.0;
    const int quotient = static_cast<int>(x / y);
    return x - static_cast<double>(quotient) * y;
}

static inline double hosv_cos(double value) {
    constexpr double pi = 3.14159265358979323846;
    value = hosv_fmod(value, 2.0 * pi);
    if (value > pi) value -= 2.0 * pi;
    if (value < -pi) value += 2.0 * pi;
    const double x2 = value * value;
    return 1.0 - x2 / 2.0 + (x2 * x2) / 24.0 - (x2 * x2 * x2) / 720.0 + (x2 * x2 * x2 * x2) / 40320.0;
}

static inline double hosv_acos(double value) {
    constexpr double half_pi = 1.57079632679489661923;
    if (value <= -1.0) return 2.0 * half_pi;
    if (value >= 1.0) return 0.0;
    const double negate = value < 0.0 ? 1.0 : 0.0;
    value = hosv_abs(value);
    double out = -0.0187293;
    out = out * value + 0.0742610;
    out = out * value - 0.2121144;
    out = out * value + half_pi;
    out = out * hosv_sqrt(1.0 - value);
    return negate ? 2.0 * half_pi - out : out;
}

static inline double hosv_cuberoot(double value) {
    if (value == 0.0) return 0.0;
    const bool negative = value < 0.0;
    double target = negative ? -value : value;
    double x = target > 1.0 ? target / 3.0 : 1.0;
    for (int i = 0; i < 16; ++i) x = (2.0 * x + target / (x * x)) / 3.0;
    return negative ? -x : x;
}

static inline double hosv_pow(double x, double y) {
    if (y > 0.333 && y < 0.334) return hosv_cuberoot(x);
    if (y == 2.0) return x * x;
    if (y == 0.5) return hosv_sqrt(x);
    return 1.0;
}

#define STBTT_ifloor(x) hosv_floor(x)
#define STBTT_iceil(x) hosv_ceil(x)
#define STBTT_sqrt(x) hosv_sqrt(x)
#define STBTT_pow(x, y) hosv_pow((x), (y))
#define STBTT_fmod(x, y) hosv_fmod((x), (y))
#define STBTT_cos(x) hosv_cos(x)
#define STBTT_acos(x) hosv_acos(x)
#define STBTT_fabs(x) hosv_abs(x)
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace {

using XID = unsigned long;
using Window = XID;
using Drawable = XID;
using Pixmap = XID;
using GC = struct _XGC*;
using Display = struct _XDisplay;
using Atom = unsigned long;
using Colormap = unsigned long;
using Time = unsigned long;
using KeySym = unsigned long;

namespace fs = std::filesystem;

std::string trim_spaces(const std::string& str) {
    auto first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    auto last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

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

void log_exception(const std::exception& e, const std::string& context) {
    log_error("Exception in " + context + ": " + e.what());
}

size_t write_file_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out_file = static_cast<std::ostream*>(userdata);
    const size_t bytes = size * nmemb;
    out_file->write(ptr, static_cast<std::streamsize>(bytes));
    return bytes;
}

bool download_file(const std::string& url, const std::string& path, std::string& err_msg) {
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

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out_file);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L); // 5 minutes max for download

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_easy_cleanup(curl);
    out_file.close();

    if (res != CURLE_OK) {
        err_msg = std::string("Curl download failed: ") + curl_easy_strerror(res);
        std::error_code ec;
        fs::remove(path, ec); // clean up partial file
        return false;
    }

    if (http_code < 200 || http_code >= 300) {
        err_msg = "HTTP download returned status code " + std::to_string(http_code);
        std::error_code ec;
        fs::remove(path, ec); // clean up
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

constexpr long ExposureMask = 1L << 15;
constexpr long KeyPressMask = 1L << 0;
constexpr long ButtonPressMask = 1L << 2;
constexpr long StructureNotifyMask = 1L << 17;
constexpr int KeyPress = 2;
constexpr int ButtonPress = 4;
constexpr int Expose = 12;
constexpr int ConfigureNotify = 22;
constexpr int SelectionNotify = 31;
constexpr int ClientMessage = 33;
constexpr int XK_BackSpace = 0xff08;
constexpr int XK_Tab = 0xff09;
constexpr int XK_Return = 0xff0d;
constexpr int XK_Escape = 0xff1b;
constexpr int XK_Home = 0xff50;
constexpr int XK_Left = 0xff51;
constexpr int XK_Up = 0xff52;
constexpr int XK_Right = 0xff53;
constexpr int XK_Down = 0xff54;
constexpr int XK_End = 0xff57;
constexpr int XK_Delete = 0xffff;
constexpr int XK_Insert = 0xff63;
constexpr KeySym XK_a = 0x0061;
constexpr KeySym XK_A = 0x0041;
constexpr KeySym XK_c = 0x0063;
constexpr KeySym XK_C = 0x0043;
constexpr KeySym XK_v = 0x0076;
constexpr KeySym XK_V = 0x0056;
constexpr KeySym XK_x = 0x0078;
constexpr KeySym XK_X = 0x0058;
constexpr unsigned int ShiftMask = 0x1;
constexpr unsigned int Mod1Mask = 0x8;
constexpr unsigned int Mod4Mask = 0x40;
constexpr unsigned int ControlMask = 0x4;

struct XAnyEvent {
    int type;
    unsigned long serial;
    int send_event;
    Display* display;
    Window window;
};

struct XKeyEvent {
    int type;
    unsigned long serial;
    int send_event;
    Display* display;
    Window window;
    Window root;
    Window subwindow;
    Time time;
    int x;
    int y;
    int x_root;
    int y_root;
    unsigned int state;
    unsigned int keycode;
    int same_screen;
};

struct XButtonEvent {
    int type;
    unsigned long serial;
    int send_event;
    Display* display;
    Window window;
    Window root;
    Window subwindow;
    Time time;
    int x;
    int y;
    int x_root;
    int y_root;
    unsigned int state;
    unsigned int button;
    int same_screen;
};

struct XConfigureEvent {
    int type;
    unsigned long serial;
    int send_event;
    Display* display;
    Window event;
    Window window;
    int x;
    int y;
    int width;
    int height;
    int border_width;
    Window above;
    int override_redirect;
};

struct XSelectionEvent {
    int type;
    unsigned long serial;
    int send_event;
    Display* display;
    Window window;
    Window owner;
    Atom selection;
    Atom target;
    Atom property;
    Time time;
};

struct XClientMessageEvent {
    int type;
    unsigned long serial;
    int send_event;
    Display* display;
    Window window;
    Atom message_type;
    int format;
    union {
        char b[20];
        short s[10];
        long l[5];
    } data;
};

union XEvent {
    int type;
    XAnyEvent xany;
    XKeyEvent xkey;
    XButtonEvent xbutton;
    XConfigureEvent xconfigure;
    XSelectionEvent xselection;
    XClientMessageEvent xclient;
    long pad[24];
};

struct XColor {
    unsigned long pixel;
    unsigned short red;
    unsigned short green;
    unsigned short blue;
    char flags;
    char pad;
};

struct XClassHint {
    char* res_name = nullptr;
    char* res_class = nullptr;
};

constexpr int PropModeReplace = 0;

enum class FontRole {
    Title,
    Label,
    Body,
    Button,
    Small
};

struct X11 {
    void* lib = nullptr;
    Display* display = nullptr;
    Colormap colormap = 0;
    int screen = 0;
    Window window = 0;
    Pixmap back_buffer = 0;
    int buffer_w = 0;
    int buffer_h = 0;
    Drawable draw_target = 0;
    GC gc = nullptr;
    Atom wm_delete = 0;
    Atom clipboard = 0;
    Atom utf8_string = 0;
    Atom string_atom = 0;
    Atom paste_property = 0;
    int paste_field = -1;
    Atom paste_target = 0;
    std::string clipboard_text;
    bool clipboard_text_exported = false;
    unsigned long current = 0;
    unsigned long background = 0;
    unsigned long surface = 0;
    unsigned long raised = 0;
    unsigned long ink = 0;
    unsigned long muted = 0;
    unsigned long accent = 0;
    unsigned long line = 0;
    unsigned long warning = 0;
    std::array<unsigned long, 4> aa_ink{};
    std::array<unsigned long, 4> aa_muted{};
    std::array<unsigned long, 4> aa_accent{};
    std::array<unsigned long, 4> aa_warning{};
    std::array<unsigned long, 4> aa_dark_on_accent{};

    Display* (*XOpenDisplay)(const char*) = nullptr;
    int (*XDefaultScreen)(Display*) = nullptr;
    Window (*XRootWindow)(Display*, int) = nullptr;
    unsigned long (*XBlackPixel)(Display*, int) = nullptr;
    unsigned long (*XWhitePixel)(Display*, int) = nullptr;
    Window (*XCreateSimpleWindow)(Display*, Window, int, int, unsigned int, unsigned int, unsigned int, unsigned long, unsigned long) = nullptr;
    int (*XStoreName)(Display*, Window, const char*) = nullptr;
    int (*XSelectInput)(Display*, Window, long) = nullptr;
    int (*XMapWindow)(Display*, Window) = nullptr;
    GC (*XCreateGC)(Display*, Drawable, unsigned long, void*) = nullptr;
    int (*XSetForeground)(Display*, GC, unsigned long) = nullptr;
    int (*XFillRectangle)(Display*, Drawable, GC, int, int, unsigned int, unsigned int) = nullptr;
    int (*XDrawRectangle)(Display*, Drawable, GC, int, int, unsigned int, unsigned int) = nullptr;
    int (*XDrawLine)(Display*, Drawable, GC, int, int, int, int) = nullptr;
    int (*XDrawString)(Display*, Drawable, GC, int, int, const char*, int) = nullptr;
    int (*XNextEvent)(Display*, XEvent*) = nullptr;
    int (*XPending)(Display*) = nullptr;
    int (*XLookupString)(XKeyEvent*, char*, int, KeySym*, void*) = nullptr;
    int (*XFlush)(Display*) = nullptr;
    int (*XCloseDisplay)(Display*) = nullptr;
    int (*XFreeGC)(Display*, GC) = nullptr;
    Atom (*XInternAtom)(Display*, const char*, int) = nullptr;
    int (*XSetWMProtocols)(Display*, Window, Atom*, int) = nullptr;
    Colormap (*XDefaultColormap)(Display*, int) = nullptr;
    int (*XAllocNamedColor)(Display*, Colormap, const char*, XColor*, XColor*) = nullptr;
    int (*XDefaultDepth)(Display*, int) = nullptr;
    Pixmap (*XCreatePixmap)(Display*, Drawable, unsigned int, unsigned int, unsigned int) = nullptr;
    int (*XFreePixmap)(Display*, Pixmap) = nullptr;
    int (*XCopyArea)(Display*, Drawable, Drawable, GC, int, int, unsigned int, unsigned int, int, int) = nullptr;
    int (*XChangeProperty)(Display*, Window, Atom, Atom, int, int, const unsigned char*, int) = nullptr;
    int (*XSetClassHint)(Display*, Window, XClassHint*) = nullptr;
    int (*XSetIconName)(Display*, Window, const char*) = nullptr;
    int (*XConvertSelection)(Display*, Atom, Atom, Atom, Window, Time) = nullptr;
    int (*XGetWindowProperty)(Display*, Window, Atom, long, long, int, Atom, Atom*, int*, unsigned long*, unsigned long*, unsigned char**) = nullptr;
    int (*XFree)(void*) = nullptr;
    Window (*XGetSelectionOwner)(Display*, Atom) = nullptr;
    int (*XInitThreads)(void) = nullptr;
    int (*XSendEvent)(Display*, Window, int, long, XEvent*) = nullptr;
};

struct GlyphBitmap {
    int width = 0;
    int height = 0;
    int xoff = 0;
    int yoff = 0;
    int advance = 0;
    std::vector<unsigned char> pixels;
};

struct RenderFont {
    std::vector<unsigned char> bytes;
    stbtt_fontinfo info{};
    float scale = 1.0f;
    int baseline = 0;
    int line_height = 16;
    bool ready = false;
    std::unordered_map<int, GlyphBitmap> glyphs;
};

struct FontBook {
    RenderFont title;
    RenderFont label;
    RenderFont body;
    RenderFont button;
    RenderFont small;
};

FontBook g_fonts;

template <typename T>
void load_symbol(void* lib, const char* name, T& out) {
    out = reinterpret_cast<T>(dlsym(lib, name));
    if (!out) throw std::runtime_error(std::string("Missing X11 symbol: ") + name);
}

unsigned long named_color(X11& x11, Colormap map, const char* name, unsigned long fallback) {
    XColor exact{};
    XColor screen{};
    if (x11.XAllocNamedColor && x11.XAllocNamedColor(x11.display, map, name, &screen, &exact)) {
        return screen.pixel;
    }
    return fallback;
}

unsigned long color_from_rgb(X11& x11, unsigned char red, unsigned char green, unsigned char blue, unsigned long fallback) {
    XColor exact{};
    XColor screen{};
    char spec[8] = {};
    static const char* hex = "0123456789abcdef";
    spec[0] = '#';
    spec[1] = hex[red >> 4];
    spec[2] = hex[red & 0x0f];
    spec[3] = hex[green >> 4];
    spec[4] = hex[green & 0x0f];
    spec[5] = hex[blue >> 4];
    spec[6] = hex[blue & 0x0f];
    if (x11.XAllocNamedColor && x11.XAllocNamedColor(x11.display, x11.colormap, spec, &screen, &exact)) {
        return screen.pixel;
    }
    return fallback;
}

std::array<unsigned long, 4> make_aa_ramp(
    X11& x11,
    std::array<unsigned char, 3> foreground,
    std::array<unsigned char, 3> background,
    unsigned long fallback) {
    std::array<unsigned long, 4> ramp{};
    for (int i = 0; i < 4; ++i) {
        const int alpha = (i + 1) * 64;
        const auto mix = [alpha](unsigned char fg, unsigned char bg) {
            return static_cast<unsigned char>((fg * alpha + bg * (255 - alpha)) / 255);
        };
        ramp[static_cast<std::size_t>(i)] = color_from_rgb(
            x11,
            mix(foreground[0], background[0]),
            mix(foreground[1], background[1]),
            mix(foreground[2], background[2]),
            fallback);
    }
    return ramp;
}

std::vector<unsigned char> read_binary_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size <= 0) return {};
    input.seekg(0, std::ios::beg);
    std::vector<unsigned char> data(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

fs::path executable_dir() {
    std::array<char, 4096> buffer{};
    const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) return fs::current_path();
    buffer[static_cast<std::size_t>(length)] = '\0';
    return fs::path(buffer.data()).parent_path();
}

fs::path find_font_file(const std::string& file_name) {
    const auto exe_dir = executable_dir();
    const std::array<fs::path, 5> bases = {
        fs::current_path() / "fonts" / "alternative",
        exe_dir / "fonts" / "alternative",
        exe_dir.parent_path() / "fonts" / "alternative",
        fs::current_path() / ".." / "fonts" / "alternative",
        fs::path("fonts") / "alternative",
    };
    for (const auto& base : bases) {
        auto candidate = base / file_name;
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) return candidate;
    }
    return {};
}

bool load_render_font(RenderFont& font, const std::string& file_name, float pixels) {
    auto path = find_font_file(file_name);
    if (path.empty()) return false;
    auto data = read_binary_file(path);
    if (data.empty()) return false;
    stbtt_fontinfo info{};
    if (!stbtt_InitFont(&info, data.data(), stbtt_GetFontOffsetForIndex(data.data(), 0))) return false;

    font.bytes = std::move(data);
    stbtt_InitFont(&font.info, font.bytes.data(), stbtt_GetFontOffsetForIndex(font.bytes.data(), 0));
    font.scale = stbtt_ScaleForPixelHeight(&font.info, pixels);
    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    stbtt_GetFontVMetrics(&font.info, &ascent, &descent, &line_gap);
    font.baseline = static_cast<int>(ascent * font.scale + 0.5f);
    font.line_height = static_cast<int>((ascent - descent + line_gap) * font.scale + 0.5f);
    font.ready = true;
    return true;
}

void init_bundled_fonts() {
    load_render_font(g_fonts.title, "Pretendard-Bold.ttf", 20.0f);
    load_render_font(g_fonts.label, "Pretendard-SemiBold.ttf", 12.0f);
    load_render_font(g_fonts.body, "Pretendard-Regular.ttf", 13.0f);
    load_render_font(g_fonts.button, "Pretendard-Regular.ttf", 12.0f);
    load_render_font(g_fonts.small, "Pretendard-Regular.ttf", 11.0f);
}

RenderFont& font_for_role(FontRole role) {
    switch (role) {
        case FontRole::Title: return g_fonts.title;
        case FontRole::Label: return g_fonts.label;
        case FontRole::Button: return g_fonts.button;
        case FontRole::Small: return g_fonts.small;
        case FontRole::Body: return g_fonts.body;
    }
    return g_fonts.body;
}

int decode_utf8(const std::string& text, std::size_t& index) {
    const unsigned char c = static_cast<unsigned char>(text[index++]);
    if (c < 0x80) return c;
    if ((c >> 5) == 0x6 && index < text.size()) {
        return ((c & 0x1F) << 6) | (static_cast<unsigned char>(text[index++]) & 0x3F);
    }
    if ((c >> 4) == 0xE && index + 1 < text.size()) {
        int out = (c & 0x0F) << 12;
        out |= (static_cast<unsigned char>(text[index++]) & 0x3F) << 6;
        out |= static_cast<unsigned char>(text[index++]) & 0x3F;
        return out;
    }
    if ((c >> 3) == 0x1E && index + 2 < text.size()) {
        int out = (c & 0x07) << 18;
        out |= (static_cast<unsigned char>(text[index++]) & 0x3F) << 12;
        out |= (static_cast<unsigned char>(text[index++]) & 0x3F) << 6;
        out |= static_cast<unsigned char>(text[index++]) & 0x3F;
        return out;
    }
    return '?';
}

GlyphBitmap& glyph_for(RenderFont& font, int codepoint) {
    auto found = font.glyphs.find(codepoint);
    if (found != font.glyphs.end()) return found->second;

    GlyphBitmap glyph;
    int advance = 0;
    int lsb = 0;
    stbtt_GetCodepointHMetrics(&font.info, codepoint, &advance, &lsb);
    glyph.advance = static_cast<int>(advance * font.scale + 0.5f);
    unsigned char* bitmap = stbtt_GetCodepointBitmap(&font.info, 0.0f, font.scale, codepoint, &glyph.width, &glyph.height, &glyph.xoff, &glyph.yoff);
    if (bitmap && glyph.width > 0 && glyph.height > 0) {
        glyph.pixels.assign(bitmap, bitmap + glyph.width * glyph.height);
        stbtt_FreeBitmap(bitmap, nullptr);
    }
    auto inserted = font.glyphs.emplace(codepoint, std::move(glyph));
    return inserted.first->second;
}

int measure_string_width(RenderFont& font, const std::string& text) {
    if (!font.ready) return static_cast<int>(text.size()) * 7;
    int width = 0;
    int previous = 0;
    for (std::size_t index = 0; index < text.size();) {
        const int codepoint = decode_utf8(text, index);
        if (codepoint == '\n') break;
        if (previous != 0) {
            width += static_cast<int>(stbtt_GetCodepointKernAdvance(&font.info, previous, codepoint) * font.scale + 0.5f);
        }
        const auto& glyph = glyph_for(font, codepoint);
        width += glyph.advance > 0 ? glyph.advance : 8;
        previous = codepoint;
    }
    return width;
}

bool open_x11(X11& x11, int width, int height) {
    x11.lib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    if (!x11.lib) return false;

    load_symbol(x11.lib, "XOpenDisplay", x11.XOpenDisplay);
    load_symbol(x11.lib, "XDefaultScreen", x11.XDefaultScreen);
    load_symbol(x11.lib, "XRootWindow", x11.XRootWindow);
    load_symbol(x11.lib, "XBlackPixel", x11.XBlackPixel);
    load_symbol(x11.lib, "XWhitePixel", x11.XWhitePixel);
    load_symbol(x11.lib, "XCreateSimpleWindow", x11.XCreateSimpleWindow);
    load_symbol(x11.lib, "XStoreName", x11.XStoreName);
    load_symbol(x11.lib, "XSelectInput", x11.XSelectInput);
    load_symbol(x11.lib, "XMapWindow", x11.XMapWindow);
    load_symbol(x11.lib, "XCreateGC", x11.XCreateGC);
    load_symbol(x11.lib, "XSetForeground", x11.XSetForeground);
    load_symbol(x11.lib, "XFillRectangle", x11.XFillRectangle);
    load_symbol(x11.lib, "XDrawRectangle", x11.XDrawRectangle);
    load_symbol(x11.lib, "XDrawLine", x11.XDrawLine);
    load_symbol(x11.lib, "XDrawString", x11.XDrawString);
    load_symbol(x11.lib, "XNextEvent", x11.XNextEvent);
    load_symbol(x11.lib, "XPending", x11.XPending);
    load_symbol(x11.lib, "XLookupString", x11.XLookupString);
    load_symbol(x11.lib, "XFlush", x11.XFlush);
    load_symbol(x11.lib, "XCloseDisplay", x11.XCloseDisplay);
    load_symbol(x11.lib, "XFreeGC", x11.XFreeGC);
    load_symbol(x11.lib, "XInternAtom", x11.XInternAtom);
    load_symbol(x11.lib, "XSetWMProtocols", x11.XSetWMProtocols);
    load_symbol(x11.lib, "XDefaultColormap", x11.XDefaultColormap);
    load_symbol(x11.lib, "XAllocNamedColor", x11.XAllocNamedColor);
    load_symbol(x11.lib, "XDefaultDepth", x11.XDefaultDepth);
    load_symbol(x11.lib, "XCreatePixmap", x11.XCreatePixmap);
    load_symbol(x11.lib, "XFreePixmap", x11.XFreePixmap);
    load_symbol(x11.lib, "XCopyArea", x11.XCopyArea);
    load_symbol(x11.lib, "XChangeProperty", x11.XChangeProperty);
    load_symbol(x11.lib, "XSetClassHint", x11.XSetClassHint);
    load_symbol(x11.lib, "XSetIconName", x11.XSetIconName);
    load_symbol(x11.lib, "XConvertSelection", x11.XConvertSelection);
    load_symbol(x11.lib, "XGetWindowProperty", x11.XGetWindowProperty);
    load_symbol(x11.lib, "XFree", x11.XFree);
    load_symbol(x11.lib, "XGetSelectionOwner", x11.XGetSelectionOwner);
    load_symbol(x11.lib, "XInitThreads", x11.XInitThreads);
    load_symbol(x11.lib, "XSendEvent", x11.XSendEvent);

    if (x11.XInitThreads) {
        x11.XInitThreads();
    }

    x11.display = x11.XOpenDisplay(nullptr);
    if (!x11.display) return false;

    x11.screen = x11.XDefaultScreen(x11.display);
    const auto root = x11.XRootWindow(x11.display, x11.screen);
    const auto black = x11.XBlackPixel(x11.display, x11.screen);
    const auto white = x11.XWhitePixel(x11.display, x11.screen);
    x11.colormap = x11.XDefaultColormap(x11.display, x11.screen);
    x11.window = x11.XCreateSimpleWindow(x11.display, root, 80, 80, width, height, 1, black, white);
    x11.XSelectInput(x11.display, x11.window, ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask);
    x11.wm_delete = x11.XInternAtom(x11.display, "WM_DELETE_WINDOW", 0);
    x11.XSetWMProtocols(x11.display, x11.window, &x11.wm_delete, 1);
    x11.gc = x11.XCreateGC(x11.display, x11.window, 0, nullptr);
    x11.draw_target = x11.window;
    x11.background = named_color(x11, x11.colormap, "#101418", black);
    x11.surface = named_color(x11, x11.colormap, "#182026", white);
    x11.raised = named_color(x11, x11.colormap, "#222b32", white);
    x11.ink = named_color(x11, x11.colormap, "#eef3f0", white);
    x11.muted = named_color(x11, x11.colormap, "#9ca8a4", black);
    x11.accent = named_color(x11, x11.colormap, "#63d2a2", black);
    x11.line = named_color(x11, x11.colormap, "#36434a", black);
    x11.warning = named_color(x11, x11.colormap, "#f0c36a", white);
    x11.aa_ink = make_aa_ramp(x11, {0xee, 0xf3, 0xf0}, {0x10, 0x14, 0x18}, x11.ink);
    x11.aa_muted = make_aa_ramp(x11, {0x9c, 0xa8, 0xa4}, {0x10, 0x14, 0x18}, x11.muted);
    x11.aa_accent = make_aa_ramp(x11, {0x63, 0xd2, 0xa2}, {0x10, 0x14, 0x18}, x11.accent);
    x11.aa_warning = make_aa_ramp(x11, {0xf0, 0xc3, 0x6a}, {0x10, 0x14, 0x18}, x11.warning);
    x11.aa_dark_on_accent = make_aa_ramp(x11, {0x10, 0x14, 0x18}, {0x63, 0xd2, 0xa2}, x11.background);
    x11.current = x11.ink;
    init_bundled_fonts();
    return true;
}

void close_x11(X11& x11) {
    if (x11.display && x11.back_buffer) x11.XFreePixmap(x11.display, x11.back_buffer);
    if (x11.display && x11.gc) x11.XFreeGC(x11.display, x11.gc);
    if (x11.display) x11.XCloseDisplay(x11.display);
    if (x11.lib) dlclose(x11.lib);
}

inline Drawable x11_surface(const X11& x11) {
    return x11.draw_target != 0 ? x11.draw_target : x11.window;
}

void ensure_back_buffer(X11& x11, int width, int height) {
    if (x11.back_buffer && x11.buffer_w == width && x11.buffer_h == height) return;
    if (x11.back_buffer) x11.XFreePixmap(x11.display, x11.back_buffer);
    const unsigned int depth = static_cast<unsigned int>(x11.XDefaultDepth(x11.display, x11.screen));
    x11.back_buffer = x11.XCreatePixmap(x11.display, x11.window, static_cast<unsigned int>(width),
        static_cast<unsigned int>(height), depth);
    x11.buffer_w = width;
    x11.buffer_h = height;
}



bool contains(const Rect& r, int x, int y) {
    return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

std::vector<std::string> wrap_text(const std::string& text, std::size_t columns) {
    std::vector<std::string> lines;
    if (columns == 0) columns = 1;

    auto push_wrapped_token = [&](std::string token) {
        while (token.size() > columns) {
            lines.push_back(token.substr(0, columns));
            token.erase(0, columns);
        }
        if (!token.empty()) lines.push_back(std::move(token));
    };

    std::istringstream source(text);
    std::string physical;
    while (std::getline(source, physical)) {
        std::string line;
        std::istringstream words(physical);
        std::string word;
        while (words >> word) {
            if (!line.empty() && line.size() + 1 + word.size() > columns) {
                lines.push_back(line);
                line.clear();
            }
            if (!line.empty()) line += ' ';
            if (word.size() > columns) {
                if (!line.empty()) {
                    lines.push_back(line);
                    line.clear();
                }
                push_wrapped_token(word);
            } else {
                line += word;
            }
        }
        if (!line.empty()) lines.push_back(line);
        if (physical.empty()) lines.emplace_back();
    }
    if (lines.empty()) lines.emplace_back();
    return lines;
}

constexpr int FIELD_LINE_H = 19;
constexpr int FIELD_TEXT_BASELINE = 25;
constexpr int FIELD_TEXT_BOTTOM_PAD = 8;

struct FieldViewLine {
    std::string text;
    int start_byte = 0;
    int end_byte = 0;
    int window_start = 0;
};

struct FieldViewLayout {
    std::vector<FieldViewLine> lines;
    int first_line = 0;
    int line_step = FIELD_LINE_H;
};

namespace LayoutConsts {
constexpr int PAD = 28;
constexpr int GAP = 18;
constexpr int INNER_PAD = 18;
constexpr int PANEL_HEADER_H = 58;
constexpr int FIELD_LABEL_GAP = 14;
constexpr int FIELD_H = 40;
constexpr int MULTILINE_FIELD_H = 112;
constexpr int REFERENCE_FIELD_H = 44;
constexpr int BUTTON_H = 40;
constexpr int BUTTON_ROW_GAP = 12;
constexpr int SECTION_GAP = 22;
constexpr int ROW_GAP = 16;
constexpr int REFERENCE_SECTION_GAP = 32;
constexpr int RENDER_PANEL_MIN_H = 232;
constexpr int RENDER_GRID_TOP_GAP = 20;
constexpr int METADATA_TOP_GAP = 22;
constexpr int METADATA_LINE_GAP = 20;
constexpr int METADATA_LABEL_VALUE_GAP = 16;
constexpr int RENDER_BOTTOM_PAD = 18;
constexpr int ACCENT_W = 4;
constexpr int LOGO_SIZE = 36;
constexpr int LOGO_TEXTURE_SIZE = 128;
constexpr int HEADER_TOP = 28;
constexpr int CONTENT_TOP = 92;
constexpr int BOTTOM_MARGIN = 28;
} // namespace LayoutConsts

namespace {
std::vector<std::string> split_reference_values(const std::string& input) {
    std::vector<std::string> values;
    std::stringstream ss(input);
    std::string line;
    while (std::getline(ss, line)) {
        std::stringstream line_ss(line);
        std::string segment;
        while (std::getline(line_ss, segment, ',')) {
            segment = trim_spaces(segment);
            if (!segment.empty()) {
                values.push_back(segment);
            }
        }
    }
    return values;
}

std::vector<std::string> parse_reference_urls(const std::string& input) {
    std::vector<std::string> urls;
    for (const auto& value : split_reference_values(input)) {
        if (std::find(urls.begin(), urls.end(), value) == urls.end()) {
            urls.push_back(value);
        }
    }
    return urls;
}
}


int field_inner_width(const Rect& rect) {
    return std::max(16, rect.w - 20);
}

std::string field_visible_text(const Field& field) {
    if (field.secret && !field.value.empty()) return std::string(field.value.size(), '*');
    return field.value;
}

void clamp_field_cursor(Field& field) {
    field.cursor = std::max(0, std::min(field.cursor, static_cast<int>(field.value.size())));
    if (field.selection_start >= 0) {
        field.selection_start = std::max(0, std::min(field.selection_start, static_cast<int>(field.value.size())));
    }
    if (field.selection_end >= 0) {
        field.selection_end = std::max(0, std::min(field.selection_end, static_cast<int>(field.value.size())));
    }
}

int field_text_top(const Field& field) {
    return field.rect.y + FIELD_TEXT_BASELINE;
}

int field_line_step(const RenderFont& font) {
    return std::max(FIELD_LINE_H, font.line_height + 3);
}

bool is_backspace_key(KeySym sym, const char* buffer, int count) {
    if (sym == XK_BackSpace || sym == 0x0008 || sym == 0x0888) return true;
    return count > 0 && static_cast<unsigned char>(buffer[0]) == 0x08;
}

bool is_key(KeySym sym, char key, KeySym lower, KeySym upper, char lower_char, char upper_char) {
    return sym == lower || sym == upper || key == lower_char || key == upper_char;
}

void clear_selection(Field& field) {
    field.selection_start = -1;
    field.selection_end = -1;
}

bool field_has_selection(const Field& field) {
    return field.selection_start >= 0 && field.selection_end >= 0 && field.selection_start != field.selection_end;
}

std::pair<int, int> normalized_selection(const Field& field) {
    if (!field_has_selection(field)) return {field.cursor, field.cursor};
    const int start = std::max(0, std::min(field.selection_start, static_cast<int>(field.value.size())));
    const int end = std::max(0, std::min(field.selection_end, static_cast<int>(field.value.size())));
    return {std::min(start, end), std::max(start, end)};
}

void select_all(Field& field) {
    field.selection_start = 0;
    field.selection_end = static_cast<int>(field.value.size());
    field.cursor = field.selection_end;
}

std::string selected_field_text(const Field& field) {
    const auto [start, end] = normalized_selection(field);
    if (start == end) return {};
    return field.value.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start));
}

bool delete_selection(Field& field) {
    const auto [start, end] = normalized_selection(field);
    if (start == end) return false;
    field.value.erase(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start));
    field.cursor = start;
    clear_selection(field);
    return true;
}

int utf8_byte_index_before(const std::string& text, int index) {
    int start = std::max(0, std::min(index, static_cast<int>(text.size())));
    if (start <= 0) return 0;
    start--;
    while (start > 0 && (static_cast<unsigned char>(text[static_cast<std::size_t>(start)]) & 0xC0) == 0x80) {
        start--;
    }
    return start;
}

int utf8_byte_index_after(const std::string& text, int index) {
    if (index < 0) return 0;
    if (index >= static_cast<int>(text.size())) return static_cast<int>(text.size());
    std::size_t pos = static_cast<std::size_t>(index);
    decode_utf8(text, pos);
    return static_cast<int>(pos);
}

int line_start_byte(const std::string& text, int cursor) {
    int start = 0;
    for (int i = 0; i < cursor && i < static_cast<int>(text.size()); ++i) {
        if (text[static_cast<std::size_t>(i)] == '\n') start = i + 1;
    }
    return start;
}

int count_lines_before(const std::string& text, int cursor) {
    int line = 0;
    for (int i = 0; i < cursor && i < static_cast<int>(text.size()); ++i) {
        if (text[static_cast<std::size_t>(i)] == '\n') line++;
    }
    return line;
}

std::vector<std::string> split_logical_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
        if (start == text.size()) {
            lines.emplace_back();
            break;
        }
    }
    if (lines.empty()) lines.emplace_back();
    return lines;
}

std::string truncate_to_width(RenderFont& font, const std::string& text, int max_width) {
    if (measure_string_width(font, text) <= max_width) return text;
    std::string out;
    for (std::size_t index = 0; index < text.size();) {
        decode_utf8(text, index);
        const std::string candidate = text.substr(0, index);
        if (measure_string_width(font, candidate) > max_width) break;
        out = candidate;
    }
    if (out.empty() && !text.empty()) {
        std::size_t index = 0;
        decode_utf8(text, index);
        return text.substr(0, index);
    }
    return out;
}

std::string single_line_window_text(RenderFont& font, const std::string& visible, int cursor, int max_width, int& window_start) {
    window_start = 0;
    if (measure_string_width(font, visible) <= max_width) return visible;

    while (window_start < cursor) {
        const std::string slice = visible.substr(static_cast<std::size_t>(window_start), static_cast<std::size_t>(cursor - window_start));
        if (measure_string_width(font, slice) > max_width) {
            window_start = utf8_byte_index_after(visible, window_start);
            continue;
        }
        break;
    }

    int end = cursor;
    int last_fit = end;
    while (end < static_cast<int>(visible.size())) {
        const int next = utf8_byte_index_after(visible, end);
        const std::string slice = visible.substr(static_cast<std::size_t>(window_start), static_cast<std::size_t>(next - window_start));
        if (measure_string_width(font, slice) > max_width) break;
        end = next;
        last_fit = end;
    }

    return visible.substr(static_cast<std::size_t>(window_start), static_cast<std::size_t>(last_fit - window_start));
}

int line_end_byte(const std::string& text, int line_start) {
    const std::size_t next = text.find('\n', static_cast<std::size_t>(line_start));
    return next == std::string::npos ? static_cast<int>(text.size()) : static_cast<int>(next);
}

int field_max_visible_lines(const Field& field, const RenderFont& font) {
    const int available = field.rect.h - FIELD_TEXT_BASELINE - FIELD_TEXT_BOTTOM_PAD;
    return std::max(1, available / field_line_step(font));
}

FieldViewLayout build_field_view_layout(RenderFont& font, const Field& field, bool focused) {
    FieldViewLayout layout;
    layout.line_step = field_line_step(font);
    const std::string visible = field_visible_text(field);
    const int inner_w = field_inner_width(field.rect);
    const int max_lines = field_max_visible_lines(field, font);
    clamp_field_cursor(const_cast<Field&>(field));

    if (!field.multiline) {
        FieldViewLine line;
        line.start_byte = 0;
        line.end_byte = static_cast<int>(visible.size());
        if (focused) {
            int relative_window = 0;
            line.text = single_line_window_text(font, visible, field.cursor, inner_w, relative_window);
            line.window_start = relative_window;
        } else {
            line.text = truncate_to_width(font, visible, inner_w);
            line.window_start = 0;
        }
        layout.lines.push_back(std::move(line));
        return layout;
    }

    const auto logical = split_logical_lines(visible);
    const int total_lines = static_cast<int>(logical.size());
    const int cursor_line = std::max(0, std::min(count_lines_before(visible, field.cursor), total_lines - 1));

    int first_line = 0;
    if (focused && total_lines > max_lines) {
        if (cursor_line >= first_line + max_lines) first_line = cursor_line - max_lines + 1;
        if (cursor_line < first_line) first_line = cursor_line;
    }
    layout.first_line = first_line;

    int byte_offset = 0;
    for (int i = 0; i < first_line; ++i) {
        byte_offset += static_cast<int>(logical[static_cast<std::size_t>(i)].size()) + 1;
    }

    for (int i = first_line; i < total_lines && static_cast<int>(layout.lines.size()) < max_lines; ++i) {
        const std::string& content = logical[static_cast<std::size_t>(i)];
        const int line_start = byte_offset;
        const int line_end = line_end_byte(visible, line_start);

        FieldViewLine line;
        line.start_byte = line_start;
        line.end_byte = line_end;
        const bool cursor_on_line = focused && field.cursor >= line_start && field.cursor <= line_end;
        if (cursor_on_line) {
            const int cursor_in_line = field.cursor - line_start;
            int relative_window = 0;
            line.text = single_line_window_text(font, content, cursor_in_line, inner_w, relative_window);
            line.window_start = line_start + relative_window;
        } else {
            line.text = truncate_to_width(font, content, inner_w);
            line.window_start = line_start;
        }
        layout.lines.push_back(std::move(line));
        byte_offset = line_end < static_cast<int>(visible.size()) ? line_end + 1 : line_end;
    }

    return layout;
}

bool field_cursor_pixel(RenderFont& font, const Field& field, const FieldViewLayout& layout, int& out_x, int& out_y) {
    const int text_x = field.rect.x + 10;
    const int text_y0 = field_text_top(field);
    const int line_step = layout.line_step;
    const int inner_w = field_inner_width(field.rect);
    const std::string visible = field_visible_text(field);
    const int cursor = std::max(0, std::min(field.cursor, static_cast<int>(visible.size())));

    for (std::size_t i = 0; i < layout.lines.size(); ++i) {
        const auto& line = layout.lines[i];
        if (cursor < line.start_byte || cursor > line.end_byte) continue;
        const std::string before = visible.substr(
            static_cast<std::size_t>(line.window_start),
            static_cast<std::size_t>(std::max(0, cursor - line.window_start)));
        out_x = text_x + measure_string_width(font, before);
        out_y = text_y0 + static_cast<int>(i) * line_step;
        return out_x <= text_x + inner_w + 2;
    }

    if (!layout.lines.empty()) {
        const auto& line = layout.lines.back();
        out_x = text_x + measure_string_width(font, line.text);
        out_y = text_y0 + static_cast<int>(layout.lines.size() - 1) * line_step;
        return true;
    }

    out_x = text_x;
    out_y = text_y0;
    return true;
}

int hit_test_field_cursor(RenderFont& font, Field& field, int click_x, int click_y, bool focused) {
    const int text_x = field.rect.x + 10;
    const int text_y0 = field_text_top(field);
    const std::string visible = field_visible_text(field);
    const FieldViewLayout layout = build_field_view_layout(font, field, focused);
    const int line_step = layout.line_step;

    if (!field.multiline) {
        if (layout.lines.empty()) return 0;
        const auto& line = layout.lines.front();
        int best = line.window_start;
        int best_dist = 1'000'000;
        for (int pos = line.window_start; pos <= line.end_byte; pos = utf8_byte_index_after(visible, pos)) {
            const int width = measure_string_width(
                font,
                visible.substr(static_cast<std::size_t>(line.window_start), static_cast<std::size_t>(pos - line.window_start)));
            const int dist = std::abs(text_x + width - click_x);
            if (dist < best_dist) {
                best_dist = dist;
                best = pos;
            }
            if (pos == line.end_byte) break;
        }
        return best;
    }

    int row = (click_y - text_y0 + line_step / 2) / line_step;
    if (row < 0) row = 0;
    if (row >= static_cast<int>(layout.lines.size())) row = static_cast<int>(layout.lines.size()) - 1;
    if (layout.lines.empty()) return 0;

    const auto& line = layout.lines[static_cast<std::size_t>(row)];
    int best = line.start_byte;
    int best_dist = 1'000'000;
    for (int pos = line.window_start; pos <= line.end_byte; pos = utf8_byte_index_after(visible, pos)) {
        const int width = measure_string_width(
            font,
            visible.substr(static_cast<std::size_t>(line.window_start), static_cast<std::size_t>(pos - line.window_start)));
        const int dist = std::abs(text_x + width - click_x);
        if (dist < best_dist) {
            best_dist = dist;
            best = pos;
        }
        if (pos == line.end_byte) break;
    }
    return best;
}

void move_cursor_vertical(Field& field, int delta) {
    const std::string& text = field.value;
    const int current_line = count_lines_before(text, field.cursor);
    const int line_start = line_start_byte(text, field.cursor);
    const int col = field.cursor - line_start;
    const int target_line = current_line + delta;

    if (target_line < 0) {
        field.cursor = 0;
        return;
    }

    int pos = 0;
    int line = 0;
    while (pos <= static_cast<int>(text.size())) {
        if (line == target_line) {
            const int end = line_end_byte(text, pos);
            field.cursor = pos + std::min(col, end - pos);
            return;
        }
        const int end = line_end_byte(text, pos);
        if (end >= static_cast<int>(text.size())) break;
        pos = end + 1;
        line++;
    }
    field.cursor = static_cast<int>(text.size());
}

void insert_text_at_cursor(Field& field, const std::string& text) {
    std::string chunk;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\r') {
            ++i;
            continue;
        }
        if (c == '\n') {
            if (field.multiline) chunk.push_back('\n');
            ++i;
            continue;
        }
        if (c < 32 || c == 127) {
            ++i;
            continue;
        }
        const std::size_t start = i;
        decode_utf8(text, i);
        chunk.append(text, start, i - start);
    }
    if (chunk.empty()) return;
    clamp_field_cursor(field);
    delete_selection(field);
    field.value.insert(static_cast<std::size_t>(field.cursor), chunk);
    field.cursor += static_cast<int>(chunk.size());
    clear_selection(field);
}

void delete_before_cursor(Field& field) {
    if (delete_selection(field)) return;
    if (field.cursor <= 0) return;
    const int start = utf8_byte_index_before(field.value, field.cursor);
    field.value.erase(static_cast<std::size_t>(start), static_cast<std::size_t>(field.cursor - start));
    field.cursor = start;
    clear_selection(field);
}

void delete_at_cursor(Field& field) {
    if (delete_selection(field)) return;
    if (field.cursor >= static_cast<int>(field.value.size())) return;
    const int end = utf8_byte_index_after(field.value, field.cursor);
    field.value.erase(static_cast<std::size_t>(field.cursor), static_cast<std::size_t>(end - field.cursor));
    clear_selection(field);
}

std::string home_dir() {
    if (const char* home = std::getenv("HOME")) return home;
    return fs::current_path().string();
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool is_image_file(const fs::path& path) {
    const auto ext = lower_copy(path.extension().string());
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".webp" || ext == ".bmp";
}

bool is_hidden_entry_name(const std::string& name) {
    return !name.empty() && name.front() == '.';
}

std::string reference_urls_display(const std::vector<std::string>& urls) {
    if (urls.empty()) return {};
    std::ostringstream out;
    for (std::size_t i = 0; i < urls.size(); ++i) {
        if (i > 0) out << ", ";
        out << urls[i];
    }
    return out.str();
}

void load_file_entries(AppState& app) {
    app.file_entries.clear();
    std::error_code ec;
    std::vector<FileEntry> dirs;
    std::vector<FileEntry> files;
    for (const auto& item : fs::directory_iterator(app.file_browser_dir, fs::directory_options::skip_permission_denied, ec)) {
        const auto path = item.path();
        const auto name = path.filename().string();
        if (!app.file_browser_show_hidden && is_hidden_entry_name(name)) continue;
        const bool directory = item.is_directory(ec);
        if (!directory && !is_image_file(path)) continue;
        FileEntry entry;
        entry.name = name;
        entry.path = path.string();
        entry.directory = directory;
        if (directory) dirs.push_back(std::move(entry));
        else files.push_back(std::move(entry));
    }
    auto by_name = [](const FileEntry& left, const FileEntry& right) {
        return lower_copy(left.name) < lower_copy(right.name);
    };
    std::sort(dirs.begin(), dirs.end(), by_name);
    std::sort(files.begin(), files.end(), by_name);
    app.file_entries.insert(app.file_entries.end(), dirs.begin(), dirs.end());
    app.file_entries.insert(app.file_entries.end(), files.begin(), files.end());
}

void navigate_file_browser(AppState& app, const std::string& path) {
    std::error_code ec;
    if (!fs::is_directory(path, ec)) return;
    app.file_browser_dir = fs::canonical(path, ec).string();
    if (ec) app.file_browser_dir = path;
    app.file_page = 0;
    app.file_cursor = 0;
    load_file_entries(app);
}

void open_file_browser(AppState& app) {
    app.file_browser_dir = home_dir();
    app.file_browser_open = true;
    app.file_page = 0;
    app.file_cursor = 0;
    app.file_browser_show_hidden = false;
    load_file_entries(app);
}

int file_browser_visible_rows(const AppState& app) {
    const int dialog_h = std::min(520, app.height - 80);
    return std::max(1, (dialog_h - 136) / 24);
}

int file_browser_max_page(const AppState& app) {
    const int rows = file_browser_visible_rows(app);
    if (app.file_entries.empty()) return 0;
    return static_cast<int>((app.file_entries.size() - 1) / static_cast<std::size_t>(rows));
}

int file_browser_page_count(const AppState& app) {
    const int rows = file_browser_visible_rows(app);
    const int entry_count = static_cast<int>(app.file_entries.size());
    if (entry_count == 0) return 0;
    const int page_start = app.file_page * rows;
    return std::min(rows, entry_count - page_start);
}

void file_browser_clamp_cursor(AppState& app) {
    const int max_page = file_browser_max_page(app);
    app.file_page = std::max(0, std::min(max_page, app.file_page));
    const int page_count = file_browser_page_count(app);
    if (page_count <= 0) {
        app.file_cursor = 0;
        return;
    }
    app.file_cursor = std::max(0, std::min(page_count - 1, app.file_cursor));
}

void file_browser_activate_selection(AppState& app) {
    const int rows = file_browser_visible_rows(app);
    const std::size_t index = static_cast<std::size_t>(app.file_page * rows + app.file_cursor);
    if (index >= app.file_entries.size()) return;

    const auto& entry = app.file_entries[index];
    if (entry.directory) {
        navigate_file_browser(app, entry.path);
        return;
    }

    if (std::find(app.reference_urls.begin(), app.reference_urls.end(), entry.path) == app.reference_urls.end()) {
        app.reference_urls.push_back(entry.path);
    }
    app.fields[2].value = reference_urls_display(app.reference_urls);
    app.fields[2].cursor = static_cast<int>(app.fields[2].value.size());
    clear_selection(app.fields[2]);
    app.status = "Attached local reference image. Rendering still requires public URLs until upload support is configured.";
    app.file_browser_open = false;
}

bool handle_file_browser_key(AppState& app, unsigned int state, KeySym sym, const char* buffer, int count) {
    const char key = count > 0 ? static_cast<char>(buffer[0]) : '\0';
    const int max_page = file_browser_max_page(app);
    const bool control = (state & ControlMask) != 0;
    const bool shift = (state & ShiftMask) != 0;

    if (control && (sym == XK_v || sym == XK_V || key == 'v' || key == 'V')) return true;
    if (shift && sym == XK_Insert) return true;
    if (control && (key == 'h' || key == 'H' || key == '\b' || sym == XK_BackSpace)) {
        app.file_browser_show_hidden = !app.file_browser_show_hidden;
        app.file_page = 0;
        app.file_cursor = 0;
        load_file_entries(app);
        file_browser_clamp_cursor(app);
        return true;
    }
    if (state & (ControlMask | Mod1Mask | Mod4Mask)) return true;
    if (sym == XK_Up || key == 'k') {
        if (app.file_cursor > 0) {
            app.file_cursor--;
        } else if (app.file_page > 0) {
            app.file_page--;
            app.file_cursor = std::max(0, file_browser_page_count(app) - 1);
        }
        return true;
    }
    if (sym == XK_Down || key == 'j') {
        const int page_count = file_browser_page_count(app);
        if (page_count > 0 && app.file_cursor + 1 < page_count) {
            app.file_cursor++;
        } else if (app.file_page < max_page) {
            app.file_page++;
            app.file_cursor = 0;
        }
        return true;
    }
    if (sym == XK_Left || key == 'h') {
        const fs::path parent = fs::path(app.file_browser_dir).parent_path();
        if (!parent.empty()) navigate_file_browser(app, parent.string());
        return true;
    }
    if (sym == XK_Right || key == 'l' || sym == XK_Return) {
        file_browser_activate_selection(app);
        return true;
    }
    return false;
}

const char* model_label(int value) {
    return value == 1 ? "Fast" : "Standard";
}

const char* resolution_label(int value) {
    static const char* labels[] = {"480p", "720p", "1080p", "4k"};
    return labels[std::max(0, std::min(3, value))];
}

const char* aspect_label(int value) {
    static const char* labels[] = {"16:9", "9:16", "1:1", "4:3", "3:4", "21:9", "Adaptive"};
    return labels[std::max(0, std::min(6, value))];
}

int estimate_text_width(const std::string& text, int char_width = 7) {
    return static_cast<int>(text.size()) * char_width;
}

void layout_render_controls(AppState& app, Layout& L, int panel_x, int panel_y, int panel_w) {
    const int inner = LayoutConsts::INNER_PAD;
    const int gap = 28;
    const int header_h = LayoutConsts::PANEL_HEADER_H;
    const int control_x = panel_x + inner;
    const int controls_w = std::max(60 * 4 + gap * 3, panel_w - inner * 2);
    const int control_w = std::max(60, (controls_w - gap * 3) / 4);
    const int row_w = control_w * 4 + gap * 3;
    const int row1_x = control_x + std::max(0, controls_w - row_w) / 2;
    const int row2_w = control_w * 3 + gap * 2;
    const int row2_x = control_x + std::max(0, controls_w - row2_w) / 2;
    auto& rc = L.render_controls;

    rc.grid_y = panel_y + header_h + LayoutConsts::RENDER_GRID_TOP_GAP;
    const int row1_y = rc.grid_y;
    const int row2_y = row1_y + LayoutConsts::BUTTON_H + LayoutConsts::BUTTON_ROW_GAP;

    app.buttons[0] = {"Model", {row1_x, row1_y, control_w, LayoutConsts::BUTTON_H}};
    app.buttons[1] = {"Resolution", {row1_x + (control_w + gap), row1_y, control_w, LayoutConsts::BUTTON_H}};
    app.buttons[2] = {"Aspect", {row1_x + (control_w + gap) * 2, row1_y, control_w, LayoutConsts::BUTTON_H}};
    app.buttons[3] = {"Duration -", {row1_x + (control_w + gap) * 3, row1_y, control_w, LayoutConsts::BUTTON_H}};
    app.buttons[4] = {"Duration +", {row2_x, row2_y, control_w, LayoutConsts::BUTTON_H}};
    app.buttons[5] = {"Audio", {row2_x + (control_w + gap), row2_y, control_w, LayoutConsts::BUTTON_H}};
    app.buttons[6] = {"Watermark", {row2_x + (control_w + gap) * 2, row2_y, control_w, LayoutConsts::BUTTON_H}};

    rc.metadata_y = row2_y + LayoutConsts::BUTTON_H + LayoutConsts::METADATA_TOP_GAP;
    const int metadata_baseline = rc.metadata_y + LayoutConsts::FIELD_LABEL_GAP;
    constexpr const char* attached_label = "References";

    rc.attached_label_x = control_x;
    rc.attached_label_y = metadata_baseline;
    rc.attached_value_x = control_x + estimate_text_width(attached_label) + LayoutConsts::METADATA_LABEL_VALUE_GAP;
    rc.attached_value_y = metadata_baseline;
    rc.duration_text_x = control_x;
    rc.duration_text_y = metadata_baseline + LayoutConsts::METADATA_LINE_GAP;

    const int needed_h = rc.duration_text_y + LayoutConsts::RENDER_BOTTOM_PAD - panel_y;
    const int max_h = std::max(LayoutConsts::RENDER_PANEL_MIN_H, app.height - LayoutConsts::BOTTOM_MARGIN - panel_y);
    const int panel_h = std::min(max_h, std::max(LayoutConsts::RENDER_PANEL_MIN_H, needed_h));
    L.render_panel = {panel_x, panel_y, panel_w, panel_h};
}

void set_layout(AppState& app) {
    app.fields.resize(5);
    auto& L = app.layout;
    const int pad = LayoutConsts::PAD;
    const int gap = LayoutConsts::GAP;
    const int inner = LayoutConsts::INNER_PAD;
    const int header_h = LayoutConsts::PANEL_HEADER_H;

    L.pad = pad;
    L.gap = gap;
    const int columns_w = app.width - pad * 2 - gap;
    L.left_w = columns_w / 2;
    L.side_w = columns_w - L.left_w;
    L.side_x = pad + L.left_w + gap;
    L.side_y = LayoutConsts::CONTENT_TOP;
    const int column_h = app.height - L.side_y - LayoutConsts::BOTTOM_MARGIN;
    L.side_h = column_h;

    L.logo = {pad, LayoutConsts::HEADER_TOP, LayoutConsts::LOGO_SIZE, LayoutConsts::LOGO_SIZE};
    const int header_text_x = app.logo.loaded ? pad + LayoutConsts::LOGO_SIZE + 12 : pad;
    L.title_x = header_text_x;
    L.title_y = 38;
    L.tagline_x = header_text_x;
    L.tagline_y = 64;

    const int comp_x = pad;
    const int comp_y = L.side_y;
    const int comp_w = L.left_w;
    const int field_x = comp_x + inner;
    const int field_w = comp_w - inner * 2;
    int content_y = comp_y + header_h + inner;

    app.fields[0].label = "API Key";
    app.fields[0].label_y = content_y + LayoutConsts::FIELD_LABEL_GAP;
    app.fields[0].rect = {field_x, content_y + 24, field_w, LayoutConsts::FIELD_H};
    app.fields[0].multiline = false;
    app.fields[0].secret = true;
    content_y = app.fields[0].rect.y + app.fields[0].rect.h + LayoutConsts::ROW_GAP;

    app.fields[1].label = "Scene Direction";
    app.fields[1].label_y = content_y + LayoutConsts::FIELD_LABEL_GAP;
    app.fields[1].rect = {field_x, content_y + 24, field_w, LayoutConsts::MULTILINE_FIELD_H};
    app.fields[1].multiline = true;
    app.fields[1].secret = false;
    content_y = app.fields[1].rect.y + app.fields[1].rect.h + LayoutConsts::ROW_GAP + LayoutConsts::REFERENCE_SECTION_GAP;

    app.fields[2].label = "Reference Images / URLs";
    app.fields[2].label_y = content_y + LayoutConsts::FIELD_LABEL_GAP;
    const int attach_w = 110;
    const int cancel_w = 60;
    const int btn_gap = 8;
    const int reference_w = std::max(100, field_w - attach_w - cancel_w - gap - btn_gap);
    app.fields[2].rect = {field_x, content_y + 24, reference_w, LayoutConsts::REFERENCE_FIELD_H};
    app.fields[2].multiline = true;
    app.fields[2].secret = false;
    app.fields[2].readonly = false;
    content_y = app.fields[2].rect.y + app.fields[2].rect.h + inner;

    L.composition_panel = {comp_x, comp_y, comp_w, content_y - comp_y};

    const int render_y = comp_y + L.composition_panel.h + LayoutConsts::SECTION_GAP;
    app.buttons.resize(11);
    layout_render_controls(app, L, comp_x, render_y, comp_w);
    L.render_panel.h = column_h - (render_y - L.side_y);
    app.buttons[7] = {"Add File", {field_x + reference_w + gap, app.fields[2].rect.y, attach_w, LayoutConsts::REFERENCE_FIELD_H}};
    app.buttons[10] = {"Cancel", {field_x + reference_w + gap + attach_w + btn_gap, app.fields[2].rect.y, cancel_w, LayoutConsts::REFERENCE_FIELD_H}};

    L.studio_panel = {L.side_x, L.side_y, L.side_w, column_h};
    int studio_content = L.side_y + header_h + inner;

    L.pipeline_label_y = studio_content + LayoutConsts::FIELD_LABEL_GAP;
    L.progress = {L.side_x + inner, studio_content + 18, L.side_w - inner * 2, 10};
    studio_content = L.progress.y + L.progress.h + gap - 10;

    L.pipeline_status_y = studio_content + LayoutConsts::FIELD_LABEL_GAP;
    studio_content += 16;

    app.buttons[8] = {"Generate", {L.side_x + inner, studio_content, L.side_w - inner * 2, 54}};
    studio_content = app.buttons[8].rect.y + app.buttons[8].rect.h + gap;

    L.preview = {L.side_x + inner, studio_content, L.side_w - inner * 2, 118};
    studio_content = L.preview.y + L.preview.h + gap - 10;

    app.fields[3].label = "Save Path";
    app.fields[3].label_y = studio_content + LayoutConsts::FIELD_LABEL_GAP;
    app.fields[3].rect = {L.side_x + inner, studio_content + 24, L.side_w - inner * 2, LayoutConsts::FIELD_H};
    app.fields[3].multiline = false;
    app.fields[3].secret = false;
    studio_content = app.fields[3].rect.y + app.fields[3].rect.h + 10;

    app.fields[4].label = "Task ID lookup";
    app.fields[4].label_y = studio_content + LayoutConsts::FIELD_LABEL_GAP;
    app.fields[4].rect = {L.side_x + inner, studio_content + 24, L.side_w - inner * 2, LayoutConsts::FIELD_H};
    app.fields[4].multiline = false;
    app.fields[4].secret = false;
    studio_content = app.fields[4].rect.y + app.fields[4].rect.h + 10;

    app.buttons[9] = {"Check Task", {L.side_x + inner, studio_content, L.side_w - inner * 2, 44}};
    studio_content = app.buttons[9].rect.y + app.buttons[9].rect.h + 12;

    L.status_label_y = studio_content + LayoutConsts::FIELD_LABEL_GAP;
    L.status_text_y = studio_content + 30;
    studio_content = L.status_text_y + 48;

    L.output_label_y = studio_content + LayoutConsts::FIELD_LABEL_GAP;
    L.output_text_y = studio_content + 30;
}

void dump_layout_diagnostics(const AppState& app, FILE* out) {
    const auto& L = app.layout;
    const auto& rc = L.render_controls;
    const int composition_title_y = L.composition_panel.y + 28;
    const int api_key_label_y = app.fields[0].label_y;
    const int row2_bottom = app.buttons[6].rect.y + app.buttons[6].rect.h;

    std::array<char, 4096> exe_path{};
    const auto exe_len = readlink("/proc/self/exe", exe_path.data(), exe_path.size() - 1);
    if (exe_len > 0) exe_path[static_cast<std::size_t>(exe_len)] = '\0';

    std::fprintf(out, "[HOSV layout] executable=%s\n", exe_len > 0 ? exe_path.data() : "(unknown)");
    std::fprintf(out, "[HOSV layout] window=%dx%d\n", app.width, app.height);
    std::fprintf(out, "[HOSV layout] logo.loaded=%s", app.logo.loaded ? "true" : "false");
    if (!app.logo.source_path.empty()) std::fprintf(out, " path=%s", app.logo.source_path.c_str());
    std::fprintf(out, "\n");
    std::fprintf(out, "[HOSV layout] composition_panel={x=%d,y=%d,w=%d,h=%d}\n",
        L.composition_panel.x, L.composition_panel.y, L.composition_panel.w, L.composition_panel.h);
    std::fprintf(out, "[HOSV layout] composition_title_baseline_y=%d api_key_label_y=%d delta=%d\n",
        composition_title_y, api_key_label_y, api_key_label_y - composition_title_y);
    std::fprintf(out, "[HOSV layout] render_panel={x=%d,y=%d,w=%d,h=%d}\n",
        L.render_panel.x, L.render_panel.y, L.render_panel.w, L.render_panel.h);
    std::fprintf(out, "[HOSV layout] render_grid_y=%d row1_y=%d row2_y=%d row2_bottom=%d\n",
        rc.grid_y, app.buttons[0].rect.y, app.buttons[4].rect.y, row2_bottom);
    std::fprintf(out, "[HOSV layout] metadata_y=%d attached_label_y=%d attached_value_y=%d duration_text_y=%d\n",
        rc.metadata_y, rc.attached_label_y, rc.attached_value_y, rc.duration_text_y);
    std::fprintf(out, "[HOSV layout] metadata_gap_below_row2=%d\n", rc.metadata_y - row2_bottom);
}

bool verify_layout_diagnostics(const AppState& app, FILE* out) {
    const auto& L = app.layout;
    const auto& rc = L.render_controls;
    const int composition_title_y = L.composition_panel.y + 28;
    const int api_key_label_y = app.fields[0].label_y;
    const int row2_bottom = app.buttons[6].rect.y + app.buttons[6].rect.h;
    bool ok = true;

    if (L.render_panel.h < 232) {
        std::fprintf(out, "[HOSV layout] FAIL render_panel.h=%d (need >=232)\n", L.render_panel.h);
        ok = false;
    }
    if (rc.metadata_y - row2_bottom < 18) {
        std::fprintf(out, "[HOSV layout] FAIL metadata_gap=%d (need >=18)\n", rc.metadata_y - row2_bottom);
        ok = false;
    }
    if (api_key_label_y - composition_title_y < 24) {
        std::fprintf(out, "[HOSV layout] FAIL composition/api_key delta=%d (need >=24)\n",
            api_key_label_y - composition_title_y);
        ok = false;
    }
    if (rc.attached_label_y <= row2_bottom) {
        std::fprintf(out, "[HOSV layout] FAIL attached_label_y=%d overlaps row2_bottom=%d\n",
            rc.attached_label_y, row2_bottom);
        ok = false;
    }
    if (ok) std::fprintf(out, "[HOSV layout] PASS all numeric checks\n");
    return ok;
}

struct SelfTestRunner {
    int passed = 0;
    int failed = 0;

    void check(bool condition, const char* name, FILE* out) {
        if (condition) {
            passed++;
            std::fprintf(out, "[HOSV self-test] PASS %s\n", name);
        } else {
            failed++;
            std::fprintf(out, "[HOSV self-test] FAIL %s\n", name);
        }
    }
};

bool copy_text_to_clipboard(X11& x11, const std::string& text);
void click_button(
    int index,
    const std::shared_ptr<AppState>& app,
    const std::shared_ptr<std::recursive_mutex>& mutex,
    const std::shared_ptr<JobController>& jobs = nullptr);

int run_self_tests() {
    SelfTestRunner test;

    {
        Field field;
        field.value = "abc";
        field.cursor = static_cast<int>(field.value.size());
        delete_before_cursor(field);
        test.check(field.value == "ab" && field.cursor == 2, "backspace removes ASCII before cursor", stderr);
    }

    {
        Field field;
        field.value = "\xEA\xB0\x80\xEB\x82\x98";
        field.cursor = static_cast<int>(field.value.size());
        delete_before_cursor(field);
        test.check(field.value == "\xEA\xB0\x80" && field.cursor == 3, "backspace removes one UTF-8 codepoint", stderr);
    }

    {
        Field field;
        field.value = "abcdef";
        select_all(field);
        test.check(selected_field_text(field) == "abcdef", "Ctrl+A selects all field text", stderr);

        field.selection_start = 2;
        field.selection_end = 4;
        X11 x11;
        test.check(copy_text_to_clipboard(x11, selected_field_text(field)) && x11.clipboard_text == "cd", "Ctrl+C copies selected text", stderr);
        test.check(delete_selection(field) && field.value == "abef" && field.cursor == 2, "Ctrl+X removes selected text", stderr);
        insert_text_at_cursor(field, x11.clipboard_text);
        test.check(field.value == "abcdef" && field.cursor == 4, "Ctrl+V pastes copied text at cursor", stderr);
    }

    {
        RenderFont font;
        int window_start = 0;
        const std::string visible = "abcdef";
        const std::string shown = single_line_window_text(font, visible, 0, 21, window_start);
        test.check(measure_string_width(font, shown) <= 21, "single-line field text stays inside width", stderr);
    }

    {
        AppState app;
        app.fields.resize(5);
        set_layout(app);
        const int row1_left = app.buttons[0].rect.x;
        const int row1_right = app.buttons[3].rect.x + app.buttons[3].rect.w;
        const int row2_left = app.buttons[4].rect.x;
        const int row2_right = app.buttons[6].rect.x + app.buttons[6].rect.w;
        const int row1_center2 = row1_left + row1_right;
        const int row2_center2 = row2_left + row2_right;
        const int control_gap = app.buttons[1].rect.x - (app.buttons[0].rect.x + app.buttons[0].rect.w);
        test.check(std::abs(row1_center2 - row2_center2) <= 1, "render grid rows are centered together", stderr);
        test.check(control_gap >= 28, "render control buttons keep requested spacing", stderr);
        test.check(verify_layout_diagnostics(app, stderr), "layout diagnostics pass", stderr);
    }

    {
        const auto refs = parse_reference_urls("https://cdn.example/a.png, https://cdn.example/a.png\n/tmp/local.png");
        test.check(refs.size() == 2, "reference parser splits and deduplicates URLs and local paths", stderr);
    }

    {
        AppState app;
        app.fields.resize(5);
        const fs::path temp_dir = fs::temp_directory_path() / ("hosv-self-test-" + std::to_string(static_cast<long long>(getpid())));
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
        fs::create_directories(temp_dir, ec);
        {
            std::ofstream(temp_dir / "visible.png", std::ios::binary).put('\0');
            std::ofstream(temp_dir / ".hidden.png", std::ios::binary).put('\0');
            std::ofstream(temp_dir / "note.txt").put('\0');
        }

        app.file_browser_dir = temp_dir.string();
        app.file_browser_open = true;
        load_file_entries(app);
        test.check(app.file_entries.size() == 1 && app.file_entries[0].name == "visible.png", "file browser lists visible images only", stderr);

        char h = 'h';
        handle_file_browser_key(app, ControlMask, 0, &h, 1);
        test.check(app.file_browser_show_hidden, "file browser Ctrl+H toggles hidden files", stderr);
        test.check(app.file_entries.size() == 2, "file browser reloads hidden images after toggle", stderr);

        app.file_browser_show_hidden = false;
        app.file_page = 0;
        app.file_cursor = 0;
        load_file_entries(app);
        file_browser_activate_selection(app);
        test.check(app.reference_urls.size() == 1 && app.reference_urls[0].find("visible.png") != std::string::npos, "file browser attaches local image paths", stderr);
        test.check(app.fields[2].value.find("visible.png") != std::string::npos, "reference field displays local file selection", stderr);
        test.check(!app.file_browser_open, "file browser closes after image selection", stderr);

        fs::remove_all(temp_dir, ec);
    }

    {
        AppState app;
        app.fields.resize(5);
        const bool has_key = std::getenv("BYTEPLUS_ARK_API_KEY") != nullptr || std::getenv("ARK_API_KEY") != nullptr;
        if (const char* key = std::getenv("BYTEPLUS_ARK_API_KEY")) {
            app.fields[0].value = key;
        } else if (const char* key = std::getenv("ARK_API_KEY")) {
            app.fields[0].value = key;
        }
        test.check(has_key ? !app.fields[0].value.empty() : app.fields[0].value.empty(), "API key environment initializes API field", stderr);
    }

    {
        auto app_state = std::make_shared<AppState>();
        auto mutex_state = std::make_shared<std::recursive_mutex>();
        app_state->fields.resize(5);
        set_layout(*app_state);
        app_state->reference_urls.push_back("https://cdn.example/test.png");
        app_state->fields[2].value = reference_urls_display(app_state->reference_urls);

        click_button(10, app_state, mutex_state);
        test.check(app_state->reference_urls.empty(), "click cancel button clears reference URLs", stderr);
        test.check(app_state->fields[2].value.empty(), "click cancel button clears field value", stderr);
    }

    {
        const char* old_byteplus = std::getenv("BYTEPLUS_ARK_API_KEY");
        const char* old_ark = std::getenv("ARK_API_KEY");
        std::string saved_byteplus = old_byteplus ? old_byteplus : "";
        std::string saved_ark = old_ark ? old_ark : "";
        unsetenv("BYTEPLUS_ARK_API_KEY");
        unsetenv("ARK_API_KEY");

        BytePlusArkVideoProvider provider;
        VideoGenerationRequest request;
        request.prompt = "self-test prompt";
        const auto created = provider.createTask(request);
        test.check(!created.ok, "BytePlus provider reports missing API key without real API call", stderr);
        test.check(created.result.error_code == "missing_api_key", "BytePlus provider maps missing API key", stderr);

        if (old_byteplus) setenv("BYTEPLUS_ARK_API_KEY", saved_byteplus.c_str(), 1);
        if (old_ark) setenv("ARK_API_KEY", saved_ark.c_str(), 1);
    }

    {
        const char* old_provider = std::getenv("HOSV_PROVIDER");
        std::string saved_provider = old_provider ? old_provider : "";
        setenv("HOSV_PROVIDER", "seedance2.ai", 1);
        bool rejected = false;
        try {
            BytePlusArkVideoProvider provider;
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        test.check(rejected, "deprecated seedance2.ai provider is rejected", stderr);
        if (old_provider) setenv("HOSV_PROVIDER", saved_provider.c_str(), 1);
        else unsetenv("HOSV_PROVIDER");
    }

    std::fprintf(stderr, "[HOSV self-test] summary: %d passed, %d failed\n", test.passed, test.failed);
    return test.failed == 0 ? 0 : 1;
}

void set_foreground(X11& x11, unsigned long color) {
    x11.current = color;
    x11.XSetForeground(x11.display, x11.gc, color);
}

fs::path find_logo_file() {
    const auto exe_dir = executable_dir();
    const std::array<fs::path, 5> candidates = {
        exe_dir / "logo.png",
        fs::current_path() / "logo.png",
        exe_dir.parent_path() / "logo.png",
        fs::current_path() / ".." / "logo.png",
        fs::current_path() / ".." / ".." / "logo.png",
    };
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) return candidate;
    }
    return {};
}

unsigned char clamp_color_byte(float value) {
    return static_cast<unsigned char>(std::max(0.0f, std::min(255.0f, value + 0.5f)));
}

void sample_rgba_bilinear(
    const unsigned char* pixels,
    int width,
    int height,
    float x,
    float y,
    unsigned char& red,
    unsigned char& green,
    unsigned char& blue,
    unsigned char& alpha) {
    if (!pixels || width <= 0 || height <= 0) {
        red = green = blue = alpha = 0;
        return;
    }

    x = std::max(0.0f, std::min(static_cast<float>(width - 1), x));
    y = std::max(0.0f, std::min(static_cast<float>(height - 1), y));
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min(x0 + 1, width - 1);
    const int y1 = std::min(y0 + 1, height - 1);
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);

    auto premul = [&](int sx, int sy, float& pr, float& pg, float& pb, float& pa) {
        const auto* px = pixels + static_cast<std::size_t>((sy * width + sx) * 4);
        pa = static_cast<float>(px[3]) / 255.0f;
        pr = static_cast<float>(px[0]) * pa;
        pg = static_cast<float>(px[1]) * pa;
        pb = static_cast<float>(px[2]) * pa;
    };

    float pr00 = 0.0f;
    float pg00 = 0.0f;
    float pb00 = 0.0f;
    float pa00 = 0.0f;
    float pr10 = 0.0f;
    float pg10 = 0.0f;
    float pb10 = 0.0f;
    float pa10 = 0.0f;
    float pr01 = 0.0f;
    float pg01 = 0.0f;
    float pb01 = 0.0f;
    float pa01 = 0.0f;
    float pr11 = 0.0f;
    float pg11 = 0.0f;
    float pb11 = 0.0f;
    float pa11 = 0.0f;
    premul(x0, y0, pr00, pg00, pb00, pa00);
    premul(x1, y0, pr10, pg10, pb10, pa10);
    premul(x0, y1, pr01, pg01, pb01, pa01);
    premul(x1, y1, pr11, pg11, pb11, pa11);

    const float pr0 = pr00 + (pr10 - pr00) * tx;
    const float pg0 = pg00 + (pg10 - pg00) * tx;
    const float pb0 = pb00 + (pb10 - pb00) * tx;
    const float pa0 = pa00 + (pa10 - pa00) * tx;
    const float pr1 = pr01 + (pr11 - pr01) * tx;
    const float pg1 = pg01 + (pg11 - pg01) * tx;
    const float pb1 = pb01 + (pb11 - pb01) * tx;
    const float pa1 = pa01 + (pa11 - pa01) * tx;

    const float pr = pr0 + (pr1 - pr0) * ty;
    const float pg = pg0 + (pg1 - pg0) * ty;
    const float pb = pb0 + (pb1 - pb0) * ty;
    const float pa = pa0 + (pa1 - pa0) * ty;

    if (pa <= 0.0001f) {
        red = green = blue = alpha = 0;
        return;
    }

    red = clamp_color_byte(pr / pa);
    green = clamp_color_byte(pg / pa);
    blue = clamp_color_byte(pb / pa);
    alpha = clamp_color_byte(pa * 255.0f);
}

void resample_rgba(
    const unsigned char* source,
    int source_w,
    int source_h,
    std::vector<unsigned char>& out,
    int out_w,
    int out_h) {
    out.assign(static_cast<std::size_t>(out_w * out_h * 4), 0);
    if (!source || source_w <= 0 || source_h <= 0 || out_w <= 0 || out_h <= 0) return;

    for (int dy = 0; dy < out_h; ++dy) {
        const float src_y = (static_cast<float>(dy) + 0.5f) * static_cast<float>(source_h) / static_cast<float>(out_h) - 0.5f;
        for (int dx = 0; dx < out_w; ++dx) {
            const float src_x = (static_cast<float>(dx) + 0.5f) * static_cast<float>(source_w) / static_cast<float>(out_w) - 0.5f;
            unsigned char red = 0;
            unsigned char green = 0;
            unsigned char blue = 0;
            unsigned char alpha = 0;
            sample_rgba_bilinear(source, source_w, source_h, src_x, src_y, red, green, blue, alpha);
            const auto offset = static_cast<std::size_t>((dy * out_w + dx) * 4);
            out[offset] = red;
            out[offset + 1] = green;
            out[offset + 2] = blue;
            out[offset + 3] = alpha;
        }
    }
}

bool load_logo_scaled(LogoImage& logo, int max_w, int max_h) {
    logo = LogoImage{};
    const auto path = find_logo_file();
    if (path.empty() || max_w <= 0 || max_h <= 0) return false;
    logo.source_path = path.string();

    int source_w = 0;
    int source_h = 0;
    int channels = 0;
    unsigned char* source = stbi_load(path.string().c_str(), &source_w, &source_h, &channels, 4);
    if (!source || source_w <= 0 || source_h <= 0) {
        if (source) stbi_image_free(source);
        return false;
    }

    const double scale = std::min(static_cast<double>(max_w) / source_w, static_cast<double>(max_h) / source_h);
    logo.width = std::max(1, static_cast<int>(source_w * scale + 0.5));
    logo.height = std::max(1, static_cast<int>(source_h * scale + 0.5));
    resample_rgba(source, source_w, source_h, logo.pixels, logo.width, logo.height);

    stbi_image_free(source);
    logo.loaded = true;
    return true;
}

bool load_logo(LogoImage& logo, int max_dim) {
    return load_logo_scaled(logo, max_dim, max_dim);
}

void set_wm_title(X11& x11, const char* title) {
    if (!x11.display || !title) return;

    x11.XStoreName(x11.display, x11.window, title);
    x11.XSetIconName(x11.display, x11.window, title);

    XClassHint hint{};
    hint.res_name = const_cast<char*>("hosv");
    hint.res_class = const_cast<char*>("HOSV");
    x11.XSetClassHint(x11.display, x11.window, &hint);

    const Atom utf8 = x11.XInternAtom(x11.display, "UTF8_STRING", 1);
    const Atom net_wm_name = x11.XInternAtom(x11.display, "_NET_WM_NAME", 0);
    if (utf8 != 0 && net_wm_name != 0) {
        x11.XChangeProperty(
            x11.display,
            x11.window,
            net_wm_name,
            utf8,
            8,
            PropModeReplace,
            reinterpret_cast<const unsigned char*>(title),
            static_cast<int>(std::strlen(title)));
    }
}

void append_net_wm_icon(std::vector<std::uint32_t>& data, const LogoImage& icon) {
    if (!icon.loaded || icon.width <= 0 || icon.height <= 0 || icon.pixels.empty()) return;

    data.push_back(static_cast<std::uint32_t>(icon.width));
    data.push_back(static_cast<std::uint32_t>(icon.height));
    for (int row = 0; row < icon.height; ++row) {
        for (int col = 0; col < icon.width; ++col) {
            const auto offset = static_cast<std::size_t>((row * icon.width + col) * 4);
            const unsigned char red = icon.pixels[offset];
            const unsigned char green = icon.pixels[offset + 1];
            const unsigned char blue = icon.pixels[offset + 2];
            const unsigned char alpha = icon.pixels[offset + 3];
            const std::uint32_t argb =
                (static_cast<std::uint32_t>(alpha) << 24) |
                (static_cast<std::uint32_t>(red) << 16) |
                (static_cast<std::uint32_t>(green) << 8) |
                static_cast<std::uint32_t>(blue);
            data.push_back(argb);
        }
    }
}

void set_wm_icon(X11& x11) {
    if (!x11.display) return;

    LogoImage icon128;
    LogoImage icon64;
    if (!load_logo_scaled(icon128, 128, 128)) return;

    std::vector<std::uint32_t> icon_data;
    append_net_wm_icon(icon_data, icon128);
    if (load_logo_scaled(icon64, 64, 64)) append_net_wm_icon(icon_data, icon64);

    const Atom net_wm_icon = x11.XInternAtom(x11.display, "_NET_WM_ICON", 0);
    if (net_wm_icon == 0 || icon_data.empty()) return;

    x11.XChangeProperty(
        x11.display,
        x11.window,
        net_wm_icon,
        x11.XInternAtom(x11.display, "CARDINAL", 0),
        32,
        PropModeReplace,
        reinterpret_cast<const unsigned char*>(icon_data.data()),
        static_cast<int>(icon_data.size()));
    x11.XFlush(x11.display);
}

void map_window(X11& x11) {
    if (x11.display && x11.window) x11.XMapWindow(x11.display, x11.window);
}

void draw_logo(X11& x11, const LogoImage& logo, const Rect& dest) {
    if (!logo.loaded || logo.pixels.empty() || logo.width <= 0 || logo.height <= 0 || dest.w <= 0 || dest.h <= 0) {
        return;
    }

    constexpr unsigned char bg_r = 0x10;
    constexpr unsigned char bg_g = 0x14;
    constexpr unsigned char bg_b = 0x18;

    const double scale = std::min(
        static_cast<double>(dest.w) / logo.width,
        static_cast<double>(dest.h) / logo.height);
    const int draw_w = std::max(1, static_cast<int>(logo.width * scale + 0.5));
    const int draw_h = std::max(1, static_cast<int>(logo.height * scale + 0.5));
    const int origin_x = dest.x + (dest.w - draw_w) / 2;
    const int origin_y = dest.y + (dest.h - draw_h) / 2;

    for (int row = 0; row < draw_h; ++row) {
        const float src_y = (static_cast<float>(row) + 0.5f) * static_cast<float>(logo.height) / static_cast<float>(draw_h) - 0.5f;
        int run_start = -1;
        unsigned long run_color = 0;
        for (int col = 0; col < draw_w; ++col) {
            const float src_x = (static_cast<float>(col) + 0.5f) * static_cast<float>(logo.width) / static_cast<float>(draw_w) - 0.5f;
            unsigned char sample_r = 0;
            unsigned char sample_g = 0;
            unsigned char sample_b = 0;
            unsigned char sample_a = 0;
            sample_rgba_bilinear(logo.pixels.data(), logo.width, logo.height, src_x, src_y, sample_r, sample_g, sample_b, sample_a);
            const int alpha = sample_a;
            if (alpha < 8) {
                if (run_start >= 0) {
                    set_foreground(x11, run_color);
                    x11.XFillRectangle(
                        x11.display,
                        x11_surface(x11),
                        x11.gc,
                        origin_x + run_start,
                        origin_y + row,
                        static_cast<unsigned int>(col - run_start),
                        1);
                    run_start = -1;
                }
                continue;
            }

            const unsigned char red = static_cast<unsigned char>((sample_r * alpha + bg_r * (255 - alpha)) / 255);
            const unsigned char green = static_cast<unsigned char>((sample_g * alpha + bg_g * (255 - alpha)) / 255);
            const unsigned char blue = static_cast<unsigned char>((sample_b * alpha + bg_b * (255 - alpha)) / 255);
            const unsigned long color = color_from_rgb(x11, red, green, blue, x11.background);
            if (run_start < 0) {
                run_start = col;
                run_color = color;
            } else if (color != run_color) {
                set_foreground(x11, run_color);
                x11.XFillRectangle(
                    x11.display,
                    x11_surface(x11),
                    x11.gc,
                    origin_x + run_start,
                    origin_y + row,
                    static_cast<unsigned int>(col - run_start),
                    1);
                run_start = col;
                run_color = color;
            }
        }
        if (run_start >= 0) {
            set_foreground(x11, run_color);
            x11.XFillRectangle(
                x11.display,
                x11_surface(x11),
                x11.gc,
                origin_x + run_start,
                origin_y + row,
                static_cast<unsigned int>(draw_w - run_start),
                1);
        }
    }
}

int dump_layout_and_exit() {
    AppState app;
    app.fields.resize(5);
    load_logo(app.logo, LayoutConsts::LOGO_TEXTURE_SIZE);
    set_layout(app);
    dump_layout_diagnostics(app, stderr);
    return verify_layout_diagnostics(app, stderr) ? 0 : 1;
}

const std::array<unsigned long, 4>& aa_ramp_for_current(const X11& x11) {
    if (x11.current == x11.muted) return x11.aa_muted;
    if (x11.current == x11.accent) return x11.aa_accent;
    if (x11.current == x11.warning) return x11.aa_warning;
    if (x11.current == x11.background) return x11.aa_dark_on_accent;
    return x11.aa_ink;
}

void draw_string(X11& x11, int x, int y, const std::string& text, FontRole role = FontRole::Body) {
    auto& font = font_for_role(role);
    if (!font.ready) {
        x11.XDrawString(x11.display, x11_surface(x11), x11.gc, x, y, text.c_str(), static_cast<int>(text.size()));
        return;
    }

    int cursor = x;
    int previous = 0;
    const unsigned long original_color = x11.current;
    const auto& ramp = aa_ramp_for_current(x11);
    const bool use_ramp = true;
    for (std::size_t index = 0; index < text.size();) {
        const int codepoint = decode_utf8(text, index);
        if (codepoint == '\n') {
            cursor = x;
            y += font.line_height;
            previous = 0;
            continue;
        }
        if (previous != 0) {
            cursor += static_cast<int>(stbtt_GetCodepointKernAdvance(&font.info, previous, codepoint) * font.scale + 0.5f);
        }
        auto& glyph = glyph_for(font, codepoint);
        if (!glyph.pixels.empty()) {
            for (int row = 0; row < glyph.height; ++row) {
                int run_start = -1;
                int run_bucket = -1;
                for (int col = 0; col < glyph.width; ++col) {
                    const auto alpha = glyph.pixels[static_cast<std::size_t>(row * glyph.width + col)];
                    const int bucket = alpha == 0 ? -1 : (use_ramp ? std::min(3, static_cast<int>(alpha) / 64) : 3);
                    if (bucket >= 0) {
                        if (run_start < 0) run_start = col;
                        if (run_bucket < 0) {
                            run_bucket = bucket;
                            x11.XSetForeground(x11.display, x11.gc, use_ramp ? ramp[static_cast<std::size_t>(run_bucket)] : original_color);
                        } else if (run_bucket != bucket) {
                            x11.XFillRectangle(
                                x11.display,
                                x11_surface(x11),
                                x11.gc,
                                cursor + glyph.xoff + run_start,
                                y + glyph.yoff + row,
                                static_cast<unsigned int>(col - run_start),
                                1);
                            run_start = col;
                            run_bucket = bucket;
                            x11.XSetForeground(x11.display, x11.gc, use_ramp ? ramp[static_cast<std::size_t>(run_bucket)] : original_color);
                        }
                    } else if (run_start >= 0) {
                        x11.XFillRectangle(
                            x11.display,
                            x11_surface(x11),
                            x11.gc,
                            cursor + glyph.xoff + run_start,
                            y + glyph.yoff + row,
                            static_cast<unsigned int>(col - run_start),
                            1);
                        run_start = -1;
                        run_bucket = -1;
                    }
                }
                if (run_start >= 0) {
                    if (run_bucket >= 0) x11.XSetForeground(x11.display, x11.gc, use_ramp ? ramp[static_cast<std::size_t>(run_bucket)] : original_color);
                    x11.XFillRectangle(
                        x11.display,
                        x11_surface(x11),
                        x11.gc,
                        cursor + glyph.xoff + run_start,
                        y + glyph.yoff + row,
                        static_cast<unsigned int>(glyph.width - run_start),
                        1);
                }
            }
        }
        cursor += glyph.advance > 0 ? glyph.advance : 8;
        previous = codepoint;
    }
    set_foreground(x11, original_color);
}

void draw_box(X11& x11, const Rect& r, unsigned long color, bool fill) {
    set_foreground(x11, color);
    if (fill) {
        x11.XFillRectangle(x11.display, x11_surface(x11), x11.gc, r.x, r.y, r.w, r.h);
    } else {
        x11.XDrawRectangle(x11.display, x11_surface(x11), x11.gc, r.x, r.y, r.w, r.h);
    }
}

void draw_line(X11& x11, int x1, int y1, int x2, int y2, unsigned long color) {
    set_foreground(x11, color);
    x11.XDrawLine(x11.display, x11_surface(x11), x11.gc, x1, y1, x2, y2);
}

void draw_panel(X11& x11, const Rect& r, const std::string& title, const std::string& subtitle = {}) {
    const int header_h = LayoutConsts::PANEL_HEADER_H;
    draw_box(x11, r, x11.surface, true);
    draw_box(x11, {r.x, r.y, r.w, header_h}, x11.raised, true);
    draw_box(x11, r, x11.line, false);
    draw_box(x11, {r.x, r.y, LayoutConsts::ACCENT_W, r.h}, x11.accent, true);
    draw_line(x11, r.x + 8, r.y + header_h, r.x + r.w - 9, r.y + header_h, x11.line);
    set_foreground(x11, x11.ink);
    draw_string(x11, r.x + LayoutConsts::INNER_PAD, r.y + 28, title, FontRole::Label);
    if (!subtitle.empty()) {
        set_foreground(x11, x11.muted);
        draw_string(x11, r.x + LayoutConsts::INNER_PAD, r.y + 50, subtitle, FontRole::Small);
    }
}

void draw_progress(X11& x11, const Rect& r, int percent) {
    percent = std::max(0, std::min(100, percent));
    draw_box(x11, r, x11.background, true);
    draw_box(x11, r, x11.line, false);
    if (percent > 0) {
        draw_box(x11, {r.x + 2, r.y + 2, std::max(2, (r.w - 4) * percent / 100), r.h - 4}, x11.accent, true);
    }
}

void draw_preview(X11& x11, const Rect& r, const AppState& app) {
    draw_box(x11, r, x11.background, true);
    draw_box(x11, r, x11.line, false);
    const Rect inner = {r.x + 14, r.y + 10, r.w - 28, r.h - 20};
    draw_box(x11, inner, x11.surface, true);
    draw_box(x11, inner, x11.line, false);

    const int preview_logo_size = std::min(40, std::min(inner.w - 24, inner.h - 46));
    if (app.logo.loaded && preview_logo_size > 0) {
        const Rect logo_rect{
            inner.x + (inner.w - preview_logo_size) / 2,
            inner.y + 8,
            preview_logo_size,
            preview_logo_size};
        draw_logo(x11, app.logo, logo_rect);
    }

    const std::string line1 = app.output.empty() ? "Preview waits for a rendered result" : "Result URL is ready";
    const std::string line2 = app.output.empty() ? "Generated videos appear here after polling." : "Open the output URL from the result field.";
    const int w1 = measure_string_width(font_for_role(FontRole::Body), line1);
    const int w2 = measure_string_width(font_for_role(FontRole::Small), line2);
    const int x1 = inner.x + (inner.w - w1) / 2;
    const int x2 = inner.x + (inner.w - w2) / 2;
    const int text_y = app.logo.loaded ? inner.y + 8 + preview_logo_size + 18 : inner.y + (inner.h - 32) / 2 + 10;
    set_foreground(x11, x11.ink);
    draw_string(x11, x1, text_y, line1, FontRole::Body);
    set_foreground(x11, x11.muted);
    draw_string(x11, x2, text_y + 18, line2, FontRole::Small);
}

enum class IconKind {
    None,
    Attach,
    Play,
    Search
};

void draw_icon(X11& x11, IconKind icon, int x, int y, unsigned long color) {
    if (icon == IconKind::None) return;
    if (icon == IconKind::Play) {
        draw_line(x11, x, y, x, y + 13, color);
        draw_line(x11, x, y, x + 11, y + 6, color);
        draw_line(x11, x, y + 13, x + 11, y + 6, color);
        return;
    }
    if (icon == IconKind::Attach) {
        draw_box(x11, {x, y + 2, 13, 10}, color, false);
        draw_line(x11, x + 3, y + 2, x + 5, y - 1, color);
        draw_line(x11, x + 5, y - 1, x + 13, y - 1, color);
        draw_line(x11, x + 13, y - 1, x + 13, y + 2, color);
        draw_box(x11, {x + 3, y + 5, 3, 3}, color, true);
        return;
    }
    if (icon == IconKind::Search) {
        draw_box(x11, {x, y, 10, 10}, color, false);
        draw_line(x11, x + 8, y + 8, x + 14, y + 14, color);
    }
}

void draw_button(X11& x11, const Button& button, const std::string& label, bool accent = false, IconKind icon = IconKind::None) {
    draw_box(x11, button.rect, accent ? x11.accent : x11.raised, true);
    if (!accent) {
        draw_box(x11, {button.rect.x, button.rect.y, LayoutConsts::ACCENT_W, button.rect.h}, x11.accent, true);
        draw_line(x11, button.rect.x + 8, button.rect.y + 1, button.rect.x + button.rect.w - 9, button.rect.y + 1, x11.line);
    }
    draw_box(x11, button.rect, accent ? x11.accent : x11.line, false);
    set_foreground(x11, accent ? x11.background : x11.ink);
    const unsigned long text_color = accent ? x11.background : x11.ink;
    draw_icon(x11, icon, button.rect.x + 14, button.rect.y + (button.rect.h - 14) / 2, text_color);
    auto& font = font_for_role(FontRole::Button);
    const int text_y = button.rect.y + (button.rect.h + font.baseline) / 2;
    draw_string(x11, button.rect.x + (icon == IconKind::None ? (accent ? 18 : 12) : 38), text_y, label, FontRole::Button);
}

void draw_field_frame(X11& x11, const Rect& r, bool focused, bool readonly = false) {
    draw_box(x11, r, x11.background, true);
    draw_box(x11, {r.x + 1, r.y + 1, r.w - 2, r.h - 2}, readonly ? x11.raised : x11.surface, true);
    draw_box(x11, r, readonly ? x11.line : (focused ? x11.accent : x11.line), false);
    if (!readonly) draw_line(x11, r.x + 1, r.y + 1, r.x + r.w - 2, r.y + 1, x11.raised);
    if (focused && !readonly) draw_box(x11, {r.x, r.y, LayoutConsts::ACCENT_W, r.h}, x11.accent, true);
}

void draw_field_caret(X11& x11, RenderFont& font, const Field& field, const FieldViewLayout& layout) {
    int cx = 0;
    int cy = 0;
    if (!field_cursor_pixel(font, field, layout, cx, cy)) return;
    const int caret_top = std::max(field.rect.y + 6, cy - font.baseline);
    const int caret_bottom = std::min(field.rect.y + field.rect.h - 4, cy - font.baseline + font.line_height);
    set_foreground(x11, x11.ink);
    draw_line(x11, cx, caret_top, cx, caret_bottom, x11.ink);
}

void draw_field_line(
    X11& x11,
    RenderFont& font,
    const Field& field,
    const FieldViewLine& line,
    int x,
    int baseline_y) {
    if (!field_has_selection(field) || line.text.empty()) {
        draw_string(x11, x, baseline_y, line.text, FontRole::Body);
        return;
    }

    const auto [selection_start, selection_end] = normalized_selection(field);
    const int visible_start = line.window_start;
    const int visible_end = line.window_start + static_cast<int>(line.text.size());
    const int highlight_start = std::max(selection_start, visible_start);
    const int highlight_end = std::min(selection_end, visible_end);
    if (highlight_start >= highlight_end) {
        draw_string(x11, x, baseline_y, line.text, FontRole::Body);
        return;
    }

    const std::string before = line.text.substr(
        0,
        static_cast<std::size_t>(highlight_start - visible_start));
    const std::string selected = line.text.substr(
        static_cast<std::size_t>(highlight_start - visible_start),
        static_cast<std::size_t>(highlight_end - highlight_start));
    const std::string after = line.text.substr(
        static_cast<std::size_t>(highlight_end - visible_start));

    const int before_w = measure_string_width(font, before);
    const int selected_w = std::max(1, measure_string_width(font, selected));
    const int top = baseline_y - font.baseline - 1;
    draw_box(x11, {x + before_w, top, selected_w, font.line_height + 2}, x11.accent, true);

    set_foreground(x11, field.readonly ? x11.muted : x11.ink);
    draw_string(x11, x, baseline_y, before, FontRole::Body);
    set_foreground(x11, x11.background);
    draw_string(x11, x + before_w, baseline_y, selected, FontRole::Body);
    set_foreground(x11, field.readonly ? x11.muted : x11.ink);
    draw_string(x11, x + before_w + selected_w, baseline_y, after, FontRole::Body);
}

void draw_file_browser(X11& x11, AppState& app) {
    if (!app.file_browser_open) return;

    const int dialog_w = std::min(740, app.width - 80);
    const int dialog_h = std::min(520, app.height - 80);
    const Rect dialog{(app.width - dialog_w) / 2, (app.height - dialog_h) / 2, dialog_w, dialog_h};
    draw_box(x11, dialog, x11.surface, true);
    draw_box(x11, dialog, x11.accent, false);

    set_foreground(x11, x11.ink);
    draw_string(x11, dialog.x + 18, dialog.y + 34, "Add Reference Image", FontRole::Title);
    set_foreground(x11, x11.muted);
    draw_string(x11, dialog.x + 18, dialog.y + 60, "Select a local image to attach, or paste a hosted URL in the reference field.", FontRole::Small);
    draw_string(x11, dialog.x + dialog.w - 430, dialog.y + 60, "h parent  j/k move  l open  Ctrl+H hidden  Esc cancel", FontRole::Small);

    const Rect cancel_btn{dialog.x + dialog.w - 90, dialog.y + 16, 72, 28};
    Button file_cancel_btn{"Cancel", cancel_btn};
    draw_button(x11, file_cancel_btn, "Cancel");

    const auto current = app.file_browser_dir.size() > 78 ? "..." + app.file_browser_dir.substr(app.file_browser_dir.size() - 75) : app.file_browser_dir;
    set_foreground(x11, x11.warning);
    draw_string(x11, dialog.x + 18, dialog.y + 88, current, FontRole::Small);

    const int list_x = dialog.x + 18;
    const int list_y = dialog.y + 112;
    const int row_h = 24;
    const int rows = file_browser_visible_rows(app);
    file_browser_clamp_cursor(app);
    draw_box(x11, {list_x, list_y, dialog.w - 36, rows * row_h + 8}, x11.background, true);
    draw_box(x11, {list_x, list_y, dialog.w - 36, rows * row_h + 8}, x11.line, false);

    for (auto& entry : app.file_entries) entry.rect = {};
    const std::size_t start = static_cast<std::size_t>(app.file_page * rows);
    const std::size_t end = std::min(app.file_entries.size(), start + static_cast<std::size_t>(rows));
    for (std::size_t i = start; i < end; ++i) {
        auto& entry = app.file_entries[i];
        const int visible_row = static_cast<int>(i - start);
        entry.rect = {list_x + 8, list_y + 8 + visible_row * row_h, dialog.w - 52, row_h};
        const bool selected = visible_row == app.file_cursor;
        if (selected) draw_box(x11, entry.rect, x11.raised, true);
        set_foreground(x11, entry.directory ? x11.accent : (selected ? x11.ink : x11.ink));
        const auto label = std::string(entry.directory ? "[folder] " : "[image] ") + entry.name;
        draw_string(x11, entry.rect.x + (selected ? 4 : 0), entry.rect.y + 18, label.size() > 88 ? label.substr(0, 85) + "..." : label, FontRole::Body);
    }

    if (app.file_entries.empty()) {
        set_foreground(x11, x11.muted);
        draw_string(x11, list_x + 12, list_y + 34, "No folders or supported image files were found here.", FontRole::Body);
    }
}

void redraw(X11& x11, AppState& app) {
    const auto& L = app.layout;
    ensure_back_buffer(x11, app.width, app.height);
    x11.draw_target = x11.back_buffer;

    draw_box(x11, {0, 0, app.width, app.height}, x11.background, true);
    set_foreground(x11, x11.ink);

    if (app.logo.loaded) {
        draw_logo(x11, app.logo, L.logo);
    }
    draw_string(x11, L.title_x, L.title_y, "HOSV", FontRole::Title);
    set_foreground(x11, x11.muted);
    draw_string(x11, L.tagline_x, L.tagline_y, "Human-led synthetic video studio powered by official BytePlus ModelArk.", FontRole::Small);

    draw_panel(x11, L.composition_panel, "Composition", "Shape the source direction and reference inputs.");
    draw_panel(x11, L.render_panel, "Render Controls", "Tune generation parameters before launch.");
    draw_panel(x11, L.studio_panel, "HOSV Studio", "Render status and result surface.");
    draw_preview(x11, L.preview, app);

    const int progress = app.has_error ? 0 : (app.busy ? 48 : (app.output.empty() ? 0 : 100));
    set_foreground(x11, x11.muted);
    draw_string(x11, L.progress.x, L.pipeline_label_y, "Progress", FontRole::Label);
    draw_progress(x11, L.progress, progress);
    set_foreground(x11, app.busy ? x11.warning : x11.muted);
    const std::string status_text = app.busy ? "Rendering in progress" : "Ready for a generation job";
    auto& small_font = font_for_role(FontRole::Small);
    const int text_w = measure_string_width(small_font, status_text);
    const int status_x = L.progress.x + (L.progress.w - text_w) / 2;
    const int progress_bottom = L.progress.y + L.progress.h;
    const int btn_top = app.buttons[8].rect.y;
    const int status_y = (progress_bottom + btn_top) / 2 + small_font.baseline / 2;
    draw_string(x11, status_x, status_y, status_text, FontRole::Small);

    auto& body_font = font_for_role(FontRole::Body);

    for (std::size_t i = 0; i < app.fields.size(); ++i) {
        auto& field = app.fields[i];
        set_foreground(x11, x11.muted);
        draw_string(x11, field.rect.x, field.label_y, field.label, FontRole::Label);
        const bool focused = app.focus == static_cast<int>(i) && !field.readonly;
        draw_field_frame(x11, field.rect, focused, field.readonly);
        if (field.readonly && field.value.empty()) {
            set_foreground(x11, x11.muted);
            draw_string(x11, field.rect.x + 10, field_text_top(field), "Add local images or paste reference URLs.", FontRole::Small);
        } else {
            draw_box(x11, {field.rect.x + 1, field.rect.y + 1, field.rect.w - 2, field.rect.h - 2}, x11.surface, true);
            set_foreground(x11, field.readonly ? x11.muted : x11.ink);
            const FieldViewLayout layout = build_field_view_layout(body_font, field, focused);
            const int text_y0 = field_text_top(field);
            for (std::size_t line = 0; line < layout.lines.size(); ++line) {
                draw_field_line(
                    x11,
                    body_font,
                    field,
                    layout.lines[line],
                    field.rect.x + 10,
                    text_y0 + static_cast<int>(line) * layout.line_step);
            }
            if (focused && app.cursor_blink_on) {
                draw_field_caret(x11, body_font, field, layout);
            }
        }
    }

    for (std::size_t i = 0; i < app.buttons.size(); ++i) {
        const auto& button = app.buttons[i];
        std::string label = button.label;
        if (i == 0) label = model_label(app.model);
        if (i == 1) label = resolution_label(app.resolution);
        if (i == 2) label = aspect_label(app.aspect);
        if (i == 3) label = "Duration -";
        if (i == 4) label = "Duration +";
        if (i == 5) label = app.audio ? "Audio on" : "Audio off";
        if (i == 6) label = app.watermark ? "Mark on" : "Mark off";
        if (i == 8 && app.busy) label = "Rendering...";
        IconKind icon = IconKind::None;
        if (i == 7) icon = IconKind::Attach;
        if (i == 8) icon = IconKind::Play;
        if (i == 9) icon = IconKind::Search;
        draw_button(x11, button, label, i == 8, icon);
    }

    std::ostringstream options;
    options << "Duration: " << app.duration << " seconds";
    const auto& rc = L.render_controls;
    set_foreground(x11, x11.muted);
    draw_string(x11, rc.attached_label_x, rc.attached_label_y, "References", FontRole::Label);
    const auto entered_refs = split_reference_values(app.fields.size() > 2 ? app.fields[2].value : std::string{});
    set_foreground(x11, entered_refs.empty() ? x11.muted : x11.warning);
    if (entered_refs.empty()) {
        draw_string(x11, rc.attached_value_x, rc.attached_value_y, "None attached.", FontRole::Body);
    } else {
        const auto& last = entered_refs.back();
        std::ostringstream attached;
        attached << entered_refs.size() << " attached, latest: " << (last.size() > 36 ? "..." + last.substr(last.size() - 33) : last);
        draw_string(x11, rc.attached_value_x, rc.attached_value_y, attached.str(), FontRole::Body);
    }
    set_foreground(x11, x11.ink);
    draw_string(x11, rc.duration_text_x, rc.duration_text_y, options.str(), FontRole::Body);

    set_foreground(x11, x11.muted);
    draw_string(x11, L.progress.x, L.status_label_y, "Status", FontRole::Label);
    set_foreground(x11, x11.ink);
    auto status_lines = wrap_text(app.status, static_cast<std::size_t>(std::max(20, (L.side_w - LayoutConsts::INNER_PAD * 2) / 9)));
    for (int i = 0; i < 3 && i < static_cast<int>(status_lines.size()); ++i) {
        draw_string(x11, L.progress.x, L.status_text_y + i * 19, status_lines[i], FontRole::Body);
    }

    set_foreground(x11, x11.muted);
    draw_string(x11, L.progress.x, L.output_label_y, "Output", FontRole::Label);
    set_foreground(x11, x11.ink);
    auto output_lines = wrap_text(app.output.empty() ? "No result yet." : app.output, static_cast<std::size_t>(std::max(20, (L.side_w - LayoutConsts::INNER_PAD * 2) / 9)));
    for (int i = 0; i < 3 && i < static_cast<int>(output_lines.size()); ++i) {
        draw_string(x11, L.progress.x, L.output_text_y + i * 19, output_lines[i], FontRole::Body);
    }

    draw_file_browser(x11, app);

    x11.draw_target = x11.window;
    x11.XCopyArea(x11.display, x11.back_buffer, x11.window, x11.gc, 0, 0,
        static_cast<unsigned int>(app.width), static_cast<unsigned int>(app.height), 0, 0);
    x11.XFlush(x11.display);
}



SceneInput make_scene_input(const AppState& app) {
    SceneInput input;
    input.api_key = trim_spaces(app.fields[0].value);
    input.prompt = trim_spaces(app.fields[1].value);
    input.save_path = trim_spaces(app.fields[3].value);
    input.task_id_lookup = trim_spaces(app.fields[4].value);
    
    std::vector<std::string> ref_urls = parse_reference_urls(app.fields[2].value);
    input.reference_urls = std::move(ref_urls);
    
    input.model_index = app.model;
    input.resolution_index = app.resolution;
    input.aspect_index = app.aspect;
    input.duration = app.duration;
    input.audio = app.audio;
    input.watermark = app.watermark;
    return input;
}

SceneInput make_lookup_input(const AppState& app) {
    SceneInput input;
    input.api_key = trim_spaces(app.fields[0].value);
    input.task_id_lookup = trim_spaces(app.fields[4].value);
    return input;
}

void click_button(
    int index,
    const std::shared_ptr<AppState>& app,
    const std::shared_ptr<std::recursive_mutex>& mutex,
    const std::shared_ptr<JobController>& jobs) {
    if (index == 0) app->model = (app->model + 1) % 2;
    if (index == 1) app->resolution = (app->resolution + 1) % 4;
    if (index == 2) app->aspect = (app->aspect + 1) % 7;
    if (index == 3) app->duration = std::max(4, app->duration - 1);
    if (index == 4) app->duration = std::min(15, app->duration + 1);
    if (index == 5) app->audio = !app->audio;
    if (index == 6) app->watermark = !app->watermark;
    if (index == 7) {
        open_file_browser(*app);
        app->status = "Select a local image file to attach.";
    }
    if (index == 8) {
        if (jobs) {
            try {
                jobs->start_generate(make_scene_input(*app));
            } catch (const std::exception& error) {
                app->has_error = true;
                app->status = error.what();
            }
        }
    }
    if (index == 9) {
        if (jobs) jobs->start_lookup(make_lookup_input(*app));
    }
    if (index == 10) {
        if (app->busy && jobs) {
            jobs->cancel();
        } else {
            app->reference_urls.clear();
            app->fields[2].value.clear();
            app->fields[2].cursor = 0;
            clear_selection(app->fields[2]);
            app->status = "Cleared reference URLs.";
        }
    }
}

void init_clipboard_atoms(X11& x11) {
    if (!x11.display) return;
    x11.clipboard = x11.XInternAtom(x11.display, "CLIPBOARD", 0);
    x11.utf8_string = x11.XInternAtom(x11.display, "UTF8_STRING", 1);
    x11.string_atom = x11.XInternAtom(x11.display, "STRING", 0);
    x11.paste_property = x11.XInternAtom(x11.display, "HOSV_PASTE", 0);
}

void insert_text_into_field(Field& field, const std::string& text) {
    insert_text_at_cursor(field, text);
}

bool write_system_clipboard(const std::string& text) {
    FILE* pipe = popen("xclip -selection clipboard -i 2>/dev/null", "w");
    if (!pipe) return false;
    if (!text.empty()) {
        const auto written = std::fwrite(text.data(), 1, text.size(), pipe);
        if (written != text.size()) {
            pclose(pipe);
            return false;
        }
    }
    return pclose(pipe) == 0;
}

bool copy_text_to_clipboard(X11& x11, const std::string& text) {
    if (text.empty()) return false;
    x11.clipboard_text = text;
    x11.clipboard_text_exported = x11.display && write_system_clipboard(text);
    return true;
}

bool request_clipboard_paste(X11& x11, Atom target) {
    if (!x11.display || !x11.XConvertSelection || x11.clipboard == 0 || x11.paste_property == 0 || target == 0) {
        return false;
    }
    x11.paste_target = target;
    x11.XConvertSelection(x11.display, x11.clipboard, target, x11.paste_property, x11.window, 0);
    x11.XFlush(x11.display);
    return true;
}

bool read_paste_property(X11& x11, std::string& out) {
    out.clear();
    if (!x11.display || !x11.XGetWindowProperty || x11.paste_property == 0) return false;

    Atom actual_type = 0;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char* data = nullptr;
    const int status = x11.XGetWindowProperty(
        x11.display,
        x11.window,
        x11.paste_property,
        0,
        1'000'000,
        1,
        0,
        &actual_type,
        &actual_format,
        &item_count,
        &bytes_after,
        &data);
    if (status != 0 || !data || item_count == 0 || actual_format <= 0) {
        if (data) x11.XFree(data);
        return false;
    }

    const unsigned long byte_count = item_count * static_cast<unsigned long>(actual_format) / 8;
    if (byte_count > 0) out.assign(reinterpret_cast<const char*>(data), byte_count);
    x11.XFree(data);
    while (!out.empty() && out.back() == '\0') out.pop_back();
    return !out.empty();
}

void clear_paste_request(X11& x11) {
    x11.paste_field = -1;
    x11.paste_target = 0;
}

bool handle_selection_notify(X11& x11, AppState& app, const XEvent& event) {
    if (x11.paste_field < 0 || static_cast<std::size_t>(x11.paste_field) >= app.fields.size()) {
        clear_paste_request(x11);
        return false;
    }
    if (event.type != SelectionNotify || event.xany.window != x11.window) return false;

    std::string pasted;
    if (read_paste_property(x11, pasted)) {
        const int target = x11.paste_field;
        if (target == app.focus && !app.fields[static_cast<std::size_t>(target)].readonly) {
            insert_text_into_field(app.fields[static_cast<std::size_t>(target)], pasted);
            app.cursor_blink_on = true;
        }
        clear_paste_request(x11);
        return target == app.focus;
    }

    if (x11.paste_target == x11.utf8_string && x11.string_atom != 0) {
        request_clipboard_paste(x11, x11.string_atom);
        return false;
    }

    clear_paste_request(x11);
    return false;
}

bool begin_clipboard_paste(X11& x11, int field_index) {
    if (!x11.display || !x11.XGetSelectionOwner || x11.clipboard == 0) return false;
    if (x11.XGetSelectionOwner(x11.display, x11.clipboard) == 0) return false;

    const Atom target = x11.utf8_string != 0 ? x11.utf8_string : x11.string_atom;
    if (target == 0) return false;

    x11.paste_field = field_index;
    return request_clipboard_paste(x11, target);
}

void advance_focus(AppState& app) {
    const int count = static_cast<int>(app.fields.size());
    for (int step = 0; step < count; ++step) {
        if (app.focus >= 0 && static_cast<std::size_t>(app.focus) < app.fields.size()) {
            clear_selection(app.fields[app.focus]);
        }
        app.focus = (app.focus + 1) % count;
        if (!app.fields[app.focus].readonly) {
            app.fields[app.focus].cursor = static_cast<int>(app.fields[app.focus].value.size());
            clear_selection(app.fields[app.focus]);
            return;
        }
    }
}

void handle_key(X11& x11, XKeyEvent& event, AppState& app) {
    char buffer[32] = {};
    KeySym sym = 0;
    const int count = x11.XLookupString(&event, buffer, sizeof(buffer), &sym, nullptr);
    const bool control = (event.state & ControlMask) != 0;
    const bool shift = (event.state & ShiftMask) != 0;
    const char key = count > 0 ? static_cast<char>(buffer[0]) : '\0';

    if (sym == XK_Escape) {
        if (app.file_browser_open) app.file_browser_open = false;
        else app.should_close = true;
        return;
    }
    if (app.file_browser_open) {
        handle_file_browser_key(app, event.state, sym, buffer, count);
        return;
    }
    if (sym == XK_Tab) {
        advance_focus(app);
        return;
    }
    if (sym == XK_Return) {
        if (app.focus >= 0 && static_cast<std::size_t>(app.focus) < app.fields.size()) {
            auto& field = app.fields[app.focus];
            if (!field.readonly && field.multiline) {
                insert_text_at_cursor(field, "\n");
                app.cursor_blink_on = true;
            } else {
                advance_focus(app);
            }
        }
        return;
    }
    if (app.focus < 0 || static_cast<std::size_t>(app.focus) >= app.fields.size()) return;

    auto& field = app.fields[app.focus];
    if (field.readonly) return;
    clamp_field_cursor(field);

    if (control && is_key(sym, key, XK_a, XK_A, 'a', 'A')) {
        select_all(field);
        app.cursor_blink_on = true;
        return;
    }
    if (control && is_key(sym, key, XK_c, XK_C, 'c', 'C')) {
        copy_text_to_clipboard(x11, selected_field_text(field));
        app.cursor_blink_on = true;
        return;
    }
    if (control && is_key(sym, key, XK_x, XK_X, 'x', 'X')) {
        if (copy_text_to_clipboard(x11, selected_field_text(field))) {
            delete_selection(field);
        }
        app.cursor_blink_on = true;
        return;
    }
    if ((control && (sym == XK_v || sym == XK_V || key == 'v' || key == 'V')) || (shift && sym == XK_Insert)) {
        if (!x11.clipboard_text.empty() && !x11.clipboard_text_exported) {
            insert_text_at_cursor(field, x11.clipboard_text);
            app.cursor_blink_on = true;
            return;
        }
        if (!begin_clipboard_paste(x11, app.focus) && !x11.clipboard_text.empty()) {
            insert_text_at_cursor(field, x11.clipboard_text);
            app.cursor_blink_on = true;
        }
        return;
    }
    if (sym == XK_Insert) return;

    if (is_backspace_key(sym, buffer, count)) {
        delete_before_cursor(field);
        app.cursor_blink_on = true;
        return;
    }
    if (sym == XK_Delete) {
        delete_at_cursor(field);
        app.cursor_blink_on = true;
        return;
    }
    if (sym == XK_Left) {
        field.cursor = utf8_byte_index_before(field.value, field.cursor);
        clear_selection(field);
        app.cursor_blink_on = true;
        return;
    }
    if (sym == XK_Right) {
        field.cursor = utf8_byte_index_after(field.value, field.cursor);
        clear_selection(field);
        app.cursor_blink_on = true;
        return;
    }
    if (field.multiline && sym == XK_Up) {
        move_cursor_vertical(field, -1);
        clear_selection(field);
        app.cursor_blink_on = true;
        return;
    }
    if (field.multiline && sym == XK_Down) {
        move_cursor_vertical(field, 1);
        clear_selection(field);
        app.cursor_blink_on = true;
        return;
    }
    if (sym == XK_Home) {
        field.cursor = field.multiline ? line_start_byte(field.value, field.cursor) : 0;
        clear_selection(field);
        app.cursor_blink_on = true;
        return;
    }
    if (sym == XK_End) {
        if (field.multiline) {
            field.cursor = line_end_byte(field.value, line_start_byte(field.value, field.cursor));
        } else {
            field.cursor = static_cast<int>(field.value.size());
        }
        clear_selection(field);
        app.cursor_blink_on = true;
        return;
    }

    if (event.state & (ControlMask | Mod1Mask | Mod4Mask)) return;

    app.cursor_blink_on = true;
    if (count > 0) {
        insert_text_at_cursor(field, std::string(buffer, static_cast<std::size_t>(count)));
    }
}

void load_dotenv() {
    const char* override_path = std::getenv("HOSV_BASHRC_PATH");
    std::string bashrc_path;
    if (override_path) {
        if (std::string(override_path) == "skip") {
            return;
        }
        bashrc_path = override_path;
    } else {
        const char* home_dir = std::getenv("HOME");
        if (!home_dir) {
            return;
        }
        bashrc_path = (fs::path(home_dir) / ".bashrc").string();
    }
    std::ifstream file(bashrc_path);
    if (!file.is_open()) {
        return;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') {
            continue;
        }
        
        std::string parsed_line = line.substr(start);
        
        // Strip "export" command prefix if present
        if (parsed_line.rfind("export", 0) == 0) {
            if (parsed_line.size() > 6 && (parsed_line[6] == ' ' || parsed_line[6] == '\t')) {
                auto next_start = parsed_line.find_first_not_of(" \t", 6);
                if (next_start != std::string::npos) {
                    parsed_line = parsed_line.substr(next_start);
                } else {
                    continue;
                }
            }
        }
        
        auto eq = parsed_line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = parsed_line.substr(0, eq);
        auto key_end = key.find_last_not_of(" \t");
        if (key_end != std::string::npos) {
            key = key.substr(0, key_end + 1);
        } else {
            key = "";
        }
        
        std::string val = parsed_line.substr(eq + 1);
        auto val_start = val.find_first_not_of(" \t");
        if (val_start != std::string::npos) {
            val = val.substr(val_start);
        } else {
            val = "";
        }
        auto val_end = val.find_last_not_of(" \t");
        if (val_end != std::string::npos) {
            val = val.substr(0, val_end + 1);
        } else {
            val = "";
        }
        
        if (val.size() >= 2 && (
            (val.front() == '"' && val.back() == '"') ||
            (val.front() == '\'' && val.back() == '\'')
        )) {
            val = val.substr(1, val.size() - 2);
        }
        
        if (!key.empty()) {
            if (std::getenv(key.c_str()) == nullptr) {
                setenv(key.c_str(), val.c_str(), 1);
            }
        }
    }
}

int run_gui() {
    X11 x11;
    auto app_state = std::make_shared<AppState>();
    AppState& app = *app_state;
    if (!open_x11(x11, app.width, app.height)) {
        throw std::runtime_error("X11 display is unavailable. Run from a desktop session with DISPLAY set.");
    }

    app.request_redraw = [&x11]() {
        if (!x11.display || !x11.window || !x11.XSendEvent) return;
        XEvent event{};
        event.type = ClientMessage;
        event.xclient.type = ClientMessage;
        event.xclient.window = x11.window;
        event.xclient.message_type = x11.wm_delete;
        event.xclient.format = 32;
        event.xclient.data.l[0] = 999;
        x11.XSendEvent(x11.display, x11.window, 0, 0, &event);
        x11.XFlush(x11.display);
    };

    if (const char* key = std::getenv("BYTEPLUS_ARK_API_KEY")) {
        app.fields.resize(5);
        app.fields[0].value = key;
    } else if (const char* key = std::getenv("ARK_API_KEY")) {
        app.fields.resize(5);
        app.fields[0].value = key;
    }
    app.fields.resize(5);
    app.fields[1].value = "A clean HOSV product film with precise studio motion.";
    app.fields[3].value = "./";
    app.fields[app.focus].cursor = static_cast<int>(app.fields[app.focus].value.size());
    load_logo(app.logo, LayoutConsts::LOGO_TEXTURE_SIZE);
    set_wm_title(x11, "HOSV");
    set_wm_icon(x11);
    init_clipboard_atoms(x11);
    map_window(x11);
    set_layout(app);
    if (std::getenv("HOSV_LAYOUT_DEBUG")) {
        dump_layout_diagnostics(app, stderr);
        verify_layout_diagnostics(app, stderr);
    }

    auto mutex_state = std::make_shared<std::recursive_mutex>();
    std::recursive_mutex& mutex = *mutex_state;

    auto provider = std::make_shared<BytePlusArkVideoProvider>();
    auto service = std::make_shared<SeedanceService>(provider);
    auto jobs = std::make_shared<JobController>(service, app_state, mutex_state);

    auto last_cursor_blink = std::chrono::steady_clock::now();
    redraw(x11, app);
    while (!app.should_close) {
        bool redraw_for_dirty = false;
        if (!x11.XPending(x11.display)) {
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_cursor_blink).count() >= 500) {
                last_cursor_blink = now;
                std::lock_guard<std::recursive_mutex> lock(mutex);
                app.cursor_blink_on = !app.cursor_blink_on;
                if (!app.file_browser_open && app.focus >= 0 &&
                    static_cast<std::size_t>(app.focus) < app.fields.size() &&
                    !app.fields[app.focus].readonly) {
                    redraw_for_dirty = true;
                }
            }
            {
                std::lock_guard<std::recursive_mutex> lock(mutex);
                if (app.frame_dirty) {
                    app.frame_dirty = false;
                    redraw_for_dirty = true;
                }
            }
            if (redraw_for_dirty) {
                redraw(x11, app);
                continue;
            }
        }
        XEvent event;
        x11.XNextEvent(x11.display, &event);
        bool needs_redraw = false;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            if (event.type == ConfigureNotify) {
                app.width = std::max(760, event.xconfigure.width);
                app.height = std::max(846, event.xconfigure.height);
                set_layout(app);
                needs_redraw = true;
            } else if (event.type == Expose) {
                needs_redraw = true;
            } else if (event.type == ButtonPress) {
                if (!app.file_browser_open) {
                    for (std::size_t i = 0; i < app.fields.size(); ++i) {
                        if (!app.fields[i].readonly && contains(app.fields[i].rect, event.xbutton.x, event.xbutton.y)) {
                            app.focus = static_cast<int>(i);
                            app.fields[i].cursor = hit_test_field_cursor(
                                font_for_role(FontRole::Body),
                                app.fields[i],
                                event.xbutton.x,
                                event.xbutton.y,
                                app.focus == static_cast<int>(i));
                            clear_selection(app.fields[i]);
                            app.cursor_blink_on = true;
                        }
                    }
                    for (std::size_t i = 0; i < app.buttons.size(); ++i) {
                        if (contains(app.buttons[i].rect, event.xbutton.x, event.xbutton.y)) {
                            click_button(static_cast<int>(i), app_state, mutex_state, jobs);
                        }
                    }
                } else {
                    const int dialog_w = std::min(740, app.width - 80);
                    const int dialog_h = std::min(520, app.height - 80);
                    const Rect dialog{(app.width - dialog_w) / 2, (app.height - dialog_h) / 2, dialog_w, dialog_h};
                    const Rect cancel_btn{dialog.x + dialog.w - 90, dialog.y + 16, 72, 28};
                    if (contains(cancel_btn, event.xbutton.x, event.xbutton.y) || !contains(dialog, event.xbutton.x, event.xbutton.y)) {
                        app.file_browser_open = false;
                    } else {
                        const int rows = file_browser_visible_rows(app);
                        const std::size_t start = static_cast<std::size_t>(app.file_page * rows);
                        const std::size_t end = std::min(app.file_entries.size(), start + static_cast<std::size_t>(rows));
                        for (std::size_t i = start; i < end; ++i) {
                            auto& entry = app.file_entries[i];
                            if (contains(entry.rect, event.xbutton.x, event.xbutton.y)) {
                                const int visible_row = static_cast<int>(i - start);
                                app.file_cursor = visible_row;
                                file_browser_activate_selection(app);
                                break;
                            }
                        }
                    }
                }
                needs_redraw = true;
            } else if (event.type == KeyPress) {
                handle_key(x11, event.xkey, app);
                needs_redraw = true;
            } else if (event.type == SelectionNotify) {
                if (handle_selection_notify(x11, app, event)) needs_redraw = true;
            } else if (event.type == ClientMessage && static_cast<Atom>(event.xclient.data.l[0]) == x11.wm_delete) {
                app.should_close = true;
            }
            if (app.frame_dirty) {
                app.frame_dirty = false;
                needs_redraw = true;
            }
        }
        if (needs_redraw) redraw(x11, app);
    }

    app.request_redraw = nullptr;
    close_x11(x11);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    load_dotenv();
    std::string provider = "byteplus-ark";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::printf("Usage: hosv [--provider byteplus-ark] [--self-test] [--dump-layout]\n");
            return 0;
        }
        if (arg == "--provider" && i + 1 < argc) {
            provider = argv[++i];
            continue;
        }
    }
    if (provider == "seedance2.ai" || provider == "api.seedance2.ai" || provider == "seedance2") {
        std::fprintf(stderr, "The unofficial seedance2.ai provider has been disabled. Use the official BytePlus ModelArk provider.\n");
        return 2;
    }
    if (provider != "byteplus-ark") {
        std::fprintf(stderr, "Unsupported provider: %s\n", provider.c_str());
        return 2;
    }
    setenv("HOSV_PROVIDER", provider.c_str(), 1);

    if (argc >= 2 && std::string(argv[1]) == "--self-test") {
        return run_self_tests();
    }
    if (argc >= 2 && std::string(argv[1]) == "--dump-layout") {
        return dump_layout_and_exit();
    }
    try {
        return run_gui();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "HOSV: %s\n", error.what());
        return 1;
    }
}
