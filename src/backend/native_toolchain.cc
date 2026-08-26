#include "cloth/backend/native_toolchain.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace cloth {
namespace {

using ProcessResult = std::expected<int, std::string>;

#if defined(_WIN32)

std::wstring quote_windows_argument(std::wstring_view argument) {
  if (!argument.empty() &&
      argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
    return std::wstring{argument};
  }

  std::wstring quoted{L"\""};
  std::size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'\"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(character);
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, L'\\');
    backslashes = 0;
    quoted.push_back(character);
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'\"');
  return quoted;
}

ProcessResult run_process(const std::filesystem::path& executable,
                          std::span<const std::string> arguments) {
  std::wstring command_line = quote_windows_argument(executable.wstring());
  for (const std::string& argument : arguments) {
    command_line.push_back(L' ');
    command_line +=
        quote_windows_argument(std::filesystem::path{argument}.wstring());
  }
  std::vector<wchar_t> mutable_command(command_line.begin(),
                                       command_line.end());
  mutable_command.push_back(L'\0');

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (CreateProcessW(executable.c_str(), mutable_command.data(), nullptr,
                     nullptr, FALSE, 0, nullptr, nullptr, &startup,
                     &process) == FALSE) {
    return std::unexpected{"could not start process (Windows error " +
                           std::to_string(GetLastError()) + ")"};
  }
  const DWORD wait_result = WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 0;
  const BOOL read_exit_code = GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (wait_result != WAIT_OBJECT_0 || read_exit_code == FALSE) {
    return std::unexpected{"could not wait for process completion"};
  }
  if (exit_code > static_cast<DWORD>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(exit_code);
}

#else

ProcessResult run_process(const std::filesystem::path& executable,
                          std::span<const std::string> arguments) {
  std::vector<std::string> owned_arguments;
  owned_arguments.reserve(arguments.size() + 1);
  owned_arguments.push_back(executable.string());
  owned_arguments.insert(owned_arguments.end(), arguments.begin(),
                         arguments.end());
  std::vector<char*> native_arguments;
  native_arguments.reserve(owned_arguments.size() + 1);
  for (std::string& argument : owned_arguments) {
    native_arguments.push_back(argument.data());
  }
  native_arguments.push_back(nullptr);

  const pid_t child = fork();
  if (child < 0) {
    return std::unexpected{
        "could not create child process: " +
        std::error_code{errno, std::generic_category()}.message()};
  }
  if (child == 0) {
    execv(native_arguments[0], native_arguments.data());
    _exit(127);
  }

  int status = 0;
  pid_t waited = 0;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited < 0) {
    return std::unexpected{
        "could not wait for child process: " +
        std::error_code{errno, std::generic_category()}.message()};
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return std::unexpected{"child process ended with an unknown status"};
}

#endif

class TemporaryFiles {
 public:
  TemporaryFiles(std::filesystem::path ir, std::filesystem::path object)
      : ir_(std::move(ir)), object_(std::move(object)) {}

  ~TemporaryFiles() {
    std::error_code error;
    static_cast<void>(std::filesystem::remove(ir_, error));
    error.clear();
    static_cast<void>(std::filesystem::remove(object_, error));
  }

  TemporaryFiles(const TemporaryFiles&) = delete;
  TemporaryFiles& operator=(const TemporaryFiles&) = delete;

 private:
  std::filesystem::path ir_;
  std::filesystem::path object_;
};

std::expected<void, NativeBuildError> validate_tool(
    const std::filesystem::path& path, std::string_view description) {
  std::error_code error;
  if (path.empty() || !std::filesystem::is_regular_file(path, error)) {
    return std::unexpected(NativeBuildError{std::string{description} +
                                            " was not found at '" +
                                            path.generic_string() + "'"});
  }
  return {};
}

std::string unique_suffix() {
  static std::atomic<std::uint64_t> sequence{0};
  const auto timestamp = std::chrono::steady_clock::now().time_since_epoch();
  const auto ticks =
      std::chrono::duration_cast<std::chrono::nanoseconds>(timestamp).count();
  return std::to_string(ticks) + "." +
         std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

std::expected<void, NativeBuildError> run_stage(
    std::string_view stage, const std::filesystem::path& executable,
    const std::vector<std::string>& arguments) {
  const ProcessResult result = run_process(executable, arguments);
  if (!result) {
    return std::unexpected(
        NativeBuildError{std::string{stage} + ": " + result.error()});
  }
  if (*result != 0) {
    return std::unexpected(NativeBuildError{std::string{stage} +
                                            " failed with exit code " +
                                            std::to_string(*result)});
  }
  return {};
}

}  // namespace

std::expected<void, NativeBuildError> build_native_executable(
    const LlvmIrModule& module, const std::filesystem::path& output_path,
    const NativeToolchain& toolchain) {
  if (output_path.empty()) {
    return std::unexpected(NativeBuildError{"native output path is empty"});
  }
  if (toolchain.target_triple.empty()) {
    return std::unexpected(
        NativeBuildError{"native target triple is not configured"});
  }
  if (auto result = validate_tool(toolchain.llc, "LLVM llc"); !result) {
    return result;
  }
  if (auto result = validate_tool(toolchain.linker, "native linker"); !result) {
    return result;
  }
  if (auto result =
          validate_tool(toolchain.runtime_library, "Cloth runtime library");
      !result) {
    return result;
  }

  const std::filesystem::path output_directory =
      output_path.has_parent_path() ? output_path.parent_path()
                                    : std::filesystem::current_path();
  std::error_code error;
  if (!std::filesystem::is_directory(output_directory, error)) {
    return std::unexpected(
        NativeBuildError{"native output directory does not exist: '" +
                         output_directory.generic_string() + "'"});
  }

  const std::string temporary_base =
      output_path.filename().generic_string() + ".cloth." + unique_suffix();
  const std::filesystem::path ir_path =
      output_directory / (temporary_base + ".ll");
#if defined(_WIN32)
  const std::filesystem::path object_path =
      output_directory / (temporary_base + ".obj");
#else
  const std::filesystem::path object_path =
      output_directory / (temporary_base + ".o");
#endif
  TemporaryFiles temporary_files{ir_path, object_path};

  std::ofstream ir_output{ir_path, std::ios::binary};
  ir_output << module.text;
  ir_output.close();
  if (!ir_output) {
    return std::unexpected(
        NativeBuildError{"could not write temporary LLVM IR: '" +
                         ir_path.generic_string() + "'"});
  }

  std::vector<std::string> llc_arguments{
      "-filetype=obj", "-mtriple=" + toolchain.target_triple, "-o",
      object_path.string(), ir_path.string()};
  if (auto result =
          run_stage("LLVM object emission", toolchain.llc, llc_arguments);
      !result) {
    return result;
  }

  std::vector<std::string> linker_arguments;
  if (toolchain.linker_flavor == NativeLinkerFlavor::kMsvc) {
    linker_arguments = {"/nologo", object_path.string(),
                        toolchain.runtime_library.string(),
                        "/Fe:" + output_path.string()};
  } else {
    if (toolchain.link_static_runtime) {
      linker_arguments = {"-static"};
    }
    linker_arguments.insert(
        linker_arguments.end(),
        {object_path.string(), toolchain.runtime_library.string(), "-o",
         output_path.string()});
  }
  return run_stage("native linking", toolchain.linker, linker_arguments);
}

}  // namespace cloth
