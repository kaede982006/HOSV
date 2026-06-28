#pragma once

#include "AppModel.hpp"
#include "providers/VideoProvider.hpp"
#include <memory>
#include <chrono>
#include <stop_token>
#include <functional>

class SeedanceService {
public:
    explicit SeedanceService(std::shared_ptr<IVideoProvider> provider);

    VideoGenerationTask submit_text(const SceneInput& input);
    VideoGenerationResult lookup(const std::string& task_id);
    VideoGenerationResult cancel(const std::string& task_id);
    VideoGenerationResult wait(const std::string& task_id,
                               std::chrono::milliseconds interval,
                               std::stop_token stop,
                               std::function<void(const VideoGenerationResult&)> on_progress);

private:
    std::shared_ptr<IVideoProvider> provider_;
};
