#pragma once

#include <astra/transport/tcp_client.hpp>
#include <astra/ui/editor_channel.hpp>

#include <cstdint>
#include <string>

namespace astra::transport {

// The remote GUI uses the same asynchronous channel interface as ServiceHost.
// Construction completes the bounded connection phase before window creation.
class TcpEditorChannel final : public ui::EditorChannel {
public:
    TcpEditorChannel(std::string host, std::uint16_t port);
    [[nodiscard]] std::future<std::optional<std::string>> submit(std::string body) override;
    [[nodiscard]] std::vector<std::string> take_notifications() override;
private:
    TcpClient client_;
};

} // namespace astra::transport
