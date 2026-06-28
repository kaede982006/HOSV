#include <cstdlib>
#include <iostream>

#include "seedance2/seedance2.hpp"

int main() {
    const char* key = std::getenv("SEEDANCE2_API_KEY");
    if (!key) {
        std::cerr << "Set SEEDANCE2_API_KEY first.\n";
        return 2;
    }

    seedance2::Client client({key});

    seedance2::TextToVideoRequest request;
    request.prompt = "A cinematic sunrise over Seoul, gentle camera movement";
    request.options.resolution = seedance2::Resolution::R720p;
    request.options.aspect_ratio = seedance2::AspectRatio::Ratio16x9;
    request.options.duration_seconds = 5;
    request.options.generate_audio = true;
    request.options.watermark = false;

    try {
        auto task = client.create_text_to_video(request);
        std::cout << "task id: " << task.id << "\n";

        auto completed = client.wait_for_task(task.id, {});
        std::cout << "status: " << seedance2::to_string(completed.status) << "\n";
        for (const auto& url : completed.video_urls) {
            std::cout << url << "\n";
        }
    } catch (const seedance2::ApiException& e) {
        std::cerr << "Seedance API error " << e.http_status << ": " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
