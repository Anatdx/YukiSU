/**
 * Property Monitor - Watch Android system properties
 *
 * Uses __system_property_wait_any to efficiently monitor property changes.
 */

#pragma once

#include <functional>
#include <string>

namespace ksud {
namespace property {

/**
 * Get a system property value
 * @param name Property name
 * @return Property value, empty if not found
 */
std::string get_property(const std::string& name);

/**
 * Wait for a property to have specific value
 * @param name Property name
 * @param value Target value
 * @param timeout_ms Timeout in milliseconds (0 = wait forever)
 * @return true if property matched, false if timeout
 */
bool wait_property(const std::string& name, const std::string& value, uint32_t timeout_ms = 0);

/**
 * Start property monitor thread
 * This monitors sys.boot_completed and triggers callbacks
 */
void start_property_monitor();

}  // namespace property
}  // namespace ksud
