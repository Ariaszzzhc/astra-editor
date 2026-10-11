#include <astra/core/utf8.hpp>
#include <astra/ui/clipboard_client.hpp>
#include <astra/ui/clipboard_text.hpp>
#include <astra/protocol/framing.hpp>
#include <SDL3/SDL.h>
#include <uv.h>
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using Json = astra::protocol::Json;
using Client = astra::ui::ClipboardClient;

void check(int status, const char* operation) {
    if (status < 0)
        throw std::runtime_error(std::string(operation) + ": " + uv_strerror(status));
}

struct WindowDeleter {
    void operator()(SDL_Window* window) const noexcept {
        SDL_DestroyWindow(window);
    }
};

class Host final {
  public:
    Host() {
        check(uv_loop_init(&loop_), "Initialize clipboard helper loop");
        loop_ready_ = true;
        try {
            check(uv_pipe_init(&loop_, &input_, 0), "Initialize clipboard stdin");
            input_.data = this;
            check(uv_pipe_open(&input_, 0), "Open clipboard stdin");
            check(uv_pipe_init(&loop_, &output_, 0), "Initialize clipboard stdout");
            output_.data = this;
            check(uv_pipe_open(&output_, 1), "Open clipboard stdout");
            check(uv_timer_init(&loop_, &timer_), "Initialize clipboard event pump");
            timer_.data = this;
            if (!SDL_Init(SDL_INIT_VIDEO))
                throw std::runtime_error(std::string("Initialize clipboard SDL: ") +
                                         SDL_GetError());
            sdl_ready_ = true;
            window_.reset(SDL_CreateWindow("Astra clipboard owner", 1, 1, SDL_WINDOW_HIDDEN));
            if (!window_)
                throw std::runtime_error(std::string("Create clipboard owner: ") + SDL_GetError());
            check(uv_read_start(reinterpret_cast<uv_stream_t*>(&input_), allocate, read),
                  "Read clipboard requests");
            check(uv_timer_start(&timer_, tick, 5, 5), "Pump clipboard owner events");
        } catch (...) {
            close();
            static_cast<void>(uv_run(&loop_, UV_RUN_DEFAULT));
            const auto status = uv_loop_close(&loop_);
            loop_ready_ = false;
            if (status < 0)
                std::cerr << "Clipboard initialization cleanup failed: " << uv_strerror(status)
                          << '\n';
            window_.reset();
            if (sdl_ready_)
                SDL_Quit();
            throw;
        }
    }

    ~Host() {
        if (loop_ready_) {
            close();
            static_cast<void>(uv_run(&loop_, UV_RUN_DEFAULT));
            const auto status = uv_loop_close(&loop_);
            if (status < 0)
                std::cerr << "Clipboard loop cleanup failed: " << uv_strerror(status) << '\n';
        }
        window_.reset();
        if (sdl_ready_)
            SDL_Quit();
    }

    int run() {
        static_cast<void>(uv_run(&loop_, UV_RUN_DEFAULT));
        const auto status = uv_loop_close(&loop_);
        if (status < 0) {
            std::cerr << "Clipboard loop cleanup failed: " << uv_strerror(status) << '\n';
            exit_failed_ = true;
        } else {
            loop_ready_ = false;
        }
        return exit_failed_ ? 1 : 0;
    }

  private:
    uv_loop_t loop_{};
    uv_pipe_t input_{}, output_{};
    uv_timer_t timer_{};
    uv_write_t writing_{};
    std::array<char, 65536> buffer_{};
    astra::protocol::FrameDecoder decoder_{Client::max_wire_bytes};
    std::unique_ptr<SDL_Window, WindowDeleter> window_;
    std::string response_;
    bool loop_ready_{}, sdl_ready_{}, received_{}, write_{}, replying_{}, closing_{};

    void close() noexcept {
        if (closing_ || !loop_ready_)
            return;
        closing_ = true;
        uv_walk(
            &loop_,
            [](uv_handle_t* handle, void*) {
                if (!uv_is_closing(handle))
                    uv_close(handle, nullptr);
            },
            nullptr);
    }

