#include <astra/recovery/store.hpp>
#include <astra/core/utf8.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <limits>
#include <map>
#include <random>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace astra::recovery {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

void validate_limits(const RecoveryLimits& limits) {
    if (limits.max_text_bytes == 0 || limits.max_disk_bytes == 0 || limits.max_record_bytes == 0 ||
        limits.max_records == 0 || limits.max_sessions == 0) {
        throw std::invalid_argument("recovery limits must be positive");
    }
}

bool valid_session(std::string_view id) {
    return id.size() == 32 && std::ranges::all_of(id, [](char value) {
               return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
           });
}

std::string unique_session() {
    constexpr char digits[] = "0123456789abcdef";
    std::random_device random;
    std::string result;
    for (int index = 0; index < 32; ++index) {
        result += digits[random() & 15U];
    }
    return result;
}

std::string path_text(const fs::path& path) {
    const auto text = path.generic_u8string();
    std::string result(reinterpret_cast<const char*>(text.data()), text.size());
    utf8::validate(result);
    if (result.empty() || result.find('\0') != result.npos || !path.is_absolute()) {
        throw std::invalid_argument("recovery source paths must be absolute and contain no NUL");
    }
    return result;
}

fs::path text_path(const Json& value) {
    const auto text = value.get<std::string>();
    utf8::validate(text);
    const fs::path result(
        std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
    (void)path_text(result);
    return result;
}

void require_child(const fs::path& parent, const fs::path& child) {
    if (fs::canonical(parent) != parent || child.parent_path() != parent ||
        fs::weakly_canonical(child) != child) {
        throw RecoveryCorrupt("recovery path was redirected outside its owned directory");
    }
}

class SessionLock {
  public:
    explicit SessionLock(const fs::path& directory) : path_(directory / ".lock") {
        const auto& path = path_;
        require_child(directory, path);
#ifdef _WIN32
        auto native = path.native();
        if (!native.starts_with(L"\\\\?\\")) {
            native = native.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + native.substr(2)
                                                 : L"\\\\?\\" + native;
        }
        handle_ = CreateFileW(native.c_str(),
                              GENERIC_READ | GENERIC_WRITE | DELETE,
                              0,
                              nullptr,
                              OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                              nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
                throw RecoveryLocked();
            }
            throw fs::filesystem_error(
                "lock recovery session",
                path,
                std::error_code(static_cast<int>(error), std::system_category()));
        }
#else
        handle_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (handle_ < 0) {
            throw fs::filesystem_error(
                "open recovery lock", path, std::error_code(errno, std::generic_category()));
        }
        if (::flock(handle_, LOCK_EX | LOCK_NB) != 0) {
            const auto error = errno;
            (void)::close(handle_);
            handle_ = -1;
            if (error == EWOULDBLOCK) {
                throw RecoveryLocked();
            }
            throw fs::filesystem_error(
                "lock recovery session", path, std::error_code(error, std::generic_category()));
        }
#endif
    }

    ~SessionLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE && !CloseHandle(handle_)) {
            std::fputs("recovery lock close failed\n", stderr);
        }
#else
        if (handle_ >= 0 && ::close(handle_) != 0) {
            std::fputs("recovery lock close failed\n", stderr);
        }
#endif
    }

    SessionLock(const SessionLock&) = delete;
    SessionLock& operator=(const SessionLock&) = delete;

    void remove_marker() {
#ifdef _WIN32
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(
                handle_, FileDispositionInfo, &disposition, sizeof(disposition))) {
            throw fs::filesystem_error(
                "remove empty recovery lock",
                path_,
                std::error_code(static_cast<int>(GetLastError()), std::system_category()));
        }
        const auto handle = std::exchange(handle_, INVALID_HANDLE_VALUE);
        if (!CloseHandle(handle)) {
            throw fs::filesystem_error(
                "close empty recovery lock",
                path_,
                std::error_code(static_cast<int>(GetLastError()), std::system_category()));
        }
#else
        if (::unlink(path_.c_str()) != 0) {
            throw fs::filesystem_error("remove empty recovery lock",
                                       path_,
                                       std::error_code(errno, std::generic_category()));
        }
        const auto handle = std::exchange(handle_, -1);
        if (::close(handle) != 0) {
            throw fs::filesystem_error("close empty recovery lock",
                                       path_,
                                       std::error_code(errno, std::generic_category()));
        }
#endif
    }

  private:
    fs::path path_;
