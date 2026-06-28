#pragma once

#include "AppModel.hpp"
#include "seedance2/seedance2.hpp"
#include <memory>
#include <chrono>
#include <stop_token>
#include <functional>

class IHttpTransport {
public:
    virtual ~IHttpTransport() = default;
    virtual std::string get_api_key() const = 0;
    virtual std::string request(const std::string& method, const std::string& path, const std::string& body) = 0;
};

class CurlHttpTransport : public IHttpTransport {
public:
    CurlHttpTransport(const std::string& api_key, const std::string& base_url);
    std::string get_api_key() const override;
    std::string request(const std::string& method, const std::string& path, const std::string& body) override;
private:
    std::string api_key_;
    std::string base_url_;
};

class AppStateTransport : public IHttpTransport {
public:
    explicit AppStateTransport(std::shared_ptr<AppState> app);
    std::string get_api_key() const override;
    std::string request(const std::string& method, const std::string& path, const std::string& body) override;
private:
    std::shared_ptr<AppState> app_;
};

class SeedanceService {
public:
    explicit SeedanceService(std::shared_ptr<IHttpTransport> transport);

    seedance2::GenerationTask submit_text(const SceneInput& input);
    seedance2::GenerationTask lookup(const std::string& task_id);
    seedance2::GenerationTask wait(const std::string& task_id,
                                   std::chrono::milliseconds interval,
                                   std::stop_token stop,
                                   std::function<void(const seedance2::GenerationTask&)> on_progress);

private:
    std::shared_ptr<IHttpTransport> transport_;
};
