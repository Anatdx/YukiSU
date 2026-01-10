/**
 * Property Monitor Implementation
 */

#include "property_monitor.hpp"
#include "init_event.hpp"
#include "log.hpp"

#include <sys/system_properties.h>
#include <chrono>
#include <thread>

namespace ksud {
namespace property {

std::string get_property(const std::string& name) {
    char value[PROP_VALUE_MAX] = {0};
    __system_property_get(name.c_str(), value);
    return std::string(value);
}

bool wait_property(const std::string& name, const std::string& target, uint32_t timeout_ms) {
    auto start = std::chrono::steady_clock::now();

    while (true) {
        std::string current = get_property(name);
        if (current == target) {
            return true;
        }

        if (timeout_ms > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count();
            if (elapsed >= timeout_ms) {
                return false;
            }
        }

        // Poll every 100ms
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

static void property_monitor_thread() {
    LOGI("Property monitor thread started");

    // Wait for sys.boot_completed=1
    // This indicates Android framework is fully up
    LOGI("Waiting for sys.boot_completed=1...");
    while (get_property("sys.boot_completed") != "1") {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    LOGI("sys.boot_completed=1 detected, triggering boot-completed");
    on_boot_completed();

    LOGI("Property monitor thread finished");
}

void start_property_monitor() {
    std::thread(property_monitor_thread).detach();
}

}  // namespace property
}  // namespace ksud