#ifdef _WIN32
    HANDLE handle_{INVALID_HANDLE_VALUE};
#else
    int handle_{-1};
#endif
};

std::uint32_t checksum(std::string_view bytes) {
    constexpr auto table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t index = 0; index < 256; ++index) {
            auto value = index;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value >> 1U) ^ ((value & 1U) ? 0xedb88320U : 0U);
            }
            result[index] = value;
        }
        return result;
    }();
    std::uint32_t result = 0xffffffffU;
    for (const auto byte : bytes) {
        result = table[(result ^ static_cast<unsigned char>(byte)) & 255U] ^ (result >> 8U);
    }
    return result ^ 0xffffffffU;
}

std::string base64(std::string_view bytes) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const auto count = std::min<std::size_t>(3, bytes.size() - index);
        std::uint32_t value = static_cast<unsigned char>(bytes[index]) << 16U;
        if (count > 1) {
            value |= static_cast<unsigned char>(bytes[index + 1]) << 8U;
        }
        if (count > 2) {
            value |= static_cast<unsigned char>(bytes[index + 2]);
        }
        result += alphabet[(value >> 18U) & 63U];
        result += alphabet[(value >> 12U) & 63U];
        result += count > 1 ? alphabet[(value >> 6U) & 63U] : '=';
        result += count > 2 ? alphabet[value & 63U] : '=';
    }
    return result;
}

std::string unbase64(std::string_view text, std::size_t limit) {
    if (text.size() % 4 != 0 || text.size() / 4 > limit / 3 + 1) {
        throw RecoveryCorrupt("invalid or excessive disk version encoding");
    }
    const auto digit = [](char value) -> std::uint32_t {
        if (value >= 'A' && value <= 'Z') {
            return static_cast<std::uint32_t>(value - 'A');
        }
        if (value >= 'a' && value <= 'z') {
            return static_cast<std::uint32_t>(value - 'a' + 26);
        }
        if (value >= '0' && value <= '9') {
            return static_cast<std::uint32_t>(value - '0' + 52);
        }
        if (value == '+') {
            return 62;
        }
        if (value == '/') {
            return 63;
        }
        throw RecoveryCorrupt("invalid base64 disk version");
    };
    std::string result;
    for (std::size_t index = 0; index < text.size(); index += 4) {
        const bool third = text[index + 2] != '=';
        const bool fourth = text[index + 3] != '=';
        if ((!third && fourth) || ((!third || !fourth) && index + 4 != text.size())) {
            throw RecoveryCorrupt("invalid base64 padding");
        }
        const auto a = digit(text[index]), b = digit(text[index + 1]);
        const auto c = third ? digit(text[index + 2]) : 0U;
        const auto d = fourth ? digit(text[index + 3]) : 0U;
        if ((!third && (b & 15U)) || (!fourth && (c & 3U))) {
            throw RecoveryCorrupt("noncanonical base64 padding");
        }
        const auto value = (a << 18U) | (b << 12U) | (c << 6U) | d;
        result += static_cast<char>((value >> 16U) & 255U);
        if (third) {
            result += static_cast<char>((value >> 8U) & 255U);
        }
        if (fourth) {
            result += static_cast<char>(value & 255U);
        }
    }
    if (result.size() > limit) {
        throw std::length_error("recovery disk version exceeds limit");
    }
    return result;
}

std::uint64_t integer(const Json& value) {
    if (!value.is_number_unsigned()) {
        throw RecoveryCorrupt("recovery integer must be unsigned");
    }
    return value.get<std::uint64_t>();
}

std::size_t offset(const Json& value) {
    const auto result = integer(value);
    if (result > std::numeric_limits<std::size_t>::max()) {
        throw RecoveryCorrupt("recovery offset is too large");
    }
    return static_cast<std::size_t>(result);
}

void validate_record(const RecoveryRecord& record, const RecoveryLimits& limits) {
    const auto& document = record.document;
    if (document.text.size_bytes() > limits.max_text_bytes) {
        throw std::length_error("recovery text exceeds limit");
    }
    if (document.selections.empty() || document.selections.size() > 4096) {
        throw std::invalid_argument("invalid recovery selection count");
    }
    std::optional<Selection> previous;
    for (const auto selection : document.selections) {
        if (selection.end().value > document.text.size_bytes() ||
            !document.text.is_boundary(selection.anchor.value) ||
            !document.text.is_boundary(selection.head.value)) {
            throw std::invalid_argument("recovery selection is not a UTF-8 boundary");
        }
        if (previous &&
            (selection.begin() < previous->end() ||
             (selection.begin() == previous->end() && (selection.empty() || previous->empty())))) {
            throw std::invalid_argument("recovery selections must be normalized");
        }
        previous = selection;
    }
    switch (record.encoding) {
    case io::TextEncoding::utf8:
    case io::TextEncoding::utf8_bom:
    case io::TextEncoding::utf16_le:
    case io::TextEncoding::utf16_be:
    case io::TextEncoding::latin1:
        break;
    default:
        throw std::invalid_argument("unknown recovery text encoding");
    }
    if (record.path) {
        (void)path_text(*record.path);
    }
    if (record.disk_version) {
        if (!record.path || !record.disk_version->raw_content ||
            record.path->lexically_normal() != record.disk_version->path.lexically_normal()) {
            throw std::invalid_argument("recovery disk version belongs to a different path");
        }
        if (record.disk_version->raw_content->size() > limits.max_disk_bytes) {
            throw std::length_error("recovery disk bytes exceed limit");
        }
    }
}

Json payload(const RecoveryRecord& record, const RecoveryLimits& limits) {
    validate_record(record, limits);
    Json selections = Json::array();
    for (const auto selection : record.document.selections) {
        selections.push_back({selection.anchor.value, selection.head.value});
    }
    Json result{{"id", record.document.id.value},
                {"revision", record.document.revision.value},
                {"identity", record.document.content_identity.value},
                {"text", record.document.text.str()},
                {"selections", std::move(selections)},
                {"encoding", static_cast<unsigned int>(record.encoding)},
                {"sequence", record.checkpoint_sequence},
                {"path", record.path ? Json(path_text(*record.path)) : Json(nullptr)},
                {"disk", nullptr},
                {"deleted", false}};
    if (record.disk_version) {
        result["disk"] = {{"path", path_text(record.disk_version->path)},
                          {"bytes", base64(*record.disk_version->raw_content)}};
    }
    return result;
}

std::string envelope(const Json& data, const RecoveryLimits& limits) {
    const auto body = data.dump();
    const auto result = Json{
        {"format", "astra-recovery"},
        {"schema", 1U},
        {"crc32", checksum(body)},
        {"payload", data}}.dump();
    if (result.size() > limits.max_record_bytes) {
        throw std::length_error("encoded recovery checkpoint exceeds limit");
    }
    return result;
}

struct Stored {
    BufferId id;
    Revision revision;
    std::uint64_t sequence{};
    std::optional<RecoveryRecord> record;
    io::FileVersion version;
};

fs::path record_path(const fs::path& directory, BufferId id) {
    const auto path = directory / ("buffer-" + std::to_string(id.value) + ".json");
    require_child(directory, path);
    return path;
}

std::optional<Stored>
read_stored(const fs::path& directory, BufferId id, const RecoveryLimits& limits) {
    const auto path = record_path(directory, id);
    if (!fs::exists(path)) {
        return std::nullopt;
    }
    try {
        const auto file = io::read_file(path, limits.max_record_bytes, io::TextEncoding::utf8);
        const auto root = Json::parse(file.text, [](int depth, Json::parse_event_t, Json&) {
            if (depth > 24) {
                throw RecoveryCorrupt("recovery JSON nesting exceeds limit");
            }
            return true;
        });
        if (root.at("format") != "astra-recovery" || integer(root.at("schema")) != 1) {
            throw RecoveryCorrupt("unsupported recovery format/schema");
        }
        const auto& data = root.at("payload");
        if (integer(root.at("crc32")) != checksum(data.dump())) {
            throw RecoveryCorrupt("recovery checksum mismatch");
        }
        Stored stored{{integer(data.at("id"))},
                      {integer(data.at("revision"))},
                      data.contains("sequence") ? integer(data.at("sequence")) : 0,
                      std::nullopt,
                      file.version};
        if (stored.id != id) {
            throw RecoveryCorrupt("recovery filename and document ID differ");
        }
        if (data.at("deleted").get<bool>()) {
            return stored;
        }
        const auto text = data.at("text").get<std::string>();
        if (text.size() > limits.max_text_bytes) {
            throw std::length_error("recovery text exceeds limit");
        }
        RecoveryRecord record;
        record.document = {id, stored.revision, Rope(text), {}, {integer(data.at("identity"))}};
        record.checkpoint_sequence = stored.sequence;
        const auto& selections = data.at("selections");
        if (!selections.is_array() || selections.size() > 4096) {
            throw RecoveryCorrupt("invalid recovery selections");
        }
        for (const auto& selection : selections) {
            if (!selection.is_array() || selection.size() != 2) {
                throw RecoveryCorrupt("invalid recovery selection pair");
            }
            record.document.selections.push_back({{offset(selection[0])}, {offset(selection[1])}});
        }
        const auto encoding = integer(data.at("encoding"));
        if (encoding > static_cast<unsigned int>(io::TextEncoding::latin1)) {
            throw RecoveryCorrupt("unknown recovery encoding");
        }
        record.encoding = static_cast<io::TextEncoding>(encoding);
        if (!data.at("path").is_null()) {
            record.path = text_path(data.at("path"));
        }
        if (!data.at("disk").is_null()) {
            const auto& disk = data.at("disk");
            record.disk_version =
                io::FileVersion{text_path(disk.at("path")),
                                std::make_shared<const std::string>(unbase64(
                                    disk.at("bytes").get<std::string>(), limits.max_disk_bytes))};
        }
        validate_record(record, limits);
        stored.record = std::move(record);
        return stored;
    } catch (const RecoveryCorrupt&) {
        throw;
    } catch (const std::exception& error) {
        throw RecoveryCorrupt(std::string("invalid recovery checkpoint: ") + error.what());
    }
}

