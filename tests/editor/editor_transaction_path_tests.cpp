#include "app/editor/EditorTransactionPath.h"

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

#include <chrono>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using demo::editor::automation::EditorTransactionDirectoryValidation;
using demo::editor::automation::configuredTemporaryRoot;
using demo::editor::automation::validateEditorTransactionDirectory;

bool expect(bool condition, std::string_view message) {
  if(!condition) {
    std::cerr << "FAILED: " << message << '\n';
  }
  return condition;
}

bool samePath(const fs::path& left, const fs::path& right) {
  std::error_code error;
  const bool equivalent = fs::equivalent(left, right, error);
  return !error && equivalent;
}

class ScopedTestRoot final {
public:
  explicit ScopedTestRoot(const fs::path& configuredRoot) {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    for(unsigned int attempt = 0; attempt != 100u; ++attempt) {
      m_path = configuredRoot /
               ("mgif-editor-transaction-path-tests-" +
                std::to_string(ticks) + "-" + std::to_string(attempt));
      std::error_code error;
      if(fs::create_directory(m_path, error)) {
        return;
      }
      if(error && error != std::errc::file_exists) {
        throw std::runtime_error("Could not create editor transaction test root: " +
                                 error.message());
      }
    }
    throw std::runtime_error("Could not allocate a unique editor transaction test root");
  }

  ~ScopedTestRoot() {
    std::error_code ignored;
    fs::remove_all(m_path, ignored);
  }

  [[nodiscard]] const fs::path& path() const noexcept { return m_path; }

private:
  fs::path m_path;
};

