#pragma once

#include "providers/BridgeProcessRunner.hpp"
#include "providers/VideoProvider.hpp"
#include <chrono>
#include <string>

class BytePlusArkVideoProvider final : public IVideoProvider {
public:
    explicit BytePlusArkVideoProvider(std::string bridge_path = {}, std::chrono::milliseconds timeout = std::chrono::milliseconds(120000));

    CreateTaskResult createTask(const VideoGenerationRequest& request) override;
    TaskStatusResult getTask(const std::string& taskId) override;
    TaskStatusResult waitTask(const std::string& taskId, PollOptions options) override;
    CancelTaskResult cancelTask(const std::string& taskId) override;

private:
    TaskStatusResult runTaskCommand(const std::string& command, const std::string& taskId, const PollOptions* poll = nullptr);
    std::string bridge_path_;
    std::chrono::milliseconds timeout_;
    BridgeProcessRunner runner_;
};
