#pragma once

#include <chrono>
#include <string>

struct BridgeRunResult {
    int exit_code = -1;
    bool timed_out = false;
    std::string stdout_text;
    std::string stderr_text;
};

class BridgeProcessRunner {
public:
    BridgeRunResult run(const std::string& binary,
                        const std::string& command,
                        const std::string& input_json,
                        std::chrono::milliseconds timeout) const;
};
