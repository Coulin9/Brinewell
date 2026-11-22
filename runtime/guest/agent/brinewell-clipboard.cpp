#include "clipboard-port.h"
#include "ext-data-control-client.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

volatile std::sig_atomic_t clipboard::stopping = 0;

namespace {

using clipboard::max_text;
constexpr char utf8_text[] = "text/plain;charset=utf-8";
constexpr char x11_utf8[] = "UTF8_STRING";
constexpr char plain_text[] = "text/plain";
constexpr char host_origin[] = "application/x-brinewell-clipboard";
constexpr char legacy_manager_name[] = "zwlr_data_control_manager_v1";

// ext-data-control and wlr-data-control v1 have the same wire messages for
// the regular clipboard. KWin 6.2 advertises only the older registry name.
const wl_interface legacy_manager_interface = [] {
    wl_interface interface = ext_data_control_manager_v1_interface;
    interface.name = legacy_manager_name;
    return interface;
}();

void stop(int) { clipboard::stopping = 1; }

struct Offer {
    const char *type = nullptr;
    bool from_host = false;
};

struct Paste {
    int fd;
    std::shared_ptr<const std::string> text;
    size_t offset = 0;
};

struct Clipboard {
    wl_display *display;
    clipboard::Port &port;
    wl_seat *seat = nullptr;
    uint32_t manager_name = 0;
    uint32_t legacy_name = 0;
    ext_data_control_manager_v1 *manager = nullptr;
    ext_data_control_device_v1 *device = nullptr;
    ext_data_control_source_v1 *source = nullptr;
    std::shared_ptr<const std::string> host_text;
    bool initial_selection = true;
    bool finished = false;
    int selection_fd = -1;
    std::string selection_text;
    std::vector<Paste> pastes;
    Clipboard(wl_display *display, clipboard::Port &port) : display(display), port(port) {}

    void publish(std::string text)
    {
        if (source) ext_data_control_source_v1_destroy(source);
        host_text = std::make_shared<const std::string>(std::move(text));
        source = ext_data_control_manager_v1_create_data_source(manager);
        static const ext_data_control_source_v1_listener listener = {
            .send = [](void *data, ext_data_control_source_v1 *, const char *type, int32_t fd) {
                auto &self = *static_cast<Clipboard *>(data);
                int flags = fcntl(fd, F_GETFL);
                if (std::string_view(type) == host_origin || self.pastes.size() == 16 ||
                    flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
                    close(fd);
                    return;
                }
                self.pastes.push_back({fd, self.host_text});
            },
            .cancelled = [](void *data, ext_data_control_source_v1 *source) {
                auto &self = *static_cast<Clipboard *>(data);
                ext_data_control_source_v1_destroy(source);
                self.source = nullptr;
                self.host_text.reset();
            },
        };
        ext_data_control_source_v1_add_listener(source, &listener, this);
        for (const char *type : {utf8_text, plain_text, x11_utf8, host_origin}) {
            ext_data_control_source_v1_offer(source, type);
        }
        ext_data_control_device_v1_set_selection(device, source);
    }

    void close_selection()
    {
        if (selection_fd != -1) close(selection_fd);
        selection_fd = -1;
        selection_text.clear();
    }

    void receive_selection(ext_data_control_offer_v1 *offer)
    {
        close_selection();
        if (initial_selection) {
            // The initial event describes clipboard state, not a new user copy.
            initial_selection = false;
            port.ready();
        } else if (offer) {
            auto &info = *static_cast<Offer *>(ext_data_control_offer_v1_get_user_data(offer));
            int pipe_fds[2];
            if (info.type && !info.from_host && pipe2(pipe_fds, O_CLOEXEC) == 0) {
                if (fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK) != -1) {
                    selection_fd = pipe_fds[0];
                    ext_data_control_offer_v1_receive(offer, info.type, pipe_fds[1]);
                } else {
                    close(pipe_fds[0]);
                }
                close(pipe_fds[1]);
            }
        }
        destroy_offer(offer);
    }

    void read_selection()
    {
        char buffer[4096];
        ssize_t n = read(selection_fd, buffer, sizeof(buffer));
        if (n == 0) port.send_text(selection_text);
        if (n > 0 && selection_text.size() + n <= max_text) {
            selection_text.append(buffer, n);
        } else if (n >= 0 || (errno != EINTR && errno != EAGAIN)) {
            close_selection();
        }
    }

    static void destroy_offer(ext_data_control_offer_v1 *offer)
    {
        if (!offer) return;
        delete static_cast<Offer *>(ext_data_control_offer_v1_get_user_data(offer));
        ext_data_control_offer_v1_destroy(offer);
    }

