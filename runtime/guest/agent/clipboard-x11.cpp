#include "clipboard-port.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <poll.h>
#include <utility>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

namespace clipboard {
namespace {

struct Free {
    void operator()(void *pointer) const { std::free(pointer); }
};

template<class T>
using Reply = std::unique_ptr<T, Free>;
using ClipboardChanged = std::function<void(std::string)>;

class Display {
public:
    ~Display()
    {
        if (connection) xcb_disconnect(connection);
    }

    bool connect(const char *name)
    {
        int screen_number = 0;
        connection = xcb_connect(name, &screen_number);
        if (xcb_connection_has_error(connection)) return false;
        const auto *extension = xcb_get_extension_data(connection, &xcb_xfixes_id);
        if (!extension || !extension->present) return false;
        xfixes_event = extension->first_event;
        const Reply<xcb_xfixes_query_version_reply_t> version(
            xcb_xfixes_query_version_reply(connection, xcb_xfixes_query_version(connection, 1, 0), nullptr));
        if (!version || version->major_version < 1) return false;

        auto screen = xcb_setup_roots_iterator(xcb_get_setup(connection));
        while (screen_number-- > 0) xcb_screen_next(&screen);
        root = screen.data->root;
        clipboard = atom("CLIPBOARD");
        targets = atom("TARGETS");
        timestamp = atom("TIMESTAMP");
        utf8 = atom("UTF8_STRING");
        mime_utf8 = atom("text/plain;charset=utf-8");
        plain = atom("text/plain");
        text = atom("TEXT");
        incr = atom("INCR");
        property = atom("_BRINEWELL_CLIPBOARD");
        if (!(clipboard && targets && timestamp && utf8 && mime_utf8 && plain && text && incr && property)) {
            return false;
        }
        owner_window = create_window();
        const uint32_t changes = XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
                                 XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
                                 XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE;
        const Reply<xcb_generic_error_t> error(xcb_request_check(connection,
            xcb_xfixes_select_selection_input_checked(connection, owner_window, clipboard, changes)));
        return !error && alive();
    }

    bool alive() const { return connection && !xcb_connection_has_error(connection); }
    int fd() const { return xcb_get_file_descriptor(connection); }

    void publish(const std::string &value)
    {
        cancel_read();
        owned_text = value;
        xcb_set_selection_owner(connection, owner_window, clipboard, XCB_CURRENT_TIME);
        xcb_flush(connection);
    }

    void events(const ClipboardChanged &changed)
    {
        while (const Reply<xcb_generic_event_t> event{xcb_poll_for_event(connection)}) {
            const auto type = event->response_type & 0x7f;
            if (type == xfixes_event + XCB_XFIXES_SELECTION_NOTIFY) {
                const auto &change = *reinterpret_cast<const xcb_xfixes_selection_notify_event_t *>(event.get());
                cancel_read();
                if (change.owner == owner_window) owned_since = change.selection_timestamp;
                else if (change.owner != XCB_NONE) {
                    reader = create_window();
                    read_time = change.timestamp;
                    request(utf8);
                }
            } else if (type == XCB_SELECTION_REQUEST) {
                serve(*reinterpret_cast<const xcb_selection_request_event_t *>(event.get()));
            } else if (type == XCB_SELECTION_NOTIFY) {
                const auto &notify = *reinterpret_cast<const xcb_selection_notify_event_t *>(event.get());
                if (reader == XCB_NONE || notify.requestor != reader) continue;
                if (notify.property == property) receive(changed);
                else if (notify.target == utf8) request(mime_utf8);
                else if (notify.target == mime_utf8) request(plain);
                else cancel_read();
            } else if (type == XCB_PROPERTY_NOTIFY && incremental) {
                const auto &notify = *reinterpret_cast<const xcb_property_notify_event_t *>(event.get());
                if (notify.window == reader && notify.atom == property && notify.state == XCB_PROPERTY_NEW_VALUE) {
                    receive(changed);
                }
            }
        }
        xcb_flush(connection);
    }

private:
    xcb_atom_t atom(const char *name) const
    {
        const Reply<xcb_intern_atom_reply_t> reply(xcb_intern_atom_reply(
            connection, xcb_intern_atom(connection, false, std::strlen(name), name), nullptr));
        return reply ? reply->atom : xcb_atom_t(XCB_NONE);
    }

    xcb_window_t create_window() const
    {
        const xcb_window_t window = xcb_generate_id(connection);
        const uint32_t events = XCB_EVENT_MASK_PROPERTY_CHANGE;
        xcb_create_window(connection, XCB_COPY_FROM_PARENT, window, root, 0, 0, 1, 1, 0,
                          XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, &events);
        return window;
    }

    void cancel_read()
    {
        if (reader != XCB_NONE) xcb_destroy_window(connection, reader);
        reader = XCB_NONE;
        incremental = false;
        received_text.clear();
    }

    void request(xcb_atom_t target) const
    {
        xcb_convert_selection(connection, reader, clipboard, target, property, read_time);
    }

    void receive(const ClipboardChanged &changed)
    {
        // Deleting the property acknowledges each INCR chunk. A new selection
        // replaces the reader window, so late replies cannot finish a stale copy.
        const Reply<xcb_get_property_reply_t> reply(xcb_get_property_reply(connection,
            xcb_get_property(connection, true, reader, property, XCB_GET_PROPERTY_TYPE_ANY, 0, max_text / 4), nullptr));
        if (!reply || reply->bytes_after) {
            cancel_read();
            return;
        }
        const auto length = size_t(xcb_get_property_value_length(reply.get()));
        if (reply->type == incr && !incremental && reply->format == 32 && length == sizeof(uint32_t)) {
            uint32_t minimum;
            std::memcpy(&minimum, xcb_get_property_value(reply.get()), sizeof(minimum));
            if (minimum <= max_text) incremental = true;
            else cancel_read();
            return;
        }
        if (reply->format != 8 || (reply->type != utf8 && reply->type != mime_utf8 && reply->type != plain) ||
            received_text.size() + length > max_text) {
            cancel_read();
            return;
        }
        received_text.append(static_cast<const char *>(xcb_get_property_value(reply.get())), length);
        if (incremental && length) return;

        if (!received_text.empty() && received_text.find_first_of("\xfe\xff") == std::string::npos) {
            changed(std::move(received_text));
        }
        cancel_read();
    }

    void serve(const xcb_selection_request_event_t &request) const
    {
        const xcb_atom_t dest = request.property ? request.property : request.target;
        xcb_atom_t result = XCB_NONE;
        if (request.selection == clipboard && !owned_text.empty()) {
            if (request.target == targets) {
                const xcb_atom_t supported[] = {targets, timestamp, utf8, mime_utf8, plain, text};
                xcb_change_property(connection, XCB_PROP_MODE_REPLACE, request.requestor,
                                    dest, XCB_ATOM_ATOM, 32, std::size(supported), supported);
                result = dest;
            } else if (request.target == timestamp) {
                xcb_change_property(connection, XCB_PROP_MODE_REPLACE, request.requestor,
                                    dest, XCB_ATOM_INTEGER, 32, 1, &owned_since);
                result = dest;
            } else if (request.target == utf8 || request.target == mime_utf8 ||
                       request.target == plain || request.target == text) {
                const xcb_atom_t type = request.target == text ? utf8 : request.target;
                xcb_change_property(connection, XCB_PROP_MODE_REPLACE, request.requestor,
                                    dest, type, 8, owned_text.size(), owned_text.data());
                result = dest;
            }
        }
        xcb_selection_notify_event_t notify{};
        notify.response_type = XCB_SELECTION_NOTIFY;
        notify.time = request.time;
        notify.requestor = request.requestor;
        notify.selection = request.selection;
        notify.target = request.target;
        notify.property = result;
        // SendEvent reads a full 32-byte event, including the unused padding.
        std::array<char, 32> wire{};
        std::memcpy(wire.data(), &notify, sizeof(notify));
        xcb_send_event(connection, false, request.requestor, XCB_EVENT_MASK_NO_EVENT, wire.data());
    }

    xcb_connection_t *connection = nullptr;
    uint8_t xfixes_event = 0;
    xcb_window_t root = XCB_NONE, owner_window = XCB_NONE, reader = XCB_NONE;
    xcb_atom_t clipboard, targets, timestamp, utf8, mime_utf8, plain, text, incr, property;
    xcb_timestamp_t owned_since = XCB_CURRENT_TIME, read_time = XCB_CURRENT_TIME;
    bool incremental = false;
    std::string owned_text, received_text;
};

} // namespace

bool run_x11(Port &port)
{
    const char *steam = std::getenv("DISPLAY");
    const char *game = std::getenv("STEAM_GAME_DISPLAY_0");
    std::array<std::unique_ptr<Display>, 2> displays;
    const char *names[] = {steam, game};
    for (size_t i = 0; i < displays.size(); ++i) {
        if (!names[i] || !*names[i] || (i == 1 && steam && std::strcmp(steam, game) == 0)) continue;
        auto display = std::make_unique<Display>();
        if (display->connect(names[i])) displays[i] = std::move(display);
        else std::fprintf(stderr, "Brinewell clipboard: cannot watch Gamescope display %s\n", names[i]);
    }
    if (!displays[0] && !displays[1]) return false;

    std::string last_text;
    port.ready();
    while (!port.closed && !stopping) {
        std::array<pollfd, 3> fds{};
        fds[0] = {port.fd, POLLIN, 0};
        for (size_t i = 0; i < displays.size(); ++i) {
            if (displays[i]) {
                displays[i]->events([&](std::string value) {
                    // Gamescope also copies selections between these servers.
                    // Publish the new value on its peer before that server next
                    // handles an app's copy, and ignore repeated announcements.
                    if (value == last_text) return;
                    last_text = std::move(value);
                    port.send_text(last_text);
                    for (size_t peer = 0; peer < displays.size(); ++peer) {
                        if (peer != i && displays[peer]) displays[peer]->publish(last_text);
                    }
                });
                if (!displays[i]->alive()) displays[i].reset();
            }
            fds[i + 1] = {displays[i] ? displays[i]->fd() : -1, POLLIN, 0};
        }
        if (!displays[0] && !displays[1]) break;
        if (port.wants_write()) fds[0].events |= POLLOUT;
        const int count = poll(fds.data(), fds.size(), -1);
        if (count < 0) {
            if (errno == EINTR) continue;
            break;
        }
        constexpr short errors = POLLERR | POLLHUP | POLLNVAL;
        if (fds[0].revents & errors) break;
        for (size_t i = 0; i < displays.size(); ++i) {
            if (fds[i + 1].revents & errors) displays[i].reset();
        }
        if (fds[0].revents & POLLIN) {
            port.read([&](std::string value) {
                last_text = std::move(value);
                for (auto &display : displays) {
                    if (display) display->publish(last_text);
                }
            });
        }
        if (fds[0].revents & POLLOUT) port.write();
    }
    return true;
}

} // namespace clipboard