std::optional<BufferId> filename_id(std::string_view name) {
    if (!name.starts_with("buffer-") || !name.ends_with(".json")) {
        return std::nullopt;
    }
    name.remove_prefix(7);
    name.remove_suffix(5);
    std::uint64_t id = 0;
    const auto result = std::from_chars(name.data(), name.data() + name.size(), id);
    if (name.empty() || (name.size() > 1 && name.front() == '0') || result.ec != std::errc{} ||
        result.ptr != name.data() + name.size()) {
        return std::nullopt;
    }
    return BufferId{id};
}

SessionInfo inspect_session(const fs::path& directory,
                            std::string id,
                            bool active,
                            const RecoveryLimits& limits) {
    SessionInfo result{std::move(id), active, {}, {}};
    std::size_t entries = 0;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entry.path().filename() == ".lock") {
            continue;
        }
        if (++entries > limits.max_records) {
            throw std::length_error("recovery record count exceeds limit");
        }
        try {
            const auto buffer = filename_id(entry.path().filename().string());
            if (!buffer) {
                throw RecoveryCorrupt("unfinished or unrecognized recovery file retained");
            }
            const auto stored = read_stored(directory, *buffer, limits);
            if (stored && stored->record) {
                result.buffers.push_back(*buffer);
            }
        } catch (const std::exception& error) {
            result.issues.push_back({entry.path(), error.what()});
        }
    }
    std::ranges::sort(result.buffers);
    return result;
}

} // namespace

RecoveryLocked::RecoveryLocked() : std::runtime_error("recovery session has a live owner") {
}

RecoveryCorrupt::RecoveryCorrupt(const std::string& message) : std::runtime_error(message) {
}

struct RecoveryStore::Impl {
    fs::path root, directory;
    std::string id;
    RecoveryLimits limits;
    std::unique_ptr<SessionLock> lock;

    struct Watermark {
        Revision revision;
        std::uint64_t sequence;
    };

    std::map<BufferId, Watermark> erased;

    Impl(fs::path state, std::optional<std::string> claim, RecoveryLimits bounds) : limits(bounds) {
        validate_limits(limits);
        if (state.empty()) {
            throw std::invalid_argument("explicit recovery state directory is required");
        }
        (void)fs::create_directories(state);
        root = fs::canonical(state);
        if (claim) {
            if (!valid_session(*claim)) {
                throw std::invalid_argument("invalid recovery session ID");
            }
            id = std::move(*claim);
            directory = root / ("session-" + id);
            require_child(root, directory);
            if (!fs::is_directory(directory)) {
                throw std::invalid_argument("recovery session does not exist");
            }
        } else {
            for (int attempt = 0; attempt < 32; ++attempt) {
                id = unique_session();
                directory = root / ("session-" + id);
                if (fs::create_directory(directory)) {
                    break;
                }
                if (attempt == 31) {
                    throw std::runtime_error("cannot allocate unique recovery session");
                }
            }
        }
        lock = std::make_unique<SessionLock>(directory);
    }

    void check() const {
        require_child(root, directory);
    }
};

