#include <cmd/Command.h>
#include <iostream>
#include <algorithm>

namespace cloth::cmd {

    static const std::vector<CommandStructure> COMMANDS = {
        {
            "build",
            "Build a Cloth project",
            "cloth build [options] <input>",
            {"b"},
            {
                {"help", "h", "Show help for the build command", false, false},
                {"output", "o", "Output file path", true, false},
                {"release", "r", "Enable release mode", false, false}
            },
            {
                "cloth build main.co",
                "cloth build main.co --output main.exe",
                "cloth build main.co --release"
            }
        },
        {
            "run",
            "Run a Cloth project",
            "cloth run [options] <input>",
            {"r"},
            {
                {"help", "h", "Show help for the run command", false, false}
            },
            {
                "cloth run main.co"
            }
        },
        {
            "version",
            "Print Cloth version information",
            "cloth version",
            {"v"},
            {},
            {
                "cloth version"
            }
        },
        {
            "help",
            "Show help information",
            "cloth help [command]",
            {"h"},
            {},
            {
                "cloth help",
                "cloth help build"
            }
        }
    };

    static bool matchesName(std::string_view input, const CommandStructure &command) {
        if (command.name == input) {
            return true;
        }

        return std::find(command.aliases.begin(), command.aliases.end(), input) != command.aliases.end();
    }

    const CommandStructure *getCommand(std::string_view name) {
        for (const auto &command : COMMANDS) {
            if (matchesName(name, command)) {
                return &command;
            }
        }
        return nullptr;
    }

    static const OptionStructure *getOption(const CommandStructure &command, std::string_view token) {
        for (const auto &option : command.options) {
            if (token == ("--" + std::string(option.longName))) {
                return &option;
            }
            if (!option.shortName.empty() && token == ("-" + std::string(option.shortName))) {
                return &option;
            }
        }
        return nullptr;
    }

    ParsedCommand parse(int argc, char **argv) {
        ParsedCommand parsed{};

        if (argc < 2) {
            return parsed;
        }

        const char *commandName = argv[1];
        parsed.command = getCommand(commandName);

        if (!parsed.command) {
            return parsed;
        }

        for (int i = 2; i < argc; ++i) {
            std::string token = argv[i];

            if (!token.empty() && token[0] == '-') {
                const OptionStructure *option = getOption(*parsed.command, token);
                if (!option) {
                    std::cerr << "Unknown option: " << token << '\n';
                    continue;
                }

                ParsedOption parsedOption{};
                parsedOption.name = std::string(option->longName);

                if (option->takesValue) {
                    if (i + 1 >= argc) {
                        std::cerr << "Missing value for option: " << token << '\n';
                    } else {
                        parsedOption.value = std::string(argv[++i]);
                    }
                }

                parsed.options.push_back(std::move(parsedOption));
            } else {
                parsed.positionalArguments.push_back(token);
            }
        }

        return parsed;
    }

    bool hasOption(const ParsedCommand &parsed, std::string_view name) {
        for (const auto &option : parsed.options) {
            if (option.name == name) {
                return true;
            }
        }
        return false;
    }

    std::optional<std::string> getOptionValue(const ParsedCommand &parsed, std::string_view name) {
        for (const auto &option : parsed.options) {
            if (option.name == name) {
                return option.value;
            }
        }
        return std::nullopt;
    }

    void printVersion() {
        std::cout << "Cloth Compiler version 0.1.0\n";
    }

    void printCommands() {
        std::cout << "Commands:\n";
        for (const auto &command : COMMANDS) {
            std::cout << "  " << command.name << " - " << command.description << '\n';
        }
    }

    void printHelp() {
        std::cout << "Cloth Compiler\n";
        std::cout << "Usage: cloth <command> [options]\n\n";
        printCommands();
        std::cout << "\nUse 'cloth help <command>' for more information about a command.\n";
    }

    void printUsage(std::string_view name) {
        const auto *command = getCommand(name);
        if (!command) {
            std::cout << "Unknown command: " << name << '\n';
            return;
        }

        std::cout << "Usage: " << command->usage << '\n';
    }

    void printCommand(std::string_view name) {
        const auto *command = getCommand(name);
        if (!command) {
            std::cout << "Unknown command: " << name << '\n';
            return;
        }

        std::cout << "Command: " << command->name << '\n';
        std::cout << "Description: " << command->description << '\n';
        std::cout << "Usage: " << command->usage << '\n';
    }

    void printCommandHelp(std::string_view name) {
        const auto *command = getCommand(name);
        if (!command) {
            std::cout << "Unknown command: " << name << '\n';
            return;
        }

        printCommand(name);
        printCommandOptions(name);
        printCommandExamples(name);
    }

    void printCommandUsage(std::string_view name) {
        printUsage(name);
    }

    void printCommandVersion(std::string_view) {
        printVersion();
    }

    void printCommandDescription(std::string_view name) {
        const auto *command = getCommand(name);
        if (!command) {
            std::cout << "Unknown command: " << name << '\n';
            return;
        }

        std::cout << command->description << '\n';
    }

    void printCommandArguments(std::string_view name) {
        const auto *command = getCommand(name);
        if (!command) {
            std::cout << "Unknown command: " << name << '\n';
            return;
        }

        std::cout << "Arguments for " << command->name << " are described by usage:\n";
        std::cout << "  " << command->usage << '\n';
    }

    void printCommandOptions(std::string_view name) {
        const auto *command = getCommand(name);
        if (!command) {
            std::cout << "Unknown command: " << name << '\n';
            return;
        }

        std::cout << "\nOptions:\n";
        if (command->options.empty()) {
            std::cout << "  None\n";
            return;
        }

        for (const auto &option : command->options) {
            std::cout << "  --" << option.longName;
            if (!option.shortName.empty()) {
                std::cout << ", -" << option.shortName;
            }
            if (option.takesValue) {
                std::cout << " <value>";
            }
            std::cout << "\n      " << option.description << '\n';
        }
    }

    void printCommandExamples(std::string_view name) {
        const auto *command = getCommand(name);
        if (!command) {
            std::cout << "Unknown command: " << name << '\n';
            return;
        }

        std::cout << "\nExamples:\n";
        if (command->examples.empty()) {
            std::cout << "  None\n";
            return;
        }

        for (const auto &example : command->examples) {
            std::cout << "  " << example << '\n';
        }
    }

}