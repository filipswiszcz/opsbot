#ifndef HOR_CONSOLE_H
#define HOR_CONSOLE_H

#include <atomic>
#include <mutex>
#include <thread>

#include "common.h"
#include "memory.h"

enum class console_status : u8 {
    SUCCESS = 0,
    INVALID_ARG,
    REGISTRY_FULL,
    DUPLICATE_CMD
};

class console final {
public:
    [[nodiscard]] console_status initialize(void) noexcept;
    void terminate(void) noexcept;

    void process_commands(void) noexcept;

    void log(const char *message) noexcept;
    void logf(const char *message, ...) noexcept;
private:
    std::thread worker;
    std::mutex lock;
    std::atomic<u8> is_running{0U};
};

#endif // !HOR_CONSOLE_H