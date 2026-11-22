// Linux-only focused checks for nonblocking output and line recovery.
#define _GNU_SOURCE
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>

static ssize_t relay_test_write(int fd, const void *data, size_t length);
#define write relay_test_write
#define main agent_main
#include "../../runtime/guest/agent/brinewell-agent.c"
#undef main
#undef write

static int partial_fd = -1;

static ssize_t relay_test_write(int fd, const void *data, size_t length) {
    if (fd == partial_fd && length > 1) {
        partial_fd = -1;
        return write(fd, data, length / 2);
    }
    return write(fd, data, length);
}

int main(void) {
    char packet[GAME_FRAME_MESSAGE_MAX];
    size_t length = (size_t)snprintf(packet, sizeof(packet),
        "game-frame 2 neptune 321 42 1920 1080 1 128 256000000 1 1 ");
    for (unsigned i = 0; i < GAME_FRAME_BATCH_MAX; ++i)
        length += (size_t)snprintf(packet + length, sizeof(packet) - length,
                                   "2000000%c", i + 1 == GAME_FRAME_BATCH_MAX ? '\n' : ',');
    assert(length > 256 && length <= 4096);
    assert(valid_game_frame(packet, length));
    int sockets[2], output[2];
    assert(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) == 0);
    assert(pipe2(output, O_NONBLOCK | O_CLOEXEC) == 0);
    int interrupted = 0;

    // A short write must be separated from the next sample at the receiver.
    partial_fd = output[1];
    assert(send(sockets[0], packet, length, 0) == (ssize_t)length);
    relay_game_frame(sockets[1], output[1], &interrupted);
    assert(interrupted);
    assert(send(sockets[0], packet, length, 0) == (ssize_t)length);
    relay_game_frame(sockets[1], output[1], &interrupted);
    char data[4096];
    ssize_t count = read(output[0], data, sizeof(data));
    assert(count == (ssize_t)(length / 2 + 1 + length));
    assert(data[length / 2] == '\n');
    assert(memcmp(data + length / 2 + 1, packet, length) == 0);
    assert(!interrupted);

    // Fill the output without a reader. One datagram is dropped immediately,
    // then a fresh line is delivered once the output becomes writable again.
    memset(data, 'x', sizeof(data));
    while (write(output[1], data, sizeof(data)) > 0) {}
    assert(errno == EAGAIN);
    assert(send(sockets[0], packet, length, 0) == (ssize_t)length);
    struct timespec before, after;
    assert(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
    relay_game_frame(sockets[1], output[1], &interrupted);
    assert(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
    assert((after.tv_sec - before.tv_sec) * 1000000000LL + after.tv_nsec - before.tv_nsec < 100000000);
    assert(interrupted);
    while (read(output[0], data, sizeof(data)) > 0) {}
    assert(errno == EAGAIN);
    assert(send(sockets[0], packet, length, 0) == (ssize_t)length);
    relay_game_frame(sockets[1], output[1], &interrupted);
    assert(read(output[0], data, sizeof(data)) == (ssize_t)length + 1);
    assert(data[0] == '\n' && memcmp(data + 1, packet, length) == 0);
    assert(!interrupted);
    for (unsigned i = 0; i < 2; ++i) { close(sockets[i]); close(output[i]); }
    puts("PASS: partial writes and backpressure preserve the next complete game-frame line");
    return 0;
}
