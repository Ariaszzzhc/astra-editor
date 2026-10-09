#include <astra/service/service_host.hpp>
#include <astra/service/application_service.hpp>

#include <utility>
#include <cstdio>
#include <exception>
#include <stdexcept>

namespace astra::service {
struct ServiceHost::Impl {
    explicit Impl(ServiceHostOptions options)
        : server([this](std::string_view body) { return application->dispatch_text(body); },
                 [this] { return application->take_notifications(); }, {},
                 {[this, workspace = options.workspace, defaults = std::move(options.defaults_script),
                   user = std::move(options.user_script), resources = options.resource_directory,
                   recovery = options.recovery_directory, watch_files = options.watch_files] {
                      application = std::make_unique<ApplicationService>(workspace,
                          [this](auto worker, auto completion) {
                              return server.queue_work(std::move(worker), std::move(completion));
                          }, defaults, user, resources, recovery, watch_files);
                  }, [this] {
                      try { application->checkpoint_for_shutdown(); }
                      catch (...) { shutdown_error = std::current_exception(); }
                      application.reset();
                  }}) {
        server.start(options.address, options.port);
    }
    ~Impl() {
        server.stop();
        if (shutdown_error && !shutdown_reported) {
            try { std::rethrow_exception(shutdown_error); }
            catch (const std::exception& error) { std::fprintf(stderr, "Astra shutdown checkpoint failed: %s\n", error.what()); }
            catch (...) { std::fputs("Astra shutdown checkpoint failed\n", stderr); }
        }
    }
    void shutdown() {
        server.stop();
        if (shutdown_error) { shutdown_reported = true; std::rethrow_exception(shutdown_error); }
    }
    std::unique_ptr<ApplicationService> application;
    std::exception_ptr shutdown_error;
    bool shutdown_reported{};
    transport::TcpServer server;
};

ServiceHost::ServiceHost(ServiceHostOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
ServiceHost::~ServiceHost() = default;
std::future<std::optional<std::string>> ServiceHost::submit(std::string body) { return impl_->server.submit(std::move(body)); }
std::vector<std::string> ServiceHost::take_notifications() { return impl_->server.take_local_notifications(); }
std::uint16_t ServiceHost::port() const noexcept { return impl_->server.port(); }
transport::TcpServerStats ServiceHost::stats() const noexcept { return impl_->server.stats(); }
void ServiceHost::shutdown() { impl_->shutdown(); }
} // namespace astra::service
