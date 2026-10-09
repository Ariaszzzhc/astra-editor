#include <astra/io/file_store.hpp>
#include <astra/service/service_host.hpp>
#include <astra/transport/tcp_client.hpp>
#include <astra/transport/editor_channel.hpp>

#ifdef ASTRA_WITH_GUI
#include <astra/ui/editor_window.hpp>
#endif

#include <uv.h>

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
namespace fs = std::filesystem;

fs::path utf8_path(std::string_view text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

[[maybe_unused]] std::string utf8_path_text(const fs::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

fs::path executable_directory() {
    std::vector<char> buffer(32768);
    auto size = buffer.size();
    const auto status = uv_exepath(buffer.data(), &size);
    if (status < 0) { throw std::runtime_error(std::string("Cannot locate executable: ") + uv_strerror(status)); }
    return utf8_path(std::string_view(buffer.data(), size)).parent_path();
}

fs::path default_recovery_directory(const fs::path& workspace) {
    std::vector<char> home(32768);
    auto size = home.size();
#ifdef _WIN32
    auto status = uv_os_getenv("LOCALAPPDATA", home.data(), &size);
#else
    auto status = uv_os_getenv("XDG_STATE_HOME", home.data(), &size);
#endif
    fs::path state;
    if (status == 0) { state = utf8_path(std::string_view(home.data(), size)); }
    else {
        size = home.size();
        status = uv_os_homedir(home.data(), &size);
        if (status != 0) { throw std::runtime_error("Cannot locate recovery directory; pass --state-directory"); }
        state = utf8_path(std::string_view(home.data(), size));
#ifdef __APPLE__
        state /= "Library/Application Support";
#else
        state /= ".local/state";
#endif
    }
    // Stable per-workspace namespace. Every recovered source path is still
    // independently validated against the active workspace before restoration.
    const auto key = fs::canonical(workspace).generic_u8string();
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : key) { hash = (hash ^ static_cast<unsigned char>(byte)) * 1099511628211ULL; }
    std::ostringstream name;
    name << std::hex << hash;
    return state / "Astra" / "recovery" / name.str();
}

std::uint16_t port_number(std::string_view text, bool allow_zero) {
    unsigned value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value > 65535 || (!allow_zero && value == 0)) {
        throw std::invalid_argument("Port must be a valid decimal TCP port");
    }
    return static_cast<std::uint16_t>(value);
}

std::pair<std::string, std::uint16_t> endpoint(std::string_view text) {
    const auto split = text.rfind(':');
    if (split == std::string_view::npos) { throw std::invalid_argument("Endpoint requires host:port, or [::1]:port"); }
    auto host = text.substr(0, split);
    if (host.starts_with('[') && host.ends_with(']')) { host = host.substr(1, host.size() - 2); }
    if (host.empty()) { throw std::invalid_argument("Endpoint host is empty"); }
    return {std::string(host), port_number(text.substr(split + 1), false)};
}

class ServerSignals {
public:
    ServerSignals() {
        int status = uv_loop_init(&loop_);
        if (status < 0) { throw std::runtime_error(uv_strerror(status)); }
        for (auto* handle : {&interrupt_, &terminate_}) {
            status = uv_signal_init(&loop_, handle);
            if (status < 0) { cleanup(); throw std::runtime_error(uv_strerror(status)); }
            handle->data = &loop_;
        }
        status = uv_signal_start(&interrupt_, stop, SIGINT);
        if (status == 0) { status = uv_signal_start(&terminate_, stop, SIGTERM); }
        if (status < 0) { cleanup(); throw std::runtime_error(uv_strerror(status)); }
    }
    ~ServerSignals() { cleanup(); }
    void wait() { static_cast<void>(uv_run(&loop_, UV_RUN_DEFAULT)); }
private:
    static void stop(uv_signal_t* handle, int) { uv_stop(static_cast<uv_loop_t*>(handle->data)); }
    void cleanup() noexcept {
        uv_walk(&loop_, [](uv_handle_t* handle, void*) {
            if (!uv_is_closing(handle)) { uv_close(handle, nullptr); }
        }, nullptr);
        static_cast<void>(uv_run(&loop_, UV_RUN_DEFAULT));
        static_cast<void>(uv_loop_close(&loop_));
    }
    uv_loop_t loop_{};
    uv_signal_t interrupt_{};
    uv_signal_t terminate_{};
};

int run(const std::vector<std::string>& arguments) {
    astra::service::ServiceHostOptions options;
    options.workspace = fs::current_path();
#ifdef ASTRA_WITH_GUI
    bool server_mode = false;
#else
    bool server_mode = true;
#endif
    std::string remote;
    std::string request;
    std::string user_config;
    std::vector<std::string> paths;
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const auto& arg = arguments[i];
        const auto value = [&]() -> const std::string& {
            if (++i == arguments.size()) { throw std::invalid_argument("Missing value for " + arg); }
            return arguments[i];
        };
        if (arg == "--help" || arg == "-h") {
            std::cout << "Astra 0.1.0\n"
                         "  astra [--workspace DIR] [--open RELATIVE_FILE] [--config RELATIVE_JS]\n"
                         "  astra --server [--workspace DIR] [--host 127.0.0.1|::1] [--port PORT]\n"
                         "  astra --connect HOST:PORT\n"
                         "  astra-server --connect HOST:PORT --request JSON_RPC_OBJECT\n"
                         "  --state-directory DIR overrides the local recovery storage directory.\n"
                         "Remote connections use a loopback endpoint through an authenticated SSH tunnel.\n"
                         "Ctrl+C stops server mode after outstanding workers finish.\n";
            return 0;
        }
        if (arg == "--version") { std::cout << "Astra 0.1.0\n"; return 0; }
        if (arg == "--server") { server_mode = true; }
        else if (arg == "--workspace") { options.workspace = utf8_path(value()); }
        else if (arg == "--host") { options.address = value(); }
        else if (arg == "--port") { options.port = port_number(value(), true); }
        else if (arg == "--connect") { remote = value(); }
        else if (arg == "--request") { request = value(); }
        else if (arg == "--config") { user_config = value(); }
        else if (arg == "--state-directory") { options.recovery_directory = utf8_path(value()); }
        else if (arg == "--open") { paths.push_back(value()); }
        else if (arg.starts_with('-')) { throw std::invalid_argument("Unknown option: " + arg); }
        else { paths.push_back(arg); }
    }

    std::shared_ptr<astra::ui::EditorChannel> channel;
    std::shared_ptr<astra::service::ServiceHost> host;
#ifdef ASTRA_WITH_GUI
    const auto workspace_name = utf8_path_text(fs::absolute(options.workspace).lexically_normal().filename());
    const auto configured_workspace = options.workspace;
#endif
    if (!remote.empty()) {
        const auto [address, port] = endpoint(remote);
        channel = std::make_shared<astra::transport::TcpEditorChannel>(address, port);
    } else {
        options.resource_directory = executable_directory();
        if (options.recovery_directory.empty()) { options.recovery_directory = default_recovery_directory(options.workspace); }
        options.defaults_script = astra::io::read_file(options.resource_directory / "data" / "defaults.js", 1024 * 1024).text;
        if (!user_config.empty()) {
            const auto relative = utf8_path(user_config);
            if (relative.is_absolute() || relative.has_root_name()) { throw std::invalid_argument("--config must be workspace-relative"); }
            const auto root = fs::canonical(options.workspace);
            const auto path = fs::canonical(root / relative);
            const auto checked = path.lexically_relative(root);
            if (checked.empty() || checked.is_absolute()) { throw std::invalid_argument("Config is outside the workspace"); }
            for (const auto& part : checked) {
                if (part == "..") { throw std::invalid_argument("Config is outside the workspace"); }
            }
            options.user_script = astra::io::read_file(path, 1024 * 1024).text;
        }
        host = std::make_shared<astra::service::ServiceHost>(std::move(options));
        channel = host;
    }
    if (!request.empty()) {
        const auto response = channel->submit(std::move(request)).get();
        if (response) { std::cout << *response << '\n'; }
        if (host) { host->shutdown(); }
        return 0;
    }
    if (server_mode) {
        if (!host) { throw std::invalid_argument("A headless connection requires --request"); }
        ServerSignals signals;
        std::cout << "{\"application\":\"Astra\",\"port\":" << host->port() << "}" << std::endl;
        signals.wait();
        host->shutdown();
        return 0;
    }
#ifdef ASTRA_WITH_GUI
    astra::ui::EditorWindowOptions window_options;
    window_options.resource_directory = executable_directory();
    if (!user_config.empty()) {
        const auto root = fs::canonical(configured_workspace);
        const auto relative = utf8_path(user_config);
        if (relative.is_absolute() || relative.has_root_name()) { throw std::invalid_argument("--config must be workspace-relative"); }
        const auto path = fs::canonical(root / relative);
        const auto checked = path.lexically_relative(root);
        if (checked.empty() || checked.is_absolute()) { throw std::invalid_argument("Config is outside the workspace"); }
        for (const auto& part : checked) { if (part == "..") { throw std::invalid_argument("Config is outside the workspace"); } }
        window_options.user_script = astra::io::read_file(path, 1024 * 1024).text;
    }
    window_options.open_paths = std::move(paths);
    window_options.workspace_name = remote.empty() ? workspace_name : remote;
    astra::ui::EditorWindow window(channel, std::move(window_options));
    const auto result = window.run();
    if (host) { host->shutdown(); }
    return result;
#else
    throw std::invalid_argument("This binary contains the headless service; use astra for the GUI");
#endif
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    try {
        std::vector<std::string> arguments;
        arguments.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i) {
            const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
            if (!size) { throw std::runtime_error("Invalid UTF-16 command-line argument"); }
            std::string text(static_cast<std::size_t>(size), '\0');
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, text.data(), size, nullptr, nullptr)) {
                throw std::runtime_error("Cannot convert command-line argument");
            }
            text.pop_back();
            arguments.push_back(std::move(text));
        }
        return run(arguments);
    } catch (const std::exception& error) { std::cerr << "Astra: " << error.what() << '\n'; return 1; }
}
#else
int main(int argc, char* argv[]) {
    try {
        return run(std::vector<std::string>(argv, argv + argc));
    } catch (const std::exception& error) { std::cerr << "Astra: " << error.what() << '\n'; return 1; }
}
#endif
