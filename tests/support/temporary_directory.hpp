#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace astra::test {

struct CleanupIssue {
    std::filesystem::path path;
    std::string message;
};

struct CleanupStats {
    std::size_t transient_entries{};
    std::size_t retry_attempts{};
};

#ifdef _WIN32
// Failure diagnostics only: inspect this test process's own handle table, never
// the system handle table or other processes. No file content is read.
inline std::string own_directory_handles(const std::filesystem::path& target) {
    using Query = LONG(NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto symbol = module ? GetProcAddress(module, "NtQueryInformationProcess") : nullptr;
    if (!symbol) {
        return "self handle diagnostics unavailable";
    }
    const auto query = std::bit_cast<Query>(symbol);

    struct Header {
        ULONG_PTR count;
        ULONG_PTR reserved;
    };

    struct Entry {
        HANDLE value;
        ULONG_PTR handle_count, pointer_count;
        ULONG access, object_type, attributes, reserved;
    };

    std::vector<std::byte> storage(64 * 1024);
    for (;;) {
        ULONG needed = 0;
        const auto status = query(GetCurrentProcess(),
                                  51,
                                  storage.data(),
                                  static_cast<ULONG>(storage.size()),
                                  &needed); // ProcessHandleInformation
        if (status >= 0) {
            break;
        }
        if (status != static_cast<LONG>(0xc0000004UL) || storage.size() >= 16 * 1024 * 1024) {
            return "self handle query status " + std::to_string(status);
        }
        storage.resize(std::max(storage.size() * 2, static_cast<std::size_t>(needed)));
    }
    const auto* header = reinterpret_cast<const Header*>(storage.data());
    const auto* entries = reinterpret_cast<const Entry*>(storage.data() + sizeof(Header));
    if (header->count > (storage.size() - sizeof(Header)) / sizeof(Entry)) {
        return "invalid self handle snapshot";
    }
    std::string result;
    for (ULONG_PTR index = 0; index < header->count; ++index) {
        HANDLE copy = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(),
                             entries[index].value,
                             GetCurrentProcess(),
                             &copy,
                             0,
                             FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            continue;
        }

        struct Close {
            HANDLE value;

            ~Close() {
                (void)CloseHandle(value);
            }
        } close{copy};

        if (GetFileType(copy) != FILE_TYPE_DISK) {
            continue;
        }
        std::wstring name(32768, L'\0');
        const auto length =
            GetFinalPathNameByHandleW(copy, name.data(), static_cast<DWORD>(name.size()), 0);
        if (length == 0 || length >= name.size()) {
            continue;
        }
        name.resize(length);
        if (name.starts_with(L"\\\\?\\")) {
            name.erase(0, 4);
        }
        if (std::filesystem::path(name) != target) {
            continue;
        }
        result += " self PID=" + std::to_string(GetCurrentProcessId()) + " handle=" +
                  std::to_string(reinterpret_cast<std::uintptr_t>(entries[index].value)) +
                  " access=" + std::to_string(entries[index].access);
    }
    return result.empty() ? "no matching directory handle in this test process" : result;
}
#endif

