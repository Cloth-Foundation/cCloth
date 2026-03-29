#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <optional>

namespace cloth::cmd {

    struct OptionStructure {
        std::string_view longName;
        std::string_view shortName;
        std::string_view description;
        bool takesValue = false;
        bool required = false;
    };

    struct CommandStructure {
        std::string_view name;
        std::string_view description;
        std::string_view usage;
        std::vector<std::string_view> aliases;
        std::vector<OptionStructure> options;
        std::vector<std::string_view> examples;
    };

    struct ParsedOption {
        std::string name;
        std::optional<std::string> value;
    };

    struct ParsedCommand {
        const CommandStructure *command = nullptr;
        std::vector<ParsedOption> options;
        std::vector<std::string> positionalArguments;
    };

    const CommandStructure *getCommand(std::string_view name);
    ParsedCommand parse(int argc, char **argv);

    bool hasOption(const ParsedCommand &parsed, std::string_view name);
    std::optional<std::string> getOptionValue(const ParsedCommand &parsed, std::string_view name);

    void printHelp();
    void printUsage(std::string_view name);
    void printVersion();
    void printCommands();
    void printCommand(std::string_view name);
    void printCommandHelp(std::string_view name);
    void printCommandUsage(std::string_view name);
    void printCommandVersion(std::string_view name);
    void printCommandDescription(std::string_view name);
    void printCommandArguments(std::string_view name);
    void printCommandOptions(std::string_view name);
    void printCommandExamples(std::string_view name);

}
