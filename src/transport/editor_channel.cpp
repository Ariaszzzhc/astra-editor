#include <astra/transport/editor_channel.hpp>

#include <utility>

namespace astra::transport {

TcpEditorChannel::TcpEditorChannel(std::string host, std::uint16_t port) {
    client_.connect(std::move(host), port).get();
}
std::future<std::optional<std::string>> TcpEditorChannel::submit(std::string body) {
    return client_.submit(std::move(body));
}
std::vector<std::string> TcpEditorChannel::take_notifications() {
    return client_.take_notifications();
}

} // namespace astra::transport
