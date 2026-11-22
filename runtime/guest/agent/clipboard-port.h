#pragma once

#include <cerrno>
#include <csignal>
#include <string>
#include <string_view>
#include <unistd.h>

namespace clipboard {

constexpr size_t max_text = 64 * 1024;
constexpr unsigned char frame_start = 0xff; // Never occurs in valid UTF-8.
constexpr unsigned char frame_end = 0xfe;
extern volatile std::sig_atomic_t stopping;

class Port {
public:
    explicit Port(int fd) : fd(fd) {}

    const int fd;
    bool closed = false;

    bool wants_write() const { return !outgoing.empty(); }

    void ready() { send('R'); }

    void send_text(std::string_view text)
    {
        if (!text.empty() && text.size() <= max_text &&
            text.find_first_of("\xfe\xff") == std::string_view::npos) {
            send('T', text);
        }
    }

    void disconnect() const
    {
        // The start byte also discards any partially delivered text.
        const char message[] = {char(frame_start), 'D', char(frame_end)};
        (void)::write(fd, message, sizeof(message));
    }

    void write()
    {
        ssize_t n = ::write(fd, outgoing.data() + sent, outgoing.size() - sent);
        if (n <= 0) {
            if (n < 0 && errno != EINTR && errno != EAGAIN) closed = true;
            return;
        }
        sent += n;
        if (sent == outgoing.size()) {
            outgoing.swap(pending);
            pending.clear();
            sent = 0;
        }
    }

    template<class Publish>
    void read(Publish publish)
    {
        char buffer[4096];
        ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n <= 0) {
            if (n == 0 || (errno != EINTR && errno != EAGAIN)) closed = true;
            return;
        }
        for (unsigned char byte : std::string_view(buffer, n)) {
            if (byte == frame_start) {
                incoming.clear();
                in_frame = true;
            } else if (byte == frame_end) {
                if (in_frame && incoming.size() > 1 && incoming.front() == 'T') {
                    publish(incoming.substr(1));
                }
                in_frame = false;
            } else if (in_frame) {
                if (incoming.size() == max_text + 1) in_frame = false;
                else incoming.push_back(char(byte));
            }
        }
    }

private:
    void send(char kind, std::string_view text = {})
    {
        std::string &message = outgoing.empty() ? outgoing : pending;
        message.assign({char(frame_start), kind});
        message.append(text);
        message.push_back(char(frame_end));
    }

    std::string incoming;
    bool in_frame = false;
    std::string outgoing;
    size_t sent = 0;
    std::string pending;
};

bool run_x11(Port &port);

} // namespace clipboard
