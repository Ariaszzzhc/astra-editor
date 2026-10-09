#pragma once

#include <astra/transport/tcp_server.hpp>
#include <astra/ui/editor_channel.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace astra::service {

struct ServiceHostOptions {
    std::filesystem::path workspace;
    std::string address{"127.0.0.1"};
    std::uint16_t port{};
    std::string defaults_script;
    std::string user_script;
};

// Local GUI, TCP clients, scripts, and external automation all enter one loop.
// Destruction waits for outstanding disk workers before releasing JS/documents.
class ServiceHost final : public ui::EditorChannel {
public:
    explicit ServiceHost(ServiceHostOptions options);
    ~ServiceHost() override;
    ServiceHost(const ServiceHost&) = delete;
    ServiceHost& operator=(const ServiceHost&) = delete;
    [[nodiscard]] std::future<std::optional<std::string>> submit(std::string body) override;
    [[nodiscard]] std::vector<std::string> take_notifications() override;
    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] transport::TcpServerStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace astra::service
