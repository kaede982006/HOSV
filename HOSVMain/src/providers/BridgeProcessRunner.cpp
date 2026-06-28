#include "providers/BridgeProcessRunner.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
void close_fd(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

void set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

void append_available(int fd, std::string& out) {
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t n = read(fd, buffer.data(), buffer.size());
        if (n > 0) {
            out.append(buffer.data(), static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            break;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
}
}

BridgeRunResult BridgeProcessRunner::run(const std::string& binary,
                                         const std::string& command,
                                         const std::string& input_json,
                                         std::chrono::milliseconds timeout) const {
    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        throw std::runtime_error("failed to create arkbridge pipes");
    }

    const pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("failed to fork arkbridge process");
    }
    if (pid == 0) {
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        execl(binary.c_str(), binary.c_str(), command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    close_fd(stdin_pipe[0]);
    close_fd(stdout_pipe[1]);
    close_fd(stderr_pipe[1]);
    set_nonblocking(stdout_pipe[0]);
    set_nonblocking(stderr_pipe[0]);

    std::string input = input_json;
    if (input.empty() || input.back() != '\n') input.push_back('\n');
    const char* cursor = input.data();
    std::size_t remaining = input.size();
    while (remaining > 0) {
        const ssize_t n = write(stdin_pipe[1], cursor, remaining);
        if (n > 0) {
            cursor += n;
            remaining -= static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    close_fd(stdin_pipe[1]);

    BridgeRunResult result;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    bool child_done = false;
    while (!child_done) {
        append_available(stdout_pipe[0], result.stdout_text);
        append_available(stderr_pipe[0], result.stderr_text);
        const pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            child_done = true;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            result.timed_out = true;
            child_done = true;
            break;
        }
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(stdout_pipe[0], &readfds);
        FD_SET(stderr_pipe[0], &readfds);
        timeval tv{0, 100000};
        select(std::max(stdout_pipe[0], stderr_pipe[0]) + 1, &readfds, nullptr, nullptr, &tv);
    }
    append_available(stdout_pipe[0], result.stdout_text);
    append_available(stderr_pipe[0], result.stderr_text);
    close_fd(stdout_pipe[0]);
    close_fd(stderr_pipe[0]);

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    return result;
}
