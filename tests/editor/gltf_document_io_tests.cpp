#include <mgif/scene_editor/GltfDocumentIO.h>
#include <mgif/scene_editor/GltfSemanticCompare.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

using namespace demo::editor;

#ifdef _WIN32
[[nodiscard]] std::wstring windowsApiPath(const std::filesystem::path& path);
#endif

bool expect(bool condition, std::string_view message)
{
	if(!condition)
	{
		std::cerr << "FAILED: " << message << '\n';
	}
	return condition;
}

class TempDirectory final
{
public:
	TempDirectory()
	{
		static std::atomic<std::uint64_t> sequence{0};
		const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
		m_path = std::filesystem::temp_directory_path()
			/ ("mgif-p1-io-" + std::to_string(ticks) + '-'
				+ std::to_string(sequence.fetch_add(1)));
		std::filesystem::create_directories(m_path);
	}

	~TempDirectory()
	{
		std::error_code ignored;
		std::filesystem::permissions(m_path,
			std::filesystem::perms::owner_all,
			std::filesystem::perm_options::add,
			ignored);
#ifdef _WIN32
		std::filesystem::remove_all(std::filesystem::path(windowsApiPath(m_path)), ignored);
#else
		std::filesystem::remove_all(m_path, ignored);
#endif
	}