bool writeFixture(const fs::path& path, std::string_view bytes = "fixture") {
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  if(error) {
    return false;
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return output.good();
}

#ifdef _WIN32
struct MountPointReparseData final {
  DWORD reparseTag;
  WORD reparseDataLength;
  WORD reserved;
  WORD substituteNameOffset;
  WORD substituteNameLength;
  WORD printNameOffset;
  WORD printNameLength;
  wchar_t pathBuffer[1];
};

bool createDirectoryJunction(const fs::path& junction,
                             const fs::path& target,
                             std::string& diagnostic) {
  std::error_code error;
  fs::create_directory(junction, error);
  if(error) {
    diagnostic = error.message();
    return false;
  }

  const fs::path absoluteTarget = fs::absolute(target, error).lexically_normal();
  if(error || absoluteTarget.empty()) {
    diagnostic = error ? error.message() : "junction target was empty";
    fs::remove(junction, error);
    return false;
  }
  const std::wstring printName = absoluteTarget.native();
  const std::wstring substituteName = L"\\??\\" + printName;
  const std::size_t substituteBytes = substituteName.size() * sizeof(wchar_t);
  const std::size_t printBytes = printName.size() * sizeof(wchar_t);
  const std::size_t pathBytes = substituteBytes + sizeof(wchar_t) +
                                printBytes + sizeof(wchar_t);
  const std::size_t inputBytes = offsetof(MountPointReparseData, pathBuffer) +
                                 pathBytes;
  if(substituteBytes > (std::numeric_limits<WORD>::max)() ||
     printBytes > (std::numeric_limits<WORD>::max)() ||
     inputBytes - 8u > (std::numeric_limits<WORD>::max)()) {
    diagnostic = "junction fixture path was too long";
    fs::remove(junction, error);
    return false;
  }

  std::vector<std::byte> storage(inputBytes);
  auto* data = reinterpret_cast<MountPointReparseData*>(storage.data());
  data->reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
  data->reparseDataLength = static_cast<WORD>(inputBytes - 8u);
  data->reserved = 0u;
  data->substituteNameOffset = 0u;
  data->substituteNameLength = static_cast<WORD>(substituteBytes);
  data->printNameOffset = static_cast<WORD>(substituteBytes + sizeof(wchar_t));
  data->printNameLength = static_cast<WORD>(printBytes);
  std::memcpy(data->pathBuffer, substituteName.c_str(),
              substituteBytes + sizeof(wchar_t));
  std::memcpy(reinterpret_cast<std::byte*>(data->pathBuffer) +
                  data->printNameOffset,
              printName.c_str(), printBytes + sizeof(wchar_t));

  const HANDLE handle = CreateFileW(
      junction.c_str(), GENERIC_WRITE, 0u, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if(handle == INVALID_HANDLE_VALUE) {
    diagnostic = std::system_category().message(static_cast<int>(GetLastError()));
    fs::remove(junction, error);
    return false;
  }
  DWORD returned = 0u;
  const BOOL created = DeviceIoControl(
      handle, FSCTL_SET_REPARSE_POINT, data, static_cast<DWORD>(inputBytes),
      nullptr, 0u, &returned, nullptr);
  const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
  CloseHandle(handle);
  if(!created) {
    diagnostic = std::system_category().message(static_cast<int>(createError));
    fs::remove(junction, error);
    return false;
  }
  return true;
}

constexpr DWORD kConfiguredRootRejectedExitCode = 23u;

class NativeHandle final {
public:
  explicit NativeHandle(HANDLE value = nullptr) noexcept : m_value(value) {}

  ~NativeHandle() {
    if(valid()) {
      CloseHandle(m_value);
    }
  }

  NativeHandle(const NativeHandle&) = delete;
  NativeHandle& operator=(const NativeHandle&) = delete;

  [[nodiscard]] HANDLE get() const noexcept { return m_value; }
  [[nodiscard]] bool valid() const noexcept {
    return m_value != nullptr && m_value != INVALID_HANDLE_VALUE;
  }

private:
  HANDLE m_value{nullptr};
};

[[nodiscard]] std::string windowsError(DWORD error) {
  return std::system_category().message(static_cast<int>(error)) +
         " (Win32 error " + std::to_string(error) + ")";
}

[[nodiscard]] std::optional<std::wstring> readEnvironmentVariable(
    const wchar_t* name) {
  SetLastError(ERROR_SUCCESS);
  const DWORD required = GetEnvironmentVariableW(name, nullptr, 0u);
  if(required == 0u) {
    const DWORD error = GetLastError();
    if(error == ERROR_ENVVAR_NOT_FOUND) {
      return std::nullopt;
    }
    if(error == ERROR_SUCCESS) {
      return std::wstring{};
    }
    throw std::runtime_error("Could not read process environment: " +
                             windowsError(error));
  }

  std::wstring value(required, L'\0');
  const DWORD length = GetEnvironmentVariableW(name, value.data(), required);
  if(length == 0u || length >= required) {
    throw std::runtime_error("Process environment changed while being read");
  }
  value.resize(length);
  return value;
}

class ScopedEnvironmentVariable final {
public:
  ScopedEnvironmentVariable(const wchar_t* name, const std::wstring& value)
      : m_name(name), m_previous(readEnvironmentVariable(name)) {
    if(!SetEnvironmentVariableW(m_name.c_str(), value.c_str())) {
      throw std::runtime_error("Could not set child process environment: " +
                               windowsError(GetLastError()));
    }
  }

  ~ScopedEnvironmentVariable() {
    (void)SetEnvironmentVariableW(
        m_name.c_str(), m_previous ? m_previous->c_str() : nullptr);
  }

  ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
  ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
  std::wstring m_name;
  std::optional<std::wstring> m_previous;
};

[[nodiscard]] fs::path currentExecutablePath() {
  std::wstring value(32768u, L'\0');
  const DWORD length = GetModuleFileNameW(
      nullptr, value.data(), static_cast<DWORD>(value.size()));
  if(length == 0u || length >= value.size()) {
    throw std::runtime_error("Could not resolve test executable: " +
                             windowsError(GetLastError()));
  }
  value.resize(length);
  return fs::path{std::move(value)};
}

[[nodiscard]] std::string readLog(const fs::path& logPath) {
  std::ifstream input(logPath, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

bool runConfiguredRootChild(const fs::path& executable,
                            const fs::path& workingDirectory,
                            const fs::path& logPath,
                            const std::wstring& tempValue,
                            const std::wstring& tmpValue,
                            std::string_view requiredDiagnostic,
                            std::string_view caseName) {
  DWORD exitCode = 0u;
  {
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    NativeHandle log{CreateFileW(
        logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inheritable,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    NativeHandle input{CreateFileW(L"NUL", GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &inheritable, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr)};
    if(!log.valid() || !input.valid()) {
      std::cerr << "FAILED: could not open child-process log handles for "
                << caseName << ": " << windowsError(GetLastError()) << '\n';
      return false;
    }

    ScopedEnvironmentVariable temp{L"TEMP", tempValue};
    ScopedEnvironmentVariable tmp{L"TMP", tmpValue};
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input.get();
    startup.hStdOutput = log.get();
    startup.hStdError = log.get();
    PROCESS_INFORMATION process{};
    std::wstring commandLine = L"\"" + executable.native() +
                               L"\" --probe-configured-root";
    if(!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr,
                       TRUE, CREATE_NO_WINDOW, nullptr,
                       workingDirectory.c_str(), &startup, &process)) {
      std::cerr << "FAILED: could not launch configured-root child for "
                << caseName << ": " << windowsError(GetLastError()) << '\n';
      return false;
    }

    NativeHandle processHandle{process.hProcess};
    NativeHandle threadHandle{process.hThread};
    const DWORD waitResult = WaitForSingleObject(processHandle.get(), 30000u);
    if(waitResult == WAIT_TIMEOUT) {
      (void)TerminateProcess(processHandle.get(), 24u);
      (void)WaitForSingleObject(processHandle.get(), 5000u);
      std::cerr << "FAILED: configured-root child timed out for " << caseName
                << '\n';
      return false;
    }
    if(waitResult != WAIT_OBJECT_0 ||
       !GetExitCodeProcess(processHandle.get(), &exitCode)) {
      std::cerr << "FAILED: could not collect configured-root child for "
                << caseName << ": " << windowsError(GetLastError()) << '\n';
      return false;
    }
  }

  const std::string log = readLog(logPath);
  const bool rejected = exitCode == kConfiguredRootRejectedExitCode &&
                        log.find(requiredDiagnostic) != std::string::npos;
  if(!rejected) {
    std::cerr << "FAILED: " << caseName << " environment was not rejected "
              << "by the production resolver (exit " << exitCode
              << ", log: " << log << ")\n";
    return false;
  }
  std::cout << "Child process rejected " << caseName << " environment; log: "
            << logPath.string() << '\n';
  return true;
}

bool runProcessEnvironmentContract(const ScopedTestRoot& fixture) {
  // Keep the child logs beside (rather than inside) the disposable fixture so
  // every run leaves auditable evidence below the configured temporary root.
  const fs::path logRoot = fixture.path().parent_path() /
                           (fixture.path().filename().string() +
                            "-child-process-logs");
  const fs::path relativeRoot = fixture.path() / "relative-env-root";
  const fs::path tempRoot = fixture.path() / "mismatched-temp";
  const fs::path tmpRoot = fixture.path() / "mismatched-tmp";
  std::error_code error;
  fs::create_directories(logRoot, error);
  if(error || !fs::create_directory(relativeRoot, error) || error ||
     !fs::create_directory(tempRoot, error) || error ||
     !fs::create_directory(tmpRoot, error) || error) {
    std::cerr << "FAILED: could not create child-process environment fixtures: "
              << error.message() << '\n';
    return false;
  }

  const fs::path executable = currentExecutablePath();
  bool passed = runConfiguredRootChild(
      executable, fixture.path(), logRoot / "relative-environment.log",
      L"relative-env-root", L"relative-env-root",
      "process TEMP must be an absolute path", "relative TEMP/TMP");
  passed &= runConfiguredRootChild(
      executable, fixture.path(), logRoot / "mismatched-environment.log",
      tempRoot.native(), tmpRoot.native(),
      "Process TEMP and TMP must identify the same existing real directory",
      "mismatched TEMP/TMP");
  return passed;
}

int runConfiguredRootProbe() {
  try {
    const fs::path root = configuredTemporaryRoot();
    std::cout << "ACCEPTED: " << root.string() << '\n';
    return EXIT_SUCCESS;
  } catch(const std::invalid_argument& error) {
    std::cerr << "REJECTED: " << error.what() << '\n';
    return static_cast<int>(kConfiguredRootRejectedExitCode);
  } catch(const std::exception& error) {
    std::cerr << "UNEXPECTED: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
#endif

template <typename Callback>
bool rejects(Callback&& callback, std::string_view message,
             std::string_view requiredDiagnostic = {}) {
  try {
    callback();
  } catch(const std::invalid_argument& error) {
    if(requiredDiagnostic.empty() ||
       std::string_view{error.what()}.find(requiredDiagnostic) !=
           std::string_view::npos) {
      return true;
    }
    std::cerr << "FAILED: " << message << " (diagnostic was: " << error.what()
              << ")\n";
    return false;
  } catch(const std::exception& error) {
    std::cerr << "FAILED: " << message << " (wrong exception: " << error.what()
              << ")\n";
    return false;
  }
  std::cerr << "FAILED: " << message << " (path was accepted)\n";
  return false;
}

bool runPathContract() {
  bool passed = true;
  const fs::path tempRoot = configuredTemporaryRoot();
  passed &= expect(tempRoot.is_absolute(),
                   "the process-configured temporary root is absolute");
  ScopedTestRoot fixture{tempRoot};

  const fs::path source = fixture.path() / "source" / "sponza.gltf";
  passed &= expect(writeFixture(source), "the strict source fixture is created");
#ifdef _WIN32
  passed &= runProcessEnvironmentContract(fixture);
#endif
  if(!passed) {
    return false;
  }

  const fs::path positive = fixture.path() / "positive" / "nested" / "save";
  const EditorTransactionDirectoryValidation accepted =
      validateEditorTransactionDirectory(positive, source);
  std::error_code error;
  passed &= expect(samePath(accepted.root, tempRoot),
                   "production validation uses the configured temporary root");
  passed &= expect(samePath(accepted.directory, positive) &&
                       fs::is_directory(positive, error) && !error,
                   "a fresh nested child below the configured root is accepted and created");
  error.clear();
  passed &= expect(accepted.outputTarget == accepted.directory /
                                                "sponza-transaction.glb" &&
                       !fs::exists(accepted.outputTarget, error) && !error,
                   "the accepted output target is canonical and absent");

  passed &= rejects(
      [&] { (void)validateEditorTransactionDirectory(tempRoot, source); },
      "the configured root itself is rejected", "strict child");

  const fs::path sibling = tempRoot.parent_path() /
                           (fixture.path().filename().string() + "-escape");
  passed &= rejects(
      [&] { (void)validateEditorTransactionDirectory(sibling, source); },
      "a sibling outside the configured root is rejected", "strict child");
  error.clear();
  passed &= expect(!fs::exists(sibling, error) && !error,
                   "the rejected sibling is never created");

  passed &= rejects(
      [&] { (void)validateEditorTransactionDirectory("relative/save", source); },
      "a relative transaction directory is rejected", "absolute");
  passed &= rejects(
      [&] {
        (void)validateEditorTransactionDirectory(positive, source,
                                                 "relative-temp-root");
      },
      "a relative configured temporary root is rejected", "absolute");

  const fs::path nonEmpty = fixture.path() / "non-empty";
  passed &= expect(writeFixture(nonEmpty / "sentinel.txt"),
                   "the non-empty directory fixture is created");
  passed &= rejects(
      [&] { (void)validateEditorTransactionDirectory(nonEmpty, source); },
      "a non-empty transaction directory is rejected", "empty at startup");

  const fs::path existingTarget = fixture.path() / "existing-target";
  passed &= expect(writeFixture(existingTarget / "sponza-transaction.glb"),
                   "the existing output target fixture is created");
  passed &= rejects(
      [&] { (void)validateEditorTransactionDirectory(existingTarget, source); },
      "an existing transaction output target is rejected", "target must be absent");

  passed &= rejects(
      [&] {
        (void)validateEditorTransactionDirectory(source.parent_path(), source);
      },
      "the transaction output directory cannot alias the source directory",
      "cannot alias");

  const fs::path aliasTarget = fixture.path() / "alias-target";
  const fs::path alias = fixture.path() / "alias";
  fs::create_directory(aliasTarget, error);
  passed &= expect(!error, "the symlink target fixture is created");
  error.clear();
  fs::create_directory_symlink(aliasTarget, alias, error);
  bool aliasCreated = !error;
  std::string aliasDiagnostic = error.message();
#ifdef _WIN32
  if(!aliasCreated) {
    std::error_code removeError;
    fs::remove(alias, removeError);
    aliasCreated = createDirectoryJunction(alias, aliasTarget, aliasDiagnostic);
  }
#endif
  if(aliasCreated) {
    const fs::path escapedChild = aliasTarget / "must-not-be-created";
    passed &= rejects(
        [&] { (void)validateEditorTransactionDirectory(alias, source); },
        "a final directory that is a symlink/reparse alias is rejected",
        "symlink or reparse");
    passed &= rejects(
        [&] {
          (void)validateEditorTransactionDirectory(alias / "must-not-be-created" /
                                                       "save",
                                                   source);
        },
        "symlink/reparse traversal is rejected before child creation",
        "symlink or reparse");
    std::error_code existenceError;
    passed &= expect(!fs::exists(escapedChild, existenceError) &&
                         !existenceError,
                     "rejected alias traversal creates nothing through the alias");
    std::error_code removeError;
    fs::remove(alias, removeError);
    passed &= expect(!removeError, "the symlink/reparse fixture is removed safely");
  } else {
    std::cout << "SKIPPED: host could not create a directory symlink/reparse fixture: "
              << aliasDiagnostic << '\n';
  }

  std::cout << "Configured temporary root: " << tempRoot.string() << '\n';
  return passed;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  if(argc == 2 && std::string_view{argv[1]} == "--probe-configured-root") {
    return runConfiguredRootProbe();
  }
#else
  (void)argc;
  (void)argv;
#endif
  try {
    const bool passed = runPathContract();
    if(passed) {
      std::cout << "Editor transaction path isolation tests passed.\n";
    }
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
  } catch(const std::exception& error) {
    std::cerr << "FAILED: unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
