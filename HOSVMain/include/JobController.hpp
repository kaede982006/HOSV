#pragma once

#include "AppModel.hpp"
#include "SeedanceService.hpp"
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

class JobController {
public:
    explicit JobController(std::shared_ptr<SeedanceService> service, std::shared_ptr<AppState> model, std::shared_ptr<std::recursive_mutex> mutex);
    ~JobController();

    void start_generate(SceneInput snapshot);
    void start_lookup(SceneInput snapshot);
    void cancel();
    JobPhase phase() const noexcept;

private:
    void run_generate(SceneInput snapshot, std::stop_token st);
    void run_lookup(SceneInput snapshot, std::stop_token st);
    void update_status(const std::string& status, const std::string& output = {});
    void set_phase(JobPhase p, const std::string& msg);
    void mark_idle();
    void set_cancelled();
    void fail(const std::string& msg);
    void set_current_task_id(const std::string& task_id);
    std::string current_task_id() const;
    void clear_current_task_id();
    void cancel_remote_task(const std::string& task_id);

    std::shared_ptr<SeedanceService> service_;
    std::shared_ptr<AppState> model_;
    std::shared_ptr<std::recursive_mutex> mutex_;
    std::string current_task_id_;
};