// Only remove one explicitly owned, uniquely named direct child of a canonical
// test-output parent. All iterators close before deletion; immutable Git objects
// lose ReadOnly only within this verified fixture. Process lifetime tests leave
// the retry budget at zero. Other fixtures may explicitly allow at most one
// second for sharing/lock violations, with retry counts retained and reported.
inline std::vector<CleanupIssue>
cleanup_owned_directory(const std::filesystem::path& directory,
                        const std::filesystem::path& allowed_parent,
                        std::string_view prefix,
                        std::chrono::milliseconds transient_timeout = {},
                        CleanupStats* statistics = nullptr) {
    namespace fs = std::filesystem;
    std::vector<CleanupIssue> issues;
    CleanupStats retries;
    if (transient_timeout < std::chrono::milliseconds::zero() ||
        transient_timeout > std::chrono::seconds(1)) {
        return {{directory, "cleanup retry budget must be between zero and one second"}};
    }
    const auto deadline = std::chrono::steady_clock::now() + transient_timeout;
    std::error_code error;
    const auto parent = fs::weakly_canonical(allowed_parent, error);
    if (error || parent != allowed_parent || prefix.empty()) {
        return {{directory, "cleanup parent or prefix is invalid"}};
    }
    const auto resolved = fs::weakly_canonical(directory, error);
    if (error || resolved != directory || resolved.parent_path() != parent ||
        !resolved.filename().string().starts_with(prefix)) {
        return {{directory, "refusing unsafe or redirected fixture cleanup"}};
    }
    if (!fs::exists(resolved, error) && !error) {
        return issues;
    }
    std::vector<fs::path> entries;
    {
        fs::recursive_directory_iterator cursor(resolved, error), end;
        while (!error && cursor != end) {
            entries.push_back(cursor->path());
            cursor.increment(error);
        }
    }
    if (error) {
        return {{resolved, "enumerating fixture: " + error.message()}};
    }
    std::ranges::sort(entries, [](const fs::path& a, const fs::path& b) {
        return a.native().size() > b.native().size();
    });
    entries.push_back(resolved);
    for (const auto& path : entries) {
        const auto checked = fs::weakly_canonical(path, error);
        const auto relative = checked.lexically_relative(resolved);
        const bool contained = !relative.empty() && !relative.is_absolute() &&
                               std::ranges::none_of(relative, [](const fs::path& part) {
                                   return part == "..";
                               });
        if (error || checked != path || !contained) {
            issues.push_back({path, "refusing redirected fixture entry cleanup"});
            return issues;
        }
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            ((attributes & FILE_ATTRIBUTE_READONLY) != 0 &&
             !SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY))) {
            issues.push_back(
                {path, "prepare entry: Windows error " + std::to_string(GetLastError())});
            continue;
        }
        // Use the documented Win32 directory/file deletion operations. MSVC's
        // filesystem::remove uses FileDispositionInfoEx POSIX semantics, whose
        // parent-directory lifetime differs from ordinary Win32 deletion.
        const auto remove_entry = [&] {
            const auto removed = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                                     ? RemoveDirectoryW(path.c_str())
                                     : DeleteFileW(path.c_str());
            error = removed
                        ? std::error_code{}
                        : std::error_code(static_cast<int>(GetLastError()), std::system_category());
        };
        remove_entry();
        bool transient = false;
        while (
            (error.value() == ERROR_SHARING_VIOLATION || error.value() == ERROR_LOCK_VIOLATION) &&
            std::chrono::steady_clock::now() < deadline) {
            transient = true;
            ++retries.retry_attempts;
            Sleep(10);
            remove_entry();
        }
        if (transient) {
            ++retries.transient_entries;
        }
#else
        (void)fs::remove(path, error);
#endif
        if (error) {
            auto message = error.message();
#ifdef _WIN32
            if (error.value() == ERROR_SHARING_VIOLATION) {
                message += "; " + own_directory_handles(path);
            }
            // Opt-in diagnosis only. The original failure is retained and no
            // deletion is retried; normal test runs never sleep here.
            wchar_t pause[2]{};
            if (GetEnvironmentVariableW(L"ASTRA_TEST_PAUSE_CLEANUP", pause, 2) == 1 &&
                pause[0] == L'1') {
                const auto utf8 = path.u8string();
                std::fprintf(stderr,
                             "Astra cleanup diagnostic pause PID=%lu path=%s\n",
                             GetCurrentProcessId(),
                             reinterpret_cast<const char*>(utf8.c_str()));
                std::fflush(stderr);
                Sleep(15000);
            }
#endif
            issues.push_back({path, std::move(message)});
        }
    }
    if (statistics) {
        *statistics = retries;
    }
    if (retries.retry_attempts != 0) {
        const auto utf8 = directory.u8string();
        std::fprintf(stderr,
                     "Astra fixture cleanup transient_entries=%zu retry_attempts=%zu path=%s\n",
                     retries.transient_entries,
                     retries.retry_attempts,
                     reinterpret_cast<const char*>(utf8.c_str()));
    }
    return issues;
}

} // namespace astra::test
