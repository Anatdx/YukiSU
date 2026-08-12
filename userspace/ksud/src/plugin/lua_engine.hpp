#pragma once

#include <string>

namespace ksud {

enum class PluginRunResult {
    Success,
    MissingCallback,
    Failed,
};

PluginRunResult run_plugin_callback_isolated(const std::string& plugin_id,
                                             const std::string& callback);
bool exec_plugin_stage(const std::string& stage, bool block);
bool stop_plugin_daemons(const std::string& plugin_id, std::string* error);
bool start_plugin_daemon(const std::string& plugin_id, const std::string& callback,
                         int interval_seconds, int ready_fd);

}  // namespace ksud
