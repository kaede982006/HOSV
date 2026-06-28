# HOSV: Native GUI Client & C++ SDK for Seedance 2 Video Generation

HOSV is a lightweight C++ codebase designed for native AI video generation using the Seedance 2 video generation API. It consists of a performance-oriented dynamic library/SDK and a dependency-free native GUI frontend designed for Linux (X11).

---

## Project Structure

The project is split into two primary components:

1. **[HOSVApi](file:///home/hyobeen/프로젝트/HOSV/HOSVApi)**: A modern C++17 client library for the Seedance 2 API (`https://api.seedance2.ai`). It wraps curl and mbedTLS.
   - Core API declarations: [seedance2.hpp](file:///home/hyobeen/프로젝트/HOSV/HOSVApi/include/seedance2/seedance2.hpp)
   - Configuration build options: [HOSVApi/CMakeLists.txt](file:///home/hyobeen/프로젝트/HOSV/HOSVApi/CMakeLists.txt)
2. **[HOSVMain](file:///home/hyobeen/프로젝트/HOSV/HOSVMain)**: A native X11 GUI client that features a custom software rendering loop and dynamic library resolution.
   - Entry point and X11 rendering: [main.cpp](file:///home/hyobeen/프로젝트/HOSV/HOSVMain/src/main.cpp)
   - Build definitions: [HOSVMain/CMakeLists.txt](file:///home/hyobeen/프로젝트/HOSV/HOSVMain/CMakeLists.txt)

Supporting files at the root level include:
- [build.sh](file:///home/hyobeen/프로젝트/HOSV/build.sh): A unified Bash build helper script.
- [LICENSE](file:///home/hyobeen/프로젝트/HOSV/LICENSE): The GNU General Public License v3 terms.

---

## Key Features

### HOSVApi (C++ SDK)
- **Comprehensive API Support**: Easily send generation requests for Text-to-Video, Image-to-Video, Multi-Image, and Reference-to-Video.
- **Embedded Dependencies**: Includes optional, statically compiled third-party dependencies (`libcurl` and `mbedTLS`) located in `third_party/` to avoid external package version conflicts.
- **Robust Polling**: Built-in methods to poll active generation tasks with custom callbacks to track progress.

### HOSVMain (Native Linux GUI Client)
- **Zero Compile-Time Graphical Dependencies**: Does not link against heavy toolkits like Qt, GTK, or wxWidgets, and does not even link against `libX11` at compile-time.
- **Dynamic X11 Loader**: Dynamically resolves X11 function pointers at runtime using `dlopen` and `dlsym` inside [main.cpp](file:///home/hyobeen/프로젝트/HOSV/HOSVMain/src/main.cpp).
- **Custom Software Renderer**: Directly renders pixel buffers using custom implementations of layout algorithms, text drawing via `stb_truetype`, and image loading to maximize portability and minimize overhead.
- **Visual Task Configurator**: Fully interactive input fields for prompt submission, model selection, custom seeds, resolution settings, aspect ratios, audio generation, and watermarking.

---

## C++ API Usage Example

Below is a basic example illustrating how to configure the C++ SDK to generate a video from a text prompt. You can view the full example file here: [text_to_video.cpp](file:///home/hyobeen/프로젝트/HOSV/HOSVApi/examples/text_to_video.cpp).

```cpp
#include <cstdlib>
#include <iostream>
#include "seedance2/seedance2.hpp"

int main() {
    const char* key = std::getenv("SEEDANCE2_API_KEY");
    if (!key) {
        std::cerr << "Set SEEDANCE2_API_KEY first.\n";
        return 2;
    }

    // Initialize the Client with options
    seedance2::Client client({key});

    // Configure the video generation request
    seedance2::TextToVideoRequest request;
    request.prompt = "A cinematic sunrise over Seoul, gentle camera movement";
    request.options.resolution = seedance2::Resolution::R720p;
    request.options.aspect_ratio = seedance2::AspectRatio::Ratio16x9;
    request.options.duration_seconds = 5;
    request.options.generate_audio = true;
    request.options.watermark = false;

    try {
        // Submit the text-to-video request
        auto task = client.create_text_to_video(request);
        std::cout << "Task submitted successfully. ID: " << task.id << "\n";

        // Poll the API until the task is complete
        auto completed = client.wait_for_task(task.id, {});
        std::cout << "Task status: " << seedance2::to_string(completed.status) << "\n";
        
        for (const auto& url : completed.video_urls) {
            std::cout << "Generated Video URL: " << url << "\n";
        }
    } catch (const seedance2::ApiException& e) {
        std::cerr << "Seedance API error (" << e.http_status << "): " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
```

### Key API Classes & Structures

- **[Client](file:///home/hyobeen/프로젝트/HOSV/HOSVApi/include/seedance2/seedance2.hpp#L171)**: The main entry point class to interact with the API, handling endpoints such as `create_text_to_video`, `create_image_to_video`, `get_task`, and `wait_for_task`.
- **[TextToVideoRequest](file:///home/hyobeen/프로젝트/HOSV/HOSVApi/include/seedance2/seedance2.hpp#L104)** / **[ImageToVideoRequest](file:///home/hyobeen/프로젝트/HOSV/HOSVApi/include/seedance2/seedance2.hpp#L109)**: Parameter structures containing the prompt, input images, and generation settings.
- **[GenerationOptions](file:///home/hyobeen/프로젝트/HOSV/HOSVApi/include/seedance2/seedance2.hpp#L90)**: Configurations defining:
  - **[VideoModel](file:///home/hyobeen/프로젝트/HOSV/HOSVApi/include/seedance2/seedance2.hpp#L25)** (`Seedance20`, `Seedance20Fast`)
  - **[Resolution](file:///home/hyobeen/프로젝트/HOSV/HOSVApi/include/seedance2/seedance2.hpp#L30)** (`R480p`, `R720p`, `R1080p`, `R4k`)
  - **[AspectRatio](file:///home/hyobeen/프로젝트/HOSV/HOSVApi/include/seedance2/seedance2.hpp#L37)** (`Ratio16x9`, `Ratio9x16`, `Ratio1x1`, etc.)
- **[GenerationTask](file:///home/hyobeen/프로젝트/HOSV/HOSVApi/include/seedance2/seedance2.hpp#L130)**: Object returned by the API containing the current status, task metadata, billing logs, and resultant video/last frame URLs.

---

## Build and Run Instructions

### Prerequisites
- A C++ compiler supporting C++17 (e.g., GCC 9+ or Clang 10+).
- CMake version 3.16 or newer.
- An active X11 desktop environment (for the GUI client).

### Build steps

The easiest way to build the project is via the provided [build.sh](file:///home/hyobeen/프로젝트/HOSV/build.sh) script:

1. **Perform a clean build**:
   ```bash
   ./build.sh --clean
   ```
2. **Perform a standard build**:
   ```bash
   ./build.sh
   ```

Upon completion, all generated build files are kept under `build/HOSV`, and the distribution package (executable, fonts, logo) will be exported to the `dist/` directory.

### Running HOSV GUI

To launch the native GUI client:

1. Export your API Key:
   ```bash
   export SEEDANCE2_API_KEY="your-api-key-here"
   ```
2. Run the executable from the `dist/` folder:
   ```bash
   ./dist/HOSV
   ```

---

## License

This project is licensed under the terms of the GNU General Public License v3. See the [LICENSE](file:///home/hyobeen/프로젝트/HOSV/LICENSE) file for details.