    void fail(const std::exception& error) noexcept {
        std::cerr << "Clipboard helper failed: " << error.what() << '\n';
        // Record a fixed failure without allocating in an exception handler.
        exit_failed_ = true;
        close();
    }

    bool exit_failed_{};

    static void allocate(uv_handle_t* handle, std::size_t, uv_buf_t* result) noexcept {
        auto& host = *static_cast<Host*>(handle->data);
        *result = uv_buf_init(host.buffer_.data(), static_cast<unsigned>(host.buffer_.size()));
    }

    static void read(uv_stream_t* stream, std::ptrdiff_t count, const uv_buf_t* bytes) noexcept {
        auto& host = *static_cast<Host*>(stream->data);
        try {
            if (count < 0) {
                if (count != UV_EOF)
                    check(static_cast<int>(count), "Read clipboard stdin");
                host.decoder_.finish();
                if (!host.received_)
                    throw std::runtime_error("Clipboard request ended before a complete frame");
                host.close();
                return;
            }
            if (count == 0)
                return;
            const auto frames = host.decoder_.feed({bytes->base, static_cast<std::size_t>(count)});
            if (frames.size() > 1 || (!frames.empty() && host.received_))
                throw std::runtime_error("Clipboard helper accepts exactly one request");
            if (frames.empty())
                return;
            host.received_ = true;
            host.perform(Json::parse(frames.front()));
        } catch (const std::exception& error) {
            host.fail(error);
        }
    }

    void perform(const Json& request) {
        const auto id = request.at("id").get<std::string>();
        if (id.empty() || id.size() > 128)
            throw std::runtime_error("Invalid clipboard operation identity");
        Json response{{"id", id}};
        try {
            const auto kind = request.at("kind").get<std::string>();
            const auto limit = request.at("limit").get<std::size_t>();
            if (limit == 0 || limit > Client::max_text_bytes)
                throw std::runtime_error("Invalid clipboard read limit");
            write_ = kind == "write";
            if (kind == "read") {
                response["result"] = astra::ui::read_clipboard_text(limit);
            } else if (write_) {
                const auto text = request.at("text").get<std::string>();
                if (text.size() > Client::max_text_bytes || text.find('\0') != text.npos)
                    throw std::runtime_error(
                        "Clipboard write exceeds its byte bound or contains NUL");
                astra::utf8::validate(text);
                if (!SDL_SetClipboardText(text.c_str()))
                    throw std::runtime_error(std::string("Clipboard write failed: ") +
                                             SDL_GetError());
                response["result"] = true;
            } else {
                throw std::runtime_error("Unknown clipboard operation");
            }
        } catch (const std::exception& error) {
            response["error"] = error.what();
            write_ = false;
        }
        response_ = astra::protocol::encode_frame(response.dump(), Client::max_wire_bytes);
        writing_.data = this;
        auto buffer = uv_buf_init(response_.data(), static_cast<unsigned>(response_.size()));
        replying_ = true;
        check(uv_write(&writing_, reinterpret_cast<uv_stream_t*>(&output_), &buffer, 1, written),
              "Write clipboard reply");
    }

    static void written(uv_write_t* request, int status) noexcept {
        auto& host = *static_cast<Host*>(request->data);
        host.replying_ = false;
        if (status < 0) {
            if (!host.closing_) {
                std::cerr << "Clipboard reply failed: " << uv_strerror(status) << '\n';
                host.exit_failed_ = true;
            }
            host.close();
        } else if (!host.write_) {
            host.close();
        }
    }

    static void tick(uv_timer_t* timer) noexcept {
        auto& host = *static_cast<Host*>(timer->data);
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT)
                host.close();
        }
    }
};
} // namespace

int main() {
    try {
        Host host;
        return host.run();
    } catch (const std::exception& error) {
        std::cerr << "Clipboard helper startup failed: " << error.what() << '\n';
        return 1;
    }
}
