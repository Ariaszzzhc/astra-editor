#include <astra/service/service_host.hpp>
#include <astra/service/application_service.hpp>

#include <utility>

namespace astra::service {
struct ServiceHost::Impl {
    explicit Impl(ServiceHostOptions options)
        : server([this](std::string_view body) { return application->dispatch_text(body); },
                 [this] { return application->take_notifications(); }, {},
                 {[this, workspace = options.workspace, defaults = std::move(options.defaults_script),
                   user = std::move(options.user_script)] {
                      application = std::make_unique<ApplicationService>(workspace,
                          [this](auto worker, auto completion) {
                              return server.queue_work(std::move(worker), std::move(completion));
                          }, defaults, user);
                  }, [this] { application.reset(); }}) {
        server.start(options.address, options.port);
    }
    ~Impl() { server.stop(); }
    std::unique_ptr<ApplicationService> application;
    transport::TcpServer server;
};

ServiceHost::ServiceHost(ServiceHostOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
ServiceHost::~ServiceHost() = default;
std::future<std::optional<std::string>> ServiceHost::submit(std::string body) { return impl_->server.submit(std::move(body)); }
std::vector<std::string> ServiceHost::take_notifications() { return impl_->server.take_local_notifications(); }
std::uint16_t ServiceHost::port() const noexcept { return impl_->server.port(); }
transport::TcpServerStats ServiceHost::stats() const noexcept { return impl_->server.stats(); }
} // namespace astra::service
