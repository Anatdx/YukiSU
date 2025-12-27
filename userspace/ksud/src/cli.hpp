#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ksud {

int cli_run(int argc, char* argv[]);

// Command handler type
using CommandHandler = std::function<int(const std::vector<std::string>&)>;

// CLI argument parser helpers
struct CliOption {
    std::string long_name;
    char short_name;
    std::string description;
    bool takes_value;
    std::string default_value;
};

class CliParser {
public:
    void add_option(const CliOption& opt);
    bool parse(int argc, char* argv[]);

    std::optional<std::string> get_option(const std::string& name) const;
    bool has_option(const std::string& name) const;
    const std::vector<std::string>& positional() const { return positional_args_; }
    const std::string& subcommand() const { return subcommand_; }

private:
    std::vector<CliOption> options_;
    std::map<std::string, std::string> parsed_options_;
    std::vector<std::string> positional_args_;
    std::string subcommand_;
};

}  // namespace ksud