RecoveryStore::RecoveryStore(fs::path state,
                             std::optional<std::string> claim,
                             RecoveryLimits limits)
    : impl_(std::make_unique<Impl>(std::move(state), std::move(claim), limits)) {
}

RecoveryStore::~RecoveryStore() = default;
RecoveryStore::RecoveryStore(RecoveryStore&&) noexcept = default;
RecoveryStore& RecoveryStore::operator=(RecoveryStore&&) noexcept = default;

const std::string& RecoveryStore::session_id() const noexcept {
    return impl_->id;
}

const fs::path& RecoveryStore::session_directory() const noexcept {
    return impl_->directory;
}

void RecoveryStore::checkpoint(const RecoveryRecord& record) {
    impl_->check();
    const auto data = payload(record, impl_->limits);
    const auto bytes = envelope(data, impl_->limits);
    const auto id = record.document.id;
    const auto existing = read_stored(impl_->directory, id, impl_->limits);
    const auto erased = impl_->erased.find(id);
    const auto compare = [&](Revision revision, std::uint64_t sequence) {
        if (sequence != 0) {
            if (record.checkpoint_sequence == 0) {
                return -1;
            }
            return record.checkpoint_sequence < sequence   ? -1
                   : record.checkpoint_sequence > sequence ? 1
                                                           : 0;
        }
        if (record.checkpoint_sequence != 0) {
            return 1;
        }
        return record.document.revision < revision   ? -1
               : record.document.revision > revision ? 1
                                                     : 0;
    };
    if ((erased != impl_->erased.end() &&
         compare(erased->second.revision, erased->second.sequence) <= 0) ||
        (existing &&
         (compare(existing->revision, existing->sequence) < 0 ||
          (!existing->record && compare(existing->revision, existing->sequence) == 0)))) {
        throw StaleRevision();
    }
    if (existing && existing->record && record.checkpoint_sequence != 0 &&
        record.checkpoint_sequence == existing->sequence &&
        data != payload(*existing->record, impl_->limits)) {
        throw std::invalid_argument("equal recovery epochs must describe equal frames");
    }
    if (existing && existing->record && record.document.revision == existing->revision &&
        (record.document.content_identity != existing->record->document.content_identity ||
         record.document.text.str() != existing->record->document.text.str())) {
        throw std::invalid_argument("equal recovery revisions must identify equal text");
    }
    if (!existing) {
        std::size_t count = 0;
        for (const auto& entry : fs::directory_iterator(impl_->directory)) {
            if (entry.path().filename() != ".lock" && ++count >= impl_->limits.max_records) {
                throw std::length_error("recovery record count exceeds limit");
            }
        }
    }
    (void)io::save_file(record_path(impl_->directory, id),
                        bytes,
                        io::TextEncoding::utf8,
                        existing ? std::optional(existing->version) : std::nullopt);
    impl_->erased.erase(id);
}

std::optional<RecoveryRecord> RecoveryStore::load(BufferId id) const {
    impl_->check();
    auto stored = read_stored(impl_->directory, id, impl_->limits);
    return stored ? std::move(stored->record) : std::nullopt;
}

SessionInfo RecoveryStore::inspect() const {
    impl_->check();
    return inspect_session(impl_->directory, impl_->id, true, impl_->limits);
}

void RecoveryStore::erase(BufferId id, Revision expected) {
    impl_->check();
    const auto existing = read_stored(impl_->directory, id, impl_->limits);
    if (!existing) {
        return;
    }
    if (existing->revision != expected) {
        throw StaleRevision();
    }
    const auto data = Json{{"id", id.value},
                           {"revision", expected.value},
                           {"sequence", existing->sequence},
                           {"deleted", true}};
    const auto path = record_path(impl_->directory, id);
    (void)io::save_file(
        path, envelope(data, impl_->limits), io::TextEncoding::utf8, existing->version);
    impl_->erased[id] = {expected, existing->sequence};
    // If a crash interrupts unlink, the already-flushed tombstone cannot revive
    // discarded content. In-process late worker snapshots remain revision-guarded.
    // Explicit epoch tombstones remain durable across session re-claim; pruning
    // removes them only when the entire abandoned session has no dirty records.
    if (existing->sequence != 0) {
        return;
    }
    if (!fs::remove(path)) {
        throw std::runtime_error("recovery tombstone was not removed");
    }
}