	[[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

private:
	std::filesystem::path m_path;
};

class FailAt final : public EditorFaultInjector
{
public:
	explicit FailAt(EditorFaultPoint point) : m_point(point) {}
	[[nodiscard]] bool shouldFail(EditorFaultPoint point) noexcept override
	{
		if(point != m_point) return false;
		++m_hitCount;
		return true;
	}
	[[nodiscard]] std::size_t hitCount() const noexcept { return m_hitCount; }

private:
	EditorFaultPoint m_point;
	std::size_t m_hitCount{0};
};

[[nodiscard]] std::filesystem::path fixture(std::string_view name)
{
	return std::filesystem::path(MGIF_TEST_SOURCE_DIR) / "tests" / "fixtures" / "gltf_editor" / name;
}

bool writeText(const std::filesystem::path& path, std::string_view text)
{
	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	stream.write(text.data(), static_cast<std::streamsize>(text.size()));
	stream.close();
	return !stream.fail();
}

bool writeBytes(const std::filesystem::path& path, const std::vector<unsigned char>& bytes)
{
	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	stream.close();
	return !stream.fail();
}

[[nodiscard]] std::vector<unsigned char> readBytes(const std::filesystem::path& path)
{
	std::ifstream stream(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

#ifdef _WIN32

class NativeHandle final
{
public:
	explicit NativeHandle(HANDLE value = nullptr) noexcept : m_value(value) {}
	~NativeHandle()
	{
		if(m_value != nullptr && m_value != INVALID_HANDLE_VALUE)
		{
			CloseHandle(m_value);
		}
	}

	NativeHandle(const NativeHandle&) = delete;
	NativeHandle& operator=(const NativeHandle&) = delete;
	NativeHandle(NativeHandle&& other) noexcept : m_value(std::exchange(other.m_value, nullptr)) {}
	NativeHandle& operator=(NativeHandle&& other) noexcept
	{
		if(this != &other)
		{
			if(valid()) CloseHandle(m_value);
			m_value = std::exchange(other.m_value, nullptr);
		}
		return *this;
	}

	[[nodiscard]] HANDLE get() const noexcept { return m_value; }
	[[nodiscard]] bool valid() const noexcept
	{
		return m_value != nullptr && m_value != INVALID_HANDLE_VALUE;
	}

private:
	HANDLE m_value{nullptr};
};

struct ChildProcess final
{
	NativeHandle process;
	NativeHandle thread;

	~ChildProcess()
	{
		if(process.valid() && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT)
		{
			(void)TerminateProcess(process.get(), 0x4d474946u);
			(void)WaitForSingleObject(process.get(), 5000);
		}
	}
};

[[nodiscard]] std::wstring windowsApiPath(const std::filesystem::path& path)
{
	std::wstring value = path.native();
	if(value.starts_with(L"\\\\?\\") || value.starts_with(L"\\\\.\\")) return value;
	if(value.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + value.substr(2);
	return L"\\\\?\\" + value;
}

[[nodiscard]] std::wstring quoteWindowsArgument(std::wstring_view argument)
{
	std::wstring result(1, L'"');
	std::size_t backslashes = 0;
	for(const wchar_t character : argument)
	{
		if(character == L'\\')
		{
			++backslashes;
			continue;
		}
		if(character == L'"')
		{
			result.append(backslashes * 2u + 1u, L'\\');
			result.push_back(L'"');
			backslashes = 0;
			continue;
		}
		result.append(backslashes, L'\\');
		backslashes = 0;
		result.push_back(character);
	}
	result.append(backslashes * 2u, L'\\');
	result.push_back(L'"');
	return result;
}

[[nodiscard]] std::string windowsError(DWORD code)
{
	return std::error_code(static_cast<int>(code), std::system_category()).message()
		+ " (Win32 error " + std::to_string(code) + ')';
}

[[nodiscard]] bool currentExecutablePath(std::filesystem::path& executable, std::string& error)
{
	std::wstring buffer(32768, L'\0');
	const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	if(length == 0 || length >= buffer.size())
	{
		error = "GetModuleFileNameW failed: " + windowsError(GetLastError());
		return false;
	}
	buffer.resize(length);
	executable = std::filesystem::path(std::move(buffer));
	return true;
}

[[nodiscard]] bool launchAtomicReplaceChild(
	const std::filesystem::path& executable,
	const std::filesystem::path& target,
	const std::filesystem::path& ready,
	const std::filesystem::path& release,
	const std::filesystem::path& done,
	ChildProcess& child,
	std::string& error)
{
	std::wstring commandLine = quoteWindowsArgument(executable.native());
	for(const std::wstring_view argument : {
		std::wstring_view(L"--atomic-replace-child"),
		std::wstring_view(target.native()),
		std::wstring_view(ready.native()),
		std::wstring_view(release.native()),
		std::wstring_view(done.native()),
	})
	{
		commandLine.push_back(L' ');
		commandLine += quoteWindowsArgument(argument);
	}

	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process{};
	const std::wstring executablePath = windowsApiPath(executable);
	if(!CreateProcessW(
		executablePath.c_str(),
		commandLine.data(),
		nullptr,
		nullptr,
		FALSE,
		CREATE_NO_WINDOW,
		nullptr,
		nullptr,
		&startup,
		&process))
	{
		error = "CreateProcessW failed: " + windowsError(GetLastError());
		return false;
	}
	child.process = NativeHandle(process.hProcess);
	child.thread = NativeHandle(process.hThread);
	return true;
}

[[nodiscard]] bool waitForSignal(
	const std::filesystem::path& signal,
	HANDLE process,
	std::chrono::seconds timeout,
	std::string& error)
{
	const std::wstring signalPath = windowsApiPath(signal);
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while(std::chrono::steady_clock::now() < deadline)
	{
		if(GetFileAttributesW(signalPath.c_str()) != INVALID_FILE_ATTRIBUTES)
		{
			return true;
		}
		const DWORD attributeError = GetLastError();
		if(attributeError != ERROR_FILE_NOT_FOUND && attributeError != ERROR_PATH_NOT_FOUND)
		{
			error = "cannot inspect child signal: " + windowsError(attributeError);
			return false;
		}
		if(WaitForSingleObject(process, 0) != WAIT_TIMEOUT)
		{
			DWORD exitCode = 0;
			(void)GetExitCodeProcess(process, &exitCode);
			error = "atomic replacement child exited before its signal (exit "
				+ std::to_string(exitCode) + ')';
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	error = "timed out waiting for the atomic replacement child signal";
	return false;
}

[[nodiscard]] bool terminateChild(ChildProcess& child, std::string& error)
{
	if(WaitForSingleObject(child.process.get(), 0) != WAIT_TIMEOUT)
	{
		DWORD exitCode = 0;
		(void)GetExitCodeProcess(child.process.get(), &exitCode);
		error = "atomic replacement child exited before forced termination (exit "
			+ std::to_string(exitCode) + ')';
		return false;
	}
	if(!TerminateProcess(child.process.get(), 0x4d474946u))
	{
		const DWORD terminateError = GetLastError();
		DWORD exitCode = STILL_ACTIVE;
		(void)GetExitCodeProcess(child.process.get(), &exitCode);
		error = "TerminateProcess failed: " + windowsError(terminateError)
			+ "; child exit code is " + std::to_string(exitCode);
		return false;
	}
	if(WaitForSingleObject(child.process.get(), 10000) != WAIT_OBJECT_0)
	{
		error = "terminated atomic replacement child did not stop within 10 seconds";
		return false;
	}
	return true;
}

[[nodiscard]] bool readSharedBytes(
	const std::filesystem::path& path,
	std::vector<unsigned char>& bytes,
	std::string& error)
{
	const std::wstring nativePath = windowsApiPath(path);
	NativeHandle file;
	const auto openDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
	for(;;)
	{
		file = NativeHandle(CreateFileW(
			nativePath.c_str(),
			GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
			nullptr));
		if(file.valid()) break;
		const DWORD openError = GetLastError();
		if(openError != ERROR_SHARING_VIOLATION && openError != ERROR_LOCK_VIOLATION)
		{
			error = "the selected target could not be opened: " + windowsError(openError);
			return false;
		}
		if(GetFileAttributesW(nativePath.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			const DWORD attributeError = GetLastError();
			error = "the selected target disappeared while its replacement was locked: "
				+ windowsError(attributeError);
			return false;
		}
		if(std::chrono::steady_clock::now() >= openDeadline)
		{
			error = "the selected target remained exclusively locked for more than 250 ms";
			return false;
		}
		std::this_thread::yield();
	}
	LARGE_INTEGER size{};
	if(!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 || size.QuadPart > 64ll * 1024ll * 1024ll)
	{
		error = "the selected target has an invalid or unbounded size";
		return false;
	}
	bytes.resize(static_cast<std::size_t>(size.QuadPart));
	std::size_t offset = 0;
	while(offset < bytes.size())
	{
		const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
			bytes.size() - offset,
			static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
		DWORD read = 0;
		if(!ReadFile(file.get(), bytes.data() + offset, chunk, &read, nullptr))
		{
			error = "reading the selected target failed: " + windowsError(GetLastError());
			return false;
		}
		if(read == 0)
		{
			error = "reading the selected target ended before its captured size";
			return false;
		}
		offset += read;
	}
	return true;
}

enum class ObservedTargetVersion : std::uint8_t
{
	oldBytes,
	newBytes
};

[[nodiscard]] bool observeCompleteTarget(
	const std::filesystem::path& target,
	const std::vector<unsigned char>& oldBytes,
	const std::vector<unsigned char>& newBytes,
	ObservedTargetVersion& observed,
	std::string& error)
{
	std::vector<unsigned char> actual;
	if(!readSharedBytes(target, actual, error)) return false;
	if(actual == oldBytes)
	{
		observed = ObservedTargetVersion::oldBytes;
		return true;
	}
	if(actual == newBytes)
	{
		observed = ObservedTargetVersion::newBytes;
		return true;
	}
	error = "the selected target contained missing, partial, or mixed publication bytes";
	return false;
}

class PauseAtAtomicReplace final : public EditorFaultInjector
{
public:
	PauseAtAtomicReplace(
		std::filesystem::path ready,
		std::filesystem::path release)
		: m_ready(std::move(ready)), m_release(std::move(release))
	{
	}

	[[nodiscard]] bool shouldFail(EditorFaultPoint point) noexcept override
	{
		if(point != EditorFaultPoint::preAtomicReplace) return false;
		try
		{
			if(!writeText(m_ready, "ready")) return true;
			const std::wstring releasePath = windowsApiPath(m_release);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
			while(std::chrono::steady_clock::now() < deadline)
			{
				if(GetFileAttributesW(releasePath.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
				const DWORD error = GetLastError();
				if(error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return true;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
		catch(...)
		{
		}
		return true;
	}

private:
	std::filesystem::path m_ready;
	std::filesystem::path m_release;
};

#endif

bool copyFile(const std::filesystem::path& source, const std::filesystem::path& target)
{
	std::error_code ec;
	return std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, ec) && !ec;
}

[[nodiscard]] std::unique_ptr<GltfDocument> load(
	const std::filesystem::path& path,
	EditorFaultInjector* faultInjector = nullptr)
{
	GltfLoadOptions options;
	options.faultInjector = faultInjector;
	auto result = GltfDocumentIO::loadCandidate(path, options);
	if(!result.succeeded()) return {};
	std::unique_ptr<GltfDocument> document;
	(void)std::move(result).publishTo(document);
	return document;
}

struct DocumentSnapshot final
{
	std::filesystem::path sourcePath;
	GltfDocumentFormat format{};
	std::uint64_t revision{0};
	std::uint64_t cleanRevision{0};
	SceneId currentScene;
	std::string digest;
	std::vector<EditorObjectId> ids;
	std::vector<GltfDependency> dependencies;
};

[[nodiscard]] DocumentSnapshot snapshot(const GltfDocument& document)
{
	DocumentSnapshot result;
	result.sourcePath = document.sourcePath();
	result.format = document.format();
	result.revision = document.revision();
	result.cleanRevision = document.cleanRevision();
	result.currentScene = document.currentSceneId();
	result.digest = GltfSemanticCompare::digest(document);
	for(std::uint8_t raw = static_cast<std::uint8_t>(EditorObjectKind::scene);
		raw < static_cast<std::uint8_t>(EditorObjectKind::count);
		++raw)
	{
		const auto kind = static_cast<EditorObjectKind>(raw);
		for(std::size_t index = 0; index < document.objectCount(kind); ++index)
		{
			result.ids.push_back(document.idAt(kind, index));
		}
	}
	result.dependencies.assign(document.dependencies().begin(), document.dependencies().end());
	return result;
}

bool unchanged(const GltfDocument& document, const DocumentSnapshot& before)
{
	if(document.sourcePath() != before.sourcePath || document.format() != before.format
		|| document.revision() != before.revision || document.cleanRevision() != before.cleanRevision
		|| document.currentSceneId() != before.currentScene
		|| GltfSemanticCompare::digest(document) != before.digest
		|| document.dependencies().size() != before.dependencies.size())
	{
		return false;
	}
	std::size_t idOffset = 0;
	for(std::uint8_t raw = static_cast<std::uint8_t>(EditorObjectKind::scene);
		raw < static_cast<std::uint8_t>(EditorObjectKind::count);
		++raw)
	{
		const auto kind = static_cast<EditorObjectKind>(raw);
		for(std::size_t index = 0; index < document.objectCount(kind); ++index)
		{
			if(idOffset >= before.ids.size() || document.idAt(kind, index) != before.ids[idOffset++]) return false;
		}
	}
	for(std::size_t index = 0; index < before.dependencies.size(); ++index)
	{
		const auto& left = document.dependencies()[index];
		const auto& right = before.dependencies[index];
		if(left.kind != right.kind || left.objectIndex != right.objectIndex || left.uri != right.uri
			|| left.resolvedPath != right.resolvedPath || left.byteSize != right.byteSize) return false;
	}
	return idOffset == before.ids.size();
}

[[nodiscard]] std::filesystem::path testIoPath(const std::filesystem::path& path)
{
#ifdef _WIN32
	return std::filesystem::path(windowsApiPath(path));
#else
	return path;
#endif
}

[[nodiscard]] bool accessibleRegularFile(const std::filesystem::path& path)
{
	std::error_code error;
	return std::filesystem::is_regular_file(testIoPath(path), error) && !error;
}

[[nodiscard]] std::filesystem::path moderatelyLongTarget(
	const std::filesystem::path& root,
	bool unicode,
	const std::filesystem::path& filename,
	std::size_t minimumCanonicalChars)
{
	std::error_code error;
	const std::filesystem::path canonicalRoot = std::filesystem::canonical(root, error);
	if(error) return {};

	std::filesystem::path relative = unicode
		? std::filesystem::path(u8"Unicode-路径-U0001F4BE")
		: std::filesystem::path("ascii-only");
	for(std::size_t index = 0;
		(canonicalRoot / relative / filename).native().size() < minimumCanonicalChars;
		++index)
	{
		relative /= "moderately-long-segment-" + std::to_string(index);
	}
	const std::filesystem::path directory = root / relative;
	std::filesystem::create_directories(testIoPath(directory), error);
	return error ? std::filesystem::path{} : directory / filename;
}

[[nodiscard]] bool hasPublishedDependency(
	const GltfDocument& document,
	GltfDependencyKind kind)
{
	return std::any_of(
		document.dependencies().begin(),
		document.dependencies().end(),
		[&](const GltfDependency& dependency) {
			return dependency.kind == kind
				&& dependency.resolvedPath.parent_path() == document.sourcePath().parent_path()
				&& accessibleRegularFile(dependency.resolvedPath);
		});
}

bool noSaveDebris(const std::filesystem::path& directory, bool rejectPublishedResources = true)
{
	for(const auto& entry : std::filesystem::directory_iterator(directory))
	{
		const std::string name = entry.path().filename().string();
		if(name.starts_with(".mgif-s-") || name.starts_with(".mgif-b-")
			|| (rejectPublishedResources
				&& (name.starts_with("mgif-buffer-") || name.starts_with("mgif-image-"))))
		{
			return false;
		}
	}
	return true;
}

#ifdef _WIN32

[[nodiscard]] int runAtomicReplaceChild(
	const std::filesystem::path& target,
	const std::filesystem::path& ready,
	const std::filesystem::path& release,
	const std::filesystem::path& done)
{
	auto document = load(fixture("multi_scene_oracle.gltf"));
	if(!document)
	{
		(void)writeText(done, "load failed");
		return 20;
	}
	PauseAtAtomicReplace injector(ready, release);
	GltfSaveOptions options;
	options.faultInjector = &injector;
	const GltfSaveResult result = GltfDocumentIO::saveAs(*document, target, options);
	(void)writeText(done, result.succeeded() ? "success" : result.message());
	if(!result.succeeded()) return 21;
	const auto exitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while(std::chrono::steady_clock::now() < exitDeadline)
	{
		Sleep(1000);
	}
	return 0;
}

[[nodiscard]] bool waitForDoneWhileObserving(
	const std::filesystem::path& done,
	HANDLE process,
	const std::filesystem::path& target,
	const std::vector<unsigned char>& oldBytes,
	const std::vector<unsigned char>& newBytes,
	std::size_t& oldObservations,
	std::size_t& newObservations,
	std::size_t& totalObservations,
	std::string& error)
{
	const std::wstring donePath = windowsApiPath(done);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while(std::chrono::steady_clock::now() < deadline)
	{
		ObservedTargetVersion observed{};
		if(!observeCompleteTarget(target, oldBytes, newBytes, observed, error)) return false;
		if(observed == ObservedTargetVersion::oldBytes) ++oldObservations;
		else ++newObservations;
		++totalObservations;
		if(GetFileAttributesW(donePath.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
		const DWORD attributeError = GetLastError();
		if(attributeError != ERROR_FILE_NOT_FOUND && attributeError != ERROR_PATH_NOT_FOUND)
		{
			error = "cannot inspect child completion signal: " + windowsError(attributeError);
			return false;
		}
		if(WaitForSingleObject(process, 0) != WAIT_TIMEOUT)
		{
			error = "atomic replacement child exited before the parent terminated it";
			return false;
		}
		std::this_thread::yield();
	}
	error = "timed out while continuously observing an atomic replacement child";
	return false;
}

#endif

bool testAtomicReplacementInterruption()
{
#ifndef _WIN32
	std::cout << "SKIP: Windows atomic replacement interruption coverage is unsupported on this platform\n";
	return true;
#else
	TempDirectory temporary;
	auto expectedDocument = load(fixture("multi_scene_oracle.gltf"));
	if(!expectedDocument) return false;
	const auto expectedPath = temporary.path() / "expected-new.gltf";
	const GltfSaveResult expectedSave = GltfDocumentIO::saveAs(*expectedDocument, expectedPath);
	bool passed = expect(expectedSave.succeeded(), "interruption oracle Save As succeeds");
	if(!expectedSave.succeeded())
	{
		std::cerr << expectedSave.message() << '\n';
		return false;
	}
	const std::vector<unsigned char> newBytes = readBytes(expectedPath);
	const std::string oldDocument =
		R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[]}]})";
	std::vector<unsigned char> oldBytes(oldDocument.begin(), oldDocument.end());
	oldBytes.resize(512u * 1024u, static_cast<unsigned char>(' '));
	const auto oldOraclePath = temporary.path() / "expected-old.gltf";
	passed &= expect(writeBytes(oldOraclePath, oldBytes) && load(oldOraclePath) != nullptr,
	                 "the complete old interruption oracle is a valid reloadable glTF document");
	passed &= expect(!newBytes.empty() && newBytes != oldBytes,
	                 "interruption oracle versions are complete and distinguishable");
	if(!passed) return false;

	std::filesystem::path executable;
	std::string processError;
	if(!currentExecutablePath(executable, processError))
	{
		std::cerr << processError << '\n';
		return false;
	}

	constexpr std::size_t trialCount = 12;
	std::size_t oldObservations = 0;
	std::size_t newObservations = 0;
	std::size_t totalObservations = 0;
	for(std::size_t trial = 0; trial < trialCount; ++trial)
	{
		const auto trialDirectory = temporary.path() / ("interrupt-" + std::to_string(trial));
		std::filesystem::create_directory(trialDirectory);
		const auto target = trialDirectory / std::filesystem::path(u8"selected-目标.gltf");
		const auto ready = trialDirectory / "at-replace-boundary.signal";
		const auto release = trialDirectory / "release-replace.signal";
		const auto done = trialDirectory / "replace-returned.signal";
		passed &= expect(writeBytes(target, oldBytes), "interruption trial creates the complete old target");
		if(!passed) return false;

		ChildProcess child;
		if(!launchAtomicReplaceChild(executable, target, ready, release, done, child, processError))
		{
			std::cerr << processError << '\n';
			return false;
		}
		if(!waitForSignal(ready, child.process.get(), std::chrono::seconds(10), processError))
		{
			std::cerr << processError << '\n';
			return false;
		}

		ObservedTargetVersion observed{};
		passed &= expect(
			observeCompleteTarget(target, oldBytes, newBytes, observed, processError)
				&& observed == ObservedTargetVersion::oldBytes,
			"the actual Windows atomic replacement boundary still exposes the complete old target");
		++oldObservations;
		++totalObservations;
		if(!passed)
		{
			std::cerr << processError << '\n';
			return false;
		}

		if(trial != 0)
		{
			if(!writeText(release, "release")) return false;
			if(trial + 4u >= trialCount)
			{
				if(!waitForDoneWhileObserving(
					done,
					child.process.get(),
					target,
					oldBytes,
					newBytes,
					oldObservations,
					newObservations,
					totalObservations,
					processError))
				{
					std::cerr << processError << '\n';
					return false;
				}
			}
			else
			{
				const std::size_t samples = trial * 4u;
				for(std::size_t sample = 0; sample < samples; ++sample)
				{
					if(!observeCompleteTarget(target, oldBytes, newBytes, observed, processError))
					{
						std::cerr << processError << '\n';
						return false;
					}
					if(observed == ObservedTargetVersion::oldBytes) ++oldObservations;
					else ++newObservations;
					++totalObservations;
					std::this_thread::yield();
				}
			}
		}

		if(!terminateChild(child, processError))
		{
			std::cerr << "interruption trial " << trial << ": " << processError;
			if(std::filesystem::is_regular_file(done))
			{
				const std::vector<unsigned char> childResult = readBytes(done);
				std::cerr << "; child result: "
					<< std::string(childResult.begin(), childResult.end());
			}
			std::cerr << '\n';
			return false;
		}
		for(std::size_t sample = 0; sample < 8; ++sample)
		{
			if(!observeCompleteTarget(target, oldBytes, newBytes, observed, processError))
			{
				std::cerr << processError << '\n';
				return false;
			}
			if(observed == ObservedTargetVersion::oldBytes) ++oldObservations;
			else ++newObservations;
			++totalObservations;
		}
	}

	passed &= expect(totalObservations >= trialCount * 9u,
	                 "bounded interruption stress continuously observes every trial");
	passed &= expect(oldObservations != 0 && newObservations != 0,
	                 "interruption stress observes both complete old and complete new targets");
	if(passed)
	{
		std::cout << "Windows atomic replacement interruption: " << trialCount
			<< " terminated writers, " << totalObservations << " complete target observations ("
			<< oldObservations << " old, " << newObservations << " new)\n";
	}
	return passed;
#endif
}

bool testLoadAndSaveFaultMatrix()
{
	bool passed = true;
	auto active = load(fixture("minimal_scene.gltf"));
	if(!active) return false;
	const DocumentSnapshot activeBefore = snapshot(*active);
	for(const EditorFaultPoint point : {EditorFaultPoint::parse, EditorFaultPoint::postParseValidation,
	                                  EditorFaultPoint::allocation})
	{
		FailAt injector(point);
		GltfLoadOptions options;
		options.faultInjector = &injector;
		auto result = GltfDocumentIO::loadCandidate(fixture("feature_preservation.gltf"), options);
		passed &= expect(!result.succeeded() && result.error() == GltfLoadError::injectedFailure,
		                 std::string("load fault is atomic at ") + editorFaultPointName(point));
		passed &= expect(!std::move(result).publishTo(active) && unchanged(*active, activeBefore),
		                 "failed load candidate cannot publish over the active document");
	}

	for(const EditorFaultPoint point : {
		EditorFaultPoint::allocation,
		EditorFaultPoint::stagingFileCreate,
		EditorFaultPoint::write,
		EditorFaultPoint::flush,
		EditorFaultPoint::candidateUpload,
		EditorFaultPoint::preCandidatePublish,
		EditorFaultPoint::preAtomicReplace,
	})
	{
		TempDirectory temporary;
		auto document = load(fixture("feature_preservation.gltf"));
		if(!document) return false;
		const DocumentSnapshot before = snapshot(*document);
		const auto target = temporary.path() / "existing.gltf";
		const std::string sentinel = "ORIGINAL-TARGET-" + std::string(editorFaultPointName(point));
		writeText(target, sentinel);
		const auto targetBefore = readBytes(target);
		FailAt injector(point);
		GltfSaveOptions options;
		options.faultInjector = &injector;
		const auto result = GltfDocumentIO::saveAs(*document, target, options);
		passed &= expect(!result.succeeded() && result.error() == GltfSaveError::injectedFailure,
		                 std::string("Save As fault is reported at ") + editorFaultPointName(point));
		if(point == EditorFaultPoint::preAtomicReplace)
		{
			passed &= expect(injector.hitCount() == 1
				&& result.message().find("boundary") != std::string::npos,
				"preAtomicReplace is injected exactly once at the actual publication boundary");
		}
		passed &= expect(readBytes(target) == targetBefore,
		                 "injected Save As failure preserves every byte of the old target");
		passed &= expect(unchanged(*document, before),
		                 "injected Save As failure preserves path, authority, revisions, IDs, scene, and dependencies");
		passed &= expect(noSaveDebris(temporary.path()), "injected Save As failure cleans staging and created resources");
	}
	return passed;
}

bool testMissingDependencyAndTargetFailures()
{
	bool passed = true;
	TempDirectory temporary;
	const auto source = temporary.path() / "source.gltf";
	const auto dependency = temporary.path() / "external_buffer.bin";
	copyFile(fixture("external_buffer.gltf"), source);
	copyFile(fixture("external_buffer.bin"), dependency);
	auto document = load(source);
	if(!document) return false;
	const DocumentSnapshot before = snapshot(*document);
	std::filesystem::remove(dependency);
	const auto target = temporary.path() / "existing.gltf";
	writeText(target, "OLD");
	const auto missing = GltfDocumentIO::saveAs(*document, target);
	passed &= expect(!missing.succeeded() && missing.error() == GltfSaveError::dependencyUnavailable,
	                 "missing external dependency blocks Save As before staging");
	passed &= expect(readBytes(target) == std::vector<unsigned char>({'O', 'L', 'D'}) && unchanged(*document, before),
	                 "missing dependency preserves the old target and document state");

	const auto unsupported = GltfDocumentIO::saveAs(*document, temporary.path() / "target.txt");
	passed &= expect(!unsupported.succeeded() && unsupported.error() == GltfSaveError::dependencyUnavailable,
	                 "dependency validation remains fail-first for a document whose dependency disappeared");

	auto minimal = load(fixture("minimal_scene.gltf"));
	if(!minimal) return false;
	const DocumentSnapshot minimalBefore = snapshot(*minimal);
	const auto badFormat = GltfDocumentIO::saveAs(*minimal, temporary.path() / "target.txt");
	passed &= expect(!badFormat.succeeded() && badFormat.error() == GltfSaveError::unsupportedFormat
		&& unchanged(*minimal, minimalBefore), "unsupported Save As suffix is rejected atomically");
	const auto missingDirectory = GltfDocumentIO::saveAs(*minimal, temporary.path() / "missing" / "target.gltf");
	passed &= expect(!missingDirectory.succeeded() && missingDirectory.error() == GltfSaveError::unsafeTarget
		&& unchanged(*minimal, minimalBefore), "missing target directory is rejected atomically");

	const auto directoryTarget = temporary.path() / "directory.gltf";
	std::filesystem::create_directory(directoryTarget);
	const auto nonRegular = GltfDocumentIO::saveAs(*minimal, directoryTarget);
	passed &= expect(!nonRegular.succeeded() && nonRegular.error() == GltfSaveError::publishFailed
		&& std::filesystem::is_directory(directoryTarget) && unchanged(*minimal, minimalBefore),
		"non-regular target fails without moving or replacing the directory");

	const auto readOnlyTarget = temporary.path() / "read-only.gltf";
	writeText(readOnlyTarget, "READ-ONLY-OLD-TARGET");
	std::error_code permissionError;
	std::filesystem::permissions(readOnlyTarget,
		std::filesystem::perms::owner_write | std::filesystem::perms::group_write
			| std::filesystem::perms::others_write,
		std::filesystem::perm_options::remove,
		permissionError);
	if(!permissionError)
	{
		const auto bytesBefore = readBytes(readOnlyTarget);
		const auto readOnly = GltfDocumentIO::saveAs(*minimal, readOnlyTarget);
		passed &= expect(!readOnly.succeeded() && readOnly.error() == GltfSaveError::publishFailed
			&& readBytes(readOnlyTarget) == bytesBefore && unchanged(*minimal, minimalBefore),
			"read-only existing target is not overwritten or marked clean");
		std::filesystem::permissions(readOnlyTarget,
			std::filesystem::perms::owner_write,
			std::filesystem::perm_options::add,
			permissionError);
	}
	return passed;
}

bool testUnicodeLongCrossVolumeAndRepeatedSave()
{
	bool passed = true;
	TempDirectory temporary;
	const auto featureSource = fixture("feature_preservation.gltf");
	auto featureReference = load(featureSource);
	if(!featureReference) return false;
	const auto sourceBytes = readBytes(featureSource);

	const auto shortDirectory = temporary.path() / std::filesystem::path(u8"short-Unicode-路径");
	std::error_code directoryError;
	std::filesystem::create_directories(shortDirectory, directoryError);
	auto shortDocument = load(featureSource);
	if(directoryError || !shortDocument) return false;
	const auto shortTarget = shortDirectory / std::filesystem::path(u8"输出-scene.gltf");
	const auto shortSave = GltfDocumentIO::saveAs(*shortDocument, shortTarget);
	passed &= expect(shortSave.succeeded(), "short Unicode Save As succeeds");
	if(!shortSave.succeeded()) std::cerr << shortSave.message() << '\n';
	auto shortReloaded = shortSave.succeeded() ? load(shortTarget) : nullptr;
	passed &= expect(shortReloaded
		&& GltfSemanticCompare::compare(*featureReference, *shortReloaded).equivalent(),
		"short Unicode target reloads with an empty semantic diff");

	const auto asciiTarget = moderatelyLongTarget(
		temporary.path(), false, "ascii-external-buffer.gltf", 232u);
	auto externalBufferReference = load(fixture("external_buffer.gltf"));
	auto externalBufferDocument = load(fixture("external_buffer.gltf"));
	if(asciiTarget.empty() || !externalBufferReference || !externalBufferDocument) return false;
	const auto asciiSave = GltfDocumentIO::saveAs(*externalBufferDocument, asciiTarget);
	passed &= expect(asciiSave.succeeded(), "moderately long ASCII glTF Save As succeeds");
	if(!asciiSave.succeeded()) std::cerr << asciiSave.message() << '\n';
	auto asciiReloaded = asciiSave.succeeded() ? load(asciiTarget) : nullptr;
	passed &= expect(asciiReloaded
		&& GltfSemanticCompare::compare(*externalBufferReference, *asciiReloaded).equivalent(),
		"moderately long ASCII glTF reloads with an empty semantic diff");
	passed &= expect(asciiSave.succeeded()
		&& hasPublishedDependency(*externalBufferDocument, GltfDependencyKind::buffer),
		"moderately long ASCII glTF publishes its external buffer beside the target");

	const auto target = moderatelyLongTarget(
		temporary.path(), true, std::filesystem::path(u8"输出-scene.gltf"), 232u);
	auto document = load(featureSource);
	if(target.empty() || !document) return false;
	const auto first = GltfDocumentIO::saveAs(*document, target);
	passed &= expect(first.succeeded(), "Unicode and moderately long cross-volume Save As succeeds");
	if(!first.succeeded()) std::cerr << first.message() << '\n';
	passed &= expect(readBytes(fixture("feature_preservation.gltf")) == sourceBytes,
	                 "Save As leaves the existing source bytes untouched");
	if(!first.succeeded()) return false;
	auto reloaded = load(target);
	passed &= expect(reloaded && GltfSemanticCompare::compare(*featureReference, *reloaded).equivalent(),
	                 "Unicode/long-path target reloads with an empty semantic diff");
	passed &= expect(hasPublishedDependency(*document, GltfDependencyKind::buffer),
		"Unicode/long-path glTF publishes its external buffer beside the target");
#ifdef _WIN32
	passed &= expect(std::any_of(
		document->dependencies().begin(),
		document->dependencies().end(),
		[](const GltfDependency& dependency) {
			return dependency.kind == GltfDependencyKind::buffer
				&& dependency.resolvedPath.native().size() >= 260u;
		}), "the long Unicode glTF exercises an extended-length external resource path");
#endif

	const std::uint64_t revision = document->revision();
	const auto second = GltfDocumentIO::saveAs(*document, target);
	passed &= expect(second.succeeded() && document->revision() == revision
		&& document->cleanRevision() == revision,
		"repeated Save As safely reuses identical content-addressed resources without revision churn");
#ifdef _WIN32
	passed &= expect(!second.usedRecoverableReplaceFallback() && second.recoveryPath().empty(),
	                 "Windows existing-target Save As uses one atomic replacement without backup debris");
#endif
	passed &= expect(noSaveDebris(target.parent_path(), false),
		"successful repeated Save As leaves no staging or backup debris");

	const DocumentSnapshot beforeInjectedFailure = snapshot(*document);
	const std::vector<unsigned char> targetBeforeInjectedFailure = readBytes(target);
	FailAt injector(EditorFaultPoint::preAtomicReplace);
	GltfSaveOptions injectedOptions;
	injectedOptions.faultInjector = &injector;
	const auto injected = GltfDocumentIO::saveAs(*document, target, injectedOptions);
	passed &= expect(!injected.succeeded() && injected.error() == GltfSaveError::injectedFailure,
		"long Unicode existing-target failure is reported at the atomic replacement boundary");
	passed &= expect(readBytes(target) == targetBeforeInjectedFailure
		&& unchanged(*document, beforeInjectedFailure),
		"long Unicode replacement failure preserves target bytes and document revisions");
	passed &= expect(noSaveDebris(target.parent_path(), false),
		"long Unicode replacement failure cleans staging debris");

	const auto imageTarget = moderatelyLongTarget(
		temporary.path(), true, std::filesystem::path(u8"图像-scene.gltf"), 232u);
	auto externalImageReference = load(fixture("external_image.gltf"));
	auto externalImageDocument = load(fixture("external_image.gltf"));
	if(imageTarget.empty() || !externalImageReference || !externalImageDocument) return false;
	const auto imageSave = GltfDocumentIO::saveAs(*externalImageDocument, imageTarget);
	passed &= expect(imageSave.succeeded(), "moderately long Unicode external-image glTF Save As succeeds");
	if(!imageSave.succeeded()) std::cerr << imageSave.message() << '\n';
	auto imageReloaded = imageSave.succeeded() ? load(imageTarget) : nullptr;
	passed &= expect(imageReloaded
		&& GltfSemanticCompare::compare(*externalImageReference, *imageReloaded).equivalent(),
		"moderately long Unicode external-image glTF reloads with an empty semantic diff");
	passed &= expect(imageSave.succeeded()
		&& hasPublishedDependency(*externalImageDocument, GltfDependencyKind::image),
		"moderately long Unicode glTF publishes its external image beside the target");
	passed &= expect(noSaveDebris(imageTarget.parent_path(), false),
		"external-image Save As leaves no staging or backup debris");

	const auto glbTarget = moderatelyLongTarget(
		temporary.path(),
		false,
		"moderately-long-binary-main-file-abcdefghijklmnopqrstuvwxyz.glb",
		270u);
	auto glbDocument = load(featureSource);
	if(glbTarget.empty() || !glbDocument) return false;
	const auto glbSave = GltfDocumentIO::saveAs(*glbDocument, glbTarget);
	passed &= expect(glbSave.succeeded(), "moderately long ASCII GLB Save As succeeds");
	if(!glbSave.succeeded()) std::cerr << glbSave.message() << '\n';
	auto glbReloaded = glbSave.succeeded() ? load(glbTarget) : nullptr;
	passed &= expect(glbReloaded
		&& glbReloaded->format() == GltfDocumentFormat::binary
		&& glbReloaded->dependencies().empty()
		&& GltfSemanticCompare::compare(*featureReference, *glbReloaded).equivalent(),
		"moderately long GLB reloads as a self-contained binary document");
#ifdef _WIN32
	passed &= expect(glbDocument->sourcePath().native().size() >= 260u,
		"the GLB case exercises an extended-length main document path");
#endif
	passed &= expect(noSaveDebris(glbTarget.parent_path(), false),
		"moderately long GLB Save As leaves no staging or backup debris");

	const auto imageGlbTarget = moderatelyLongTarget(
		temporary.path(),
		true,
		std::filesystem::path(u8"图像-moderately-long-binary-main-file-abcdefghijklmnopqrstuvwxyz.glb"),
		270u);
	auto imageGlbDocument = load(fixture("external_image.gltf"));
	if(imageGlbTarget.empty() || !imageGlbDocument) return false;
	const auto imageGlbSave = GltfDocumentIO::saveAs(*imageGlbDocument, imageGlbTarget);
	passed &= expect(imageGlbSave.succeeded(),
		"moderately long Unicode external-image GLB Save As succeeds");
	if(!imageGlbSave.succeeded())
	{
		std::cerr << imageGlbSave.message() << '\n';
		for(const GltfValidationIssue& issue : imageGlbSave.validationReport().issues())
		{
			std::cerr << issue.path << ": " << issue.message << '\n';
		}
	}
	auto imageGlbReloaded = imageGlbSave.succeeded() ? load(imageGlbTarget) : nullptr;
	passed &= expect(imageGlbReloaded
		&& imageGlbReloaded->format() == GltfDocumentFormat::binary
		&& imageGlbReloaded->dependencies().empty()
		&& GltfSemanticCompare::compare(*externalImageReference, *imageGlbReloaded).equivalent(),
		"moderately long Unicode GLB re-encodes its image and reloads self-contained");
	passed &= expect(noSaveDebris(imageGlbTarget.parent_path(), false),
		"external-image GLB Save As leaves no staging or backup debris");

	const bool crossVolume = !fixture("feature_preservation.gltf").root_name().empty()
		&& fixture("feature_preservation.gltf").root_name() != target.root_name();
	passed &= expect(!crossVolume || first.succeeded(),
	                 "same-directory staging supports a destination on a different source volume");
	return passed;
}

} // namespace

int runTests()
{
	bool passed = true;
	passed &= testLoadAndSaveFaultMatrix();
	passed &= testMissingDependencyAndTargetFailures();
	passed &= testUnicodeLongCrossVolumeAndRepeatedSave();
	passed &= testAtomicReplacementInterruption();
	return passed ? 0 : 1;
}

#ifdef _WIN32

int wmain(int argc, wchar_t* argv[])
{
	if(argc == 6 && std::wstring_view(argv[1]) == L"--atomic-replace-child")
	{
		return runAtomicReplaceChild(argv[2], argv[3], argv[4], argv[5]);
	}
	return runTests();
}

#else

int main()
{
	return runTests();
}

#endif