    bool bind()
    {
        wl_registry *registry = wl_display_get_registry(display);
        static const wl_registry_listener registry_listener = {
            .global = [](void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t) {
                auto &self = *static_cast<Clipboard *>(data);
                if (!self.seat && std::string_view(interface) == wl_seat_interface.name) {
                    self.seat = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
                } else if (std::string_view(interface) == ext_data_control_manager_v1_interface.name) {
                    self.manager_name = name;
                } else if (std::string_view(interface) == legacy_manager_name) {
                    self.legacy_name = name;
                }
            },
            .global_remove = [](void *, wl_registry *, uint32_t) {},
        };
        wl_registry_add_listener(registry, &registry_listener, this);
        if (wl_display_roundtrip(display) < 0 || !seat || (!manager_name && !legacy_name)) return false;
        if (manager_name) {
            manager = static_cast<ext_data_control_manager_v1 *>(
                wl_registry_bind(registry, manager_name, &ext_data_control_manager_v1_interface, 1));
        } else {
            manager = static_cast<ext_data_control_manager_v1 *>(
                wl_registry_bind(registry, legacy_name, &legacy_manager_interface, 1));
        }
        device = ext_data_control_manager_v1_get_data_device(manager, seat);
        static const ext_data_control_device_v1_listener device_listener = {
            .data_offer = [](void *, ext_data_control_device_v1 *, ext_data_control_offer_v1 *offer) {
                static const ext_data_control_offer_v1_listener offer_listener = {
                    .offer = [](void *data, ext_data_control_offer_v1 *, const char *type) {
                        auto &info = *static_cast<Offer *>(data);
                        std::string_view mime(type);
                        if (mime == host_origin) info.from_host = true;
                        if (mime == utf8_text) info.type = utf8_text;
                        else if (mime == x11_utf8 && info.type != utf8_text) info.type = x11_utf8;
                        else if (mime == plain_text && !info.type) info.type = plain_text;
                    },
                };
                ext_data_control_offer_v1_add_listener(offer, &offer_listener, new Offer);
            },
            .selection = [](void *data, ext_data_control_device_v1 *, ext_data_control_offer_v1 *offer) {
                static_cast<Clipboard *>(data)->receive_selection(offer);
            },
            .finished = [](void *data, ext_data_control_device_v1 *) {
                static_cast<Clipboard *>(data)->finished = true;
            },
            .primary_selection = [](void *, ext_data_control_device_v1 *, ext_data_control_offer_v1 *offer) {
                destroy_offer(offer);
            },
        };
        ext_data_control_device_v1_add_listener(device, &device_listener, this);
        return true;
    }

    void flush_paste(size_t index)
    {
        auto &paste = pastes[index];
        ssize_t n = write(paste.fd, paste.text->data() + paste.offset, paste.text->size() - paste.offset);
        if (n > 0) paste.offset += n;
        if (paste.offset == paste.text->size() || (n < 0 && errno != EINTR && errno != EAGAIN)) {
            close(paste.fd);
            pastes.erase(pastes.begin() + index);
        }
    }

    void run()
    {
        const int wayland = wl_display_get_fd(display);
        while (!finished && !port.closed && !clipboard::stopping) {
            if (wl_display_dispatch_pending(display) < 0 || finished) break;
            if (wl_display_prepare_read(display) < 0) continue;
            const int flushed = wl_display_flush(display);
            if (flushed < 0 && errno != EAGAIN) {
                wl_display_cancel_read(display);
                break;
            }
            std::vector<pollfd> fds = {
                {wayland, short(POLLIN | (flushed < 0 ? POLLOUT : 0)), 0},
                {port.fd, short(POLLIN | (port.wants_write() ? POLLOUT : 0)), 0},
                {selection_fd, POLLIN, 0},
            };
            for (const auto &paste : pastes) fds.push_back({paste.fd, POLLOUT, 0});
            int ready = poll(fds.data(), fds.size(), -1);
            if (ready > 0 && (fds[0].revents & POLLIN)) {
                if (wl_display_read_events(display) < 0) break;
            } else {
                wl_display_cancel_read(display);
            }
            if (ready < 0) {
                if (errno == EINTR) continue;
                break;
            }
            constexpr short port_errors = POLLERR | POLLHUP | POLLNVAL;
            if ((fds[0].revents | fds[1].revents) & port_errors) break;
            if (fds[1].revents & POLLIN) port.read([this](std::string text) { publish(std::move(text)); });
            if (fds[1].revents & POLLOUT) port.write();
            if (fds[2].revents) read_selection();
            for (size_t i = pastes.size(); i > 0; --i) {
                if (fds[i + 2].revents) flush_paste(i - 1);
            }
        }
    }
};

bool run_wayland(clipboard::Port &port)
{
    wl_display *display = wl_display_connect(nullptr);
    if (!display) return false;
    Clipboard bridge(display, port);
    const bool bound = bridge.bind();
    if (bound) bridge.run();
    else std::fputs("Brinewell clipboard: Wayland data control unavailable\n", stderr);
    wl_display_disconnect(display);
    return bound;
}

} // namespace

int main(int argc, char **argv)
{
    const bool gamescope = argc == 2 && std::string_view(argv[1]) == "--gamescope";
    if (argc != 1 && !gamescope) return 1;

    int fd = open("/dev/virtio-ports/fx.clipboard", O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? 0 : 1;
    signal(SIGPIPE, SIG_IGN);
    struct sigaction action {};
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);

    clipboard::Port port(fd);
    const bool connected = gamescope ? clipboard::run_x11(port) : run_wayland(port);
    if (connected) port.disconnect();
    close(fd);
    return connected ? 0 : 1;
}