std::vector<SessionInfo> RecoveryStore::discover(const fs::path& state, RecoveryLimits limits) {
    validate_limits(limits);
    if (state.empty()) {
        throw std::invalid_argument("explicit recovery state directory is required");
    }
    if (!fs::exists(state)) {
        return {};
    }
    const auto root = fs::canonical(state);
    std::vector<SessionInfo> result;
    for (const auto& entry : fs::directory_iterator(root)) {
        if (result.size() >= limits.max_sessions) {
            throw std::length_error("recovery session count exceeds limit");
        }
        const auto name = entry.path().filename().string();
        const auto id = name.starts_with("session-") ? name.substr(8) : name;
        try {
            if (!name.starts_with("session-") || !valid_session(id)) {
                throw RecoveryCorrupt("unrecognized recovery state entry retained");
            }
            require_child(root, entry.path());
            std::unique_ptr<SessionLock> probe;
            bool active = false;
            try {
                probe = std::make_unique<SessionLock>(entry.path());
            } catch (const RecoveryLocked&) {
                active = true;
            }
            result.push_back(inspect_session(entry.path(), id, active, limits));
        } catch (const std::exception& error) {
            result.push_back({id, false, {}, {{entry.path(), error.what()}}});
        }
    }
    std::ranges::sort(result, [](const auto& a, const auto& b) {
        return a.id < b.id;
    });
    return result;
}

PruneResult RecoveryStore::prune_empty_sessions(const fs::path& state, RecoveryLimits limits) {
    validate_limits(limits);
    if (state.empty()) {
        throw std::invalid_argument("explicit recovery state directory is required");
    }
    if (!fs::exists(state)) {
        return {};
    }
    const auto root = fs::canonical(state);
    PruneResult result;
    std::vector<fs::path> candidates;
    {
        // Enumeration is bounded independently of the recoverable-session limit:
        // empty sessions from past clean starts must not consume that limit.
        for (const auto& entry : fs::directory_iterator(root)) {
            if (candidates.size() == 65'536) {
                throw std::length_error("recovery prune directory limit exceeded");
            }
            candidates.push_back(entry.path());
        }
    }
    for (const auto& directory : candidates) {
        try {
            const auto name = directory.filename().string();
            if (!name.starts_with("session-") || !valid_session(std::string_view(name).substr(8))) {
                result.issues.push_back({directory, "unrecognized recovery state entry retained"});
                continue;
            }
            require_child(root, directory);
            std::unique_ptr<SessionLock> owner;
            try {
                owner = std::make_unique<SessionLock>(directory);
            } catch (const RecoveryLocked&) {
                continue;
            }
            bool empty = true;
            std::vector<fs::path> tombstones;
            {
                for (const auto& entry : fs::directory_iterator(directory)) {
                    if (entry.path().filename() == ".lock") {
                        continue;
                    }
                    const auto id = filename_id(entry.path().filename().string());
                    if (!id) {
                        empty = false;
                        break;
                    }
                    const auto stored = read_stored(directory, *id, limits);
                    if (stored && stored->record) {
                        empty = false;
                        break;
                    }
                    if (tombstones.size() >= limits.max_records) {
                        throw std::length_error("recovery prune record limit exceeded");
                    }
                    tombstones.push_back(entry.path());
                }
            }
            if (!empty) {
                continue;
            }
            require_child(root, directory);
            for (const auto& path : tombstones) {
                require_child(directory, path);
                if (!fs::remove(path)) {
                    throw std::runtime_error("empty recovery tombstone disappeared");
                }
            }
            owner->remove_marker();
            // Another claimant can create a new .lock after our close; in that
            // race rmdir refuses a nonempty directory and preserves its owner.
#ifdef _WIN32
            if (!RemoveDirectoryW(directory.c_str())) {
                const auto error = GetLastError();
                if (error == ERROR_DIR_NOT_EMPTY || error == ERROR_FILE_NOT_FOUND ||
                    error == ERROR_PATH_NOT_FOUND) {
                    continue;
                }
                throw fs::filesystem_error(
                    "remove empty recovery session",
                    directory,
                    std::error_code(static_cast<int>(error), std::system_category()));
            }
            ++result.removed;
#else
            std::error_code error;
            if (fs::remove(directory, error)) {
                ++result.removed;
            } else if (error && error != std::errc::directory_not_empty &&
                       error != std::errc::no_such_file_or_directory) {
                throw fs::filesystem_error("remove empty recovery session", directory, error);
            }
#endif
        } catch (const std::exception& error) {
            result.issues.push_back({directory, error.what()});
        }
    }
    return result;
}

} // namespace astra::recovery
