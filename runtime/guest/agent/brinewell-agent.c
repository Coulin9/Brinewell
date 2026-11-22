// Brinewell guest agent: progress/focus, virtual gamepad and hardware telemetry.
// Built statically for the SteamOS guest; no third-party dependencies.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int stop_signal_fd(void) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) return -1;
    return signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
}

static int open_port(const char *path, int flags) {
    int fd = open(path, flags | O_CLOEXEC);
    return fd;
}

static int write_all(int fd, const void *data, size_t len) {
    const char *p = data;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int game_frame_socket(void) {
    const char name[] = "brinewell.game-frame.v1";
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    memcpy(address.sun_path + 1, name, sizeof(name) - 1);
    socklen_t length = offsetof(struct sockaddr_un, sun_path) + 1 + sizeof(name) - 1;
    if (bind(fd, (struct sockaddr *)&address, length) < 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

// Only frame the existing protocol here; source selection and measurements
// belong to the host. Never relay arbitrary progress commands from this socket.
#define GAME_FRAME_MESSAGE_MAX 4096
#define GAME_FRAME_BATCH_MAX 128

static int game_frame_number(const char *data, size_t length, size_t *position,
                             char delimiter, uint64_t *value) {
    size_t start = *position;
    *value = 0;
    while (*position < length && data[*position] >= '0' && data[*position] <= '9') {
        unsigned digit = (unsigned)(data[(*position)++] - '0');
        if (*value > (UINT64_MAX - digit) / 10) return 0;
        *value = *value * 10 + digit;
    }
    return start != *position && *position < length && data[(*position)++] == delimiter;
}

static int valid_game_frame(const char *data, size_t length) {
    const char prefix[] = "game-frame ";
    if (length <= sizeof(prefix) + 2 || length > GAME_FRAME_MESSAGE_MAX || data[length - 1] != '\n'
        || memcmp(data, prefix, sizeof(prefix) - 1) != 0)
        return 0;
    size_t pos = sizeof(prefix) - 1;
    bool history = data[pos] == '2';
    if ((!history && data[pos] != '1') || data[pos + 1] != ' ') return 0;
    if (!history && length > 256) return 0;
    pos += 2;
    if (length - pos >= 8 && memcmp(data + pos, "neptune ", 8) == 0) pos += 8;
    else if (length - pos >= 7 && memcmp(data + pos, "vulkan ", 7) == 0) pos += 7;
    else return 0;
    uint64_t fields[9] = {0};
    unsigned field_count = history ? 9 : 7;
    for (unsigned field = 0; field < field_count; ++field) {
        char delimiter = !history && field == 6 ? '\n' : ' ';
        if (!game_frame_number(data, length, &pos, delimiter, &fields[field])) return 0;
    }
    if (!fields[0] || fields[0] > INT32_MAX || !fields[1] ||
        !fields[2] || fields[2] > 65535 || !fields[3] || fields[3] > 65535 ||
        fields[4] > 1 || (fields[4] && (!fields[5] || !fields[6]))) return 0;
    if (!history) return pos == length;
    if (!fields[7] || fields[8] > 1) return 0;
    if (!fields[4] && (fields[5] || fields[6] || fields[8])) return 0;
    if (!fields[8]) return length - pos == 2 && data[pos] == '-' && data[pos + 1] == '\n';
    if (!fields[4] || fields[5] > GAME_FRAME_BATCH_MAX) return 0;
    uint64_t total = 0;
    for (uint64_t i = 0; i < fields[5]; ++i) {
        uint64_t interval;
        if (!game_frame_number(data, length, &pos, i + 1 == fields[5] ? '\n' : ',', &interval)
            || !interval || total > UINT64_MAX - interval) return 0;
        total += interval;
    }
    return pos == length && total == fields[6];
}

static void relay_game_frame(int socket_fd, int progress_fd, int *interrupted) {
    char data[GAME_FRAME_MESSAGE_MAX];
    struct iovec buffer = { .iov_base = data, .iov_len = sizeof(data) };
    struct msghdr message = { .msg_iov = &buffer, .msg_iovlen = 1 };
    ssize_t length = recvmsg(socket_fd, &message, MSG_DONTWAIT);
    if (length <= 0 || (message.msg_flags & MSG_TRUNC)
        || !valid_game_frame(data, (size_t)length)) return;
    // Both descriptors are nonblocking. Drop samples under backpressure, and
    // separate any interrupted write before the next complete line.
    if (*interrupted) {
        if (write(progress_fd, "\n", 1) != 1) return;
        *interrupted = 0;
    }
    if (write(progress_fd, data, (size_t)length) != length) *interrupted = 1;
}

static int run_progress(void) {
    int stop = stop_signal_fd();
    if (stop < 0) return 1;
    int fd = open_port("/dev/virtio-ports/fx.progress", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { close(stop); return 0; }
    // Bind before announcing ready: games may start as soon as the host sees it.
    int frames = game_frame_socket();
    if (frames < 0) fprintf(stderr, "brinewell-agent: game frame relay unavailable: %s\n", strerror(errno));
    if (write_all(fd, "ready\n", 6) < 0) {
        if (frames >= 0) close(frames);
        close(fd);
        close(stop);
        return 0;
    }
    struct pollfd pending[] = {
        { .fd = stop, .events = POLLIN }, { .fd = frames, .events = POLLIN },
    };
    int interrupted = 0;
    for (;;) {
        int ready = poll(pending, 2, -1);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || pending[0].revents) break;
        if (pending[1].revents & POLLIN) relay_game_frame(frames, fd, &interrupted);
        if (pending[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "brinewell-agent: game frame relay socket closed\n");
            close(frames);
            frames = -1;
            pending[1].fd = -1;
        }
    }
    if (frames >= 0) close(frames);
    close(fd);
    close(stop);
    return 0;
}

struct pad_device {
    int fd;
};

static void pad_destroy(struct pad_device *dev) {
    if (dev->fd >= 0) {
        ioctl(dev->fd, UI_DEV_DESTROY);
        close(dev->fd);
        dev->fd = -1;
    }
}

static int emit_event(int fd, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    return write_all(fd, &ev, sizeof(ev));
}

static int pad_create(struct pad_device *dev, const char *line) {
    char *copy = strdup(line);
    if (!copy) return -1;
    char *save = NULL;
    char *tok = strtok_r(copy, " ", &save);
    if (!tok || strcmp(tok, "create") != 0) { free(copy); return -1; }

    char *bus_s = strtok_r(NULL, " ", &save);
    char *vendor_s = strtok_r(NULL, " ", &save);
    char *product_s = strtok_r(NULL, " ", &save);
    char *version_s = strtok_r(NULL, " ", &save);
    char *buttons_s = strtok_r(NULL, " ", &save);
    char *axes_s = strtok_r(NULL, " ", &save);
    char *name = save && *save ? save : (char *)"Brinewell gamepad";
    if (!bus_s || !vendor_s || !product_s || !version_s || !buttons_s || !axes_s) {
        free(copy);
        return -1;
    }

    pad_destroy(dev);
    int fd = open_port("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("brinewell-agent: /dev/uinput");
        free(copy);
        return -1;
    }

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) {
        perror("brinewell-agent: UI_SET_EVBIT");
        close(fd);
        free(copy);
        return -1;
    }

    char *bsave = NULL;
    char *b = strtok_r(buttons_s, ",", &bsave);
    while (b) {
        char *end = NULL;
        long code = strtol(b, &end, 10);
        if (end != b && code >= 0 && code <= KEY_MAX) {
            if (ioctl(fd, UI_SET_KEYBIT, (unsigned long)code) < 0) {
                perror("brinewell-agent: UI_SET_KEYBIT");
            }
        }
        b = strtok_r(NULL, ",", &bsave);
    }

    struct uinput_user_dev udev;
    memset(&udev, 0, sizeof(udev));
    snprintf(udev.name, sizeof(udev.name), "%s", name);
    udev.id.bustype = (uint16_t)strtoul(bus_s, NULL, 16);
    udev.id.vendor = (uint16_t)strtoul(vendor_s, NULL, 16);
    udev.id.product = (uint16_t)strtoul(product_s, NULL, 16);
    udev.id.version = (uint16_t)strtoul(version_s, NULL, 16);

    char *asave = NULL;
    char *a = strtok_r(axes_s, ",", &asave);
    while (a) {
        int code = -1, min = 0, max = 0, fuzz = 0, flat = 0;
        if (sscanf(a, "%d:%d:%d:%d:%d", &code, &min, &max, &fuzz, &flat) == 5 &&
            code >= 0 && code <= ABS_MAX) {
            if (ioctl(fd, UI_SET_ABSBIT, (unsigned long)code) < 0) {
                perror("brinewell-agent: UI_SET_ABSBIT");
            }
            udev.absmin[code] = (int16_t)min;
            udev.absmax[code] = (int16_t)max;
            udev.absfuzz[code] = (int16_t)fuzz;
            udev.absflat[code] = (int16_t)flat;
        }
        a = strtok_r(NULL, ",", &asave);
    }

    if (write_all(fd, &udev, sizeof(udev)) < 0 ||
        ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("brinewell-agent: UI_DEV_CREATE");
        close(fd);
        free(copy);
        return -1;
    }
    dev->fd = fd;
    free(copy);
    return 0;
}

static int pad_event(struct pad_device *dev, const char *line) {
    if (dev->fd < 0) return -1;
    char *copy = strdup(line);
    if (!copy) return -1;
    char *save = NULL;
    char *tok = strtok_r(copy, " ", &save);
    if (!tok || strcmp(tok, "ev") != 0) { free(copy); return -1; }
    while ((tok = strtok_r(NULL, " ", &save)) != NULL) {
        int type = -1, code = -1, value = 0;
        if (sscanf(tok, "%d:%d:%d", &type, &code, &value) != 3) continue;
        if (type < 0 || code < 0) continue;
        if (emit_event(dev->fd, (uint16_t)type, (uint16_t)code, value) < 0) {
            free(copy);
            return -1;
        }
    }
    free(copy);
    return emit_event(dev->fd, EV_SYN, SYN_REPORT, 0);
}

static int run_pad(void) {
    int stop = stop_signal_fd();
    if (stop < 0) return 1;
    struct pollfd pending[2] = {
        { .fd = stop, .events = POLLIN },
        { .fd = -1, .events = POLLIN },
    };
    int port = -1;
    for (int attempt = 0; attempt < 50; attempt++) {
        port = open_port("/dev/virtio-ports/fx.pad", O_RDWR | O_NONBLOCK);
        if (port >= 0) break;
        int result = poll(pending, 1, 100);
        if (result > 0) { close(stop); return 0; }
        if (result < 0 && errno != EINTR) { close(stop); return 1; }
    }
    if (port < 0) {
        fprintf(stderr, "brinewell-agent: fx.pad unavailable: %s\n", strerror(errno));
        close(stop);
        return 1;
    }
    if (write_all(port, "hello\n", 6) < 0) {
        close(port);
        close(stop);
        return 0;
    }

    struct pad_device dev = { .fd = -1 };
    char line[65537];
    size_t used = 0;
    int overflow = 0;
    int result = 0;
    pending[1].fd = port;
    for (;;) {
        int ready = poll(pending, 2, -1);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { result = 1; break; }
        if (pending[0].revents) break;
        char bytes[4096];
        ssize_t count = read(port, bytes, sizeof(bytes));
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (count <= 0) break;
        for (ssize_t i = 0; i < count; i++) {
            if (bytes[i] != '\n') {
                if (used < sizeof(line) - 1) line[used++] = bytes[i];
                else overflow = 1;
                continue;
            }
            if (!overflow) {
                if (used && line[used - 1] == '\r') used--;
                line[used] = '\0';
                if (strncmp(line, "create ", 7) == 0) pad_create(&dev, line);
                else if (strncmp(line, "ev ", 3) == 0) pad_event(&dev, line);
                else if (strcmp(line, "destroy") == 0) pad_destroy(&dev);
            }
            used = 0;
            overflow = 0;
        }
    }
    close(port);
    close(stop);
    pad_destroy(&dev);
    return result;
}

#define CPU_POWER_PATH "/run/power-monitor/power/cores_total"
#define GPU_POWER_PATH "/run/power-monitor/power/gfx"
#define HWMON_SNAPSHOT_PATH "/sys/devices/platform/brinewell_telemetry/snapshot"
#define OVERLAY_SNAPSHOT_PATH "/run/brinewell-telemetry/snapshot"
#define TELEMETRY_LINE_SIZE 4096
#define MAX_CPU_CORES 64

static void clear_telemetry(void) {
    unlink(CPU_POWER_PATH);
    unlink(GPU_POWER_PATH);
    unlink(OVERLAY_SNAPSHOT_PATH);
}

static void publish_file(const char *path, const char *contents, size_t length) {
    char temp[128];
    int len = snprintf(temp, sizeof(temp), "%s.XXXXXX", path);
    if (len < 0 || (size_t)len >= sizeof(temp)) return;
    int fd = mkstemp(temp);
    if (fd < 0) { unlink(path); return; }
    int ok = write_all(fd, contents, length) == 0 && fchmod(fd, 0644) == 0;
    if (close(fd) < 0) ok = 0;
    if (ok && rename(temp, path) == 0) return;
    unlink(temp);
    unlink(path);
}

static void publish_power(const char *path, long long microwatts) {
    if (microwatts < 0 || microwatts > 1000000000) {
        unlink(path);
        return;
    }
    char reading[32];
    int length = snprintf(reading, sizeof(reading), "%lld.%06lld\n", microwatts / 1000000, microwatts % 1000000);
    publish_file(path, reading, (size_t)length);
}

static void publish_hwmon(long long cpu_temp, long long gpu_temp, long long gpu_power, long long gpu_freq) {
    int fd = open(HWMON_SNAPSHOT_PATH, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    char snapshot[96];
    int len = snprintf(snapshot, sizeof(snapshot), "%lld %lld %lld %lld\n",
                       cpu_temp, gpu_temp, gpu_power, gpu_freq);
    if (len > 0 && (size_t)len < sizeof(snapshot)) (void)write(fd, snapshot, (size_t)len);
    close(fd);
}

static int valid_core_readings(const char *fields) {
    unsigned count;
    int length;
    if (sscanf(fields, "%2u%n", &count, &length) != 1 || count > MAX_CPU_CORES) return 0;
    fields += length;
    for (unsigned i = 0; i < count; ++i) {
        char tier;
        unsigned die, core;
        int mhz, busy, temp;
        if (sscanf(fields, " %c %2u %3u %5d %5d %6d%n", &tier, &die, &core,
                   &mhz, &busy, &temp, &length) != 6
            || !strchr("EPS_", tier) || die > 63 || core > 255
            || mhz < -1 || mhz > 20000 || busy < -1 || busy > 10000 || temp < -1 || temp > 150000)
            return 0;
        fields += length;
    }
    return *fields == '\0';
}

static int publish_telemetry(const char *line, unsigned long long request) {
    unsigned long long id;
    long long cpu_power, cpu_temp, gpu_temp, gpu_power, gpu_freq;
    long long gpu_busy, gpu_memory, cpu_busy, cpu_freq, ram_used, ram_total;
    int header_length;
    if (sscanf(line, "hardware %20llu %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %n", &id,
               &cpu_power, &cpu_temp, &gpu_temp, &gpu_power, &gpu_freq,
               &gpu_busy, &gpu_memory, &cpu_busy, &cpu_freq, &ram_used, &ram_total, &header_length) != 12
        || id != request)
        return 0;

    const char *cores = line + header_length;
    if (!*cores) cores = "0"; // Older host replies contain only aggregate readings.
    if (!valid_core_readings(cores)) return 0;

    char snapshot[TELEMETRY_LINE_SIZE];
    int length = snprintf(snapshot, sizeof(snapshot),
                          "2 %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %s\n",
                          gpu_busy, gpu_freq, gpu_temp, gpu_power, gpu_memory,
                          cpu_temp, cpu_power, cpu_busy, cpu_freq, ram_used, ram_total, cores);
    if (length < 0 || (size_t)length >= sizeof(snapshot)) return 0;
    publish_power(CPU_POWER_PATH, cpu_power);
    publish_power(GPU_POWER_PATH, gpu_power);
    publish_hwmon(cpu_temp, gpu_temp, gpu_power, gpu_freq);
    publish_file(OVERLAY_SNAPSHOT_PATH, snapshot, (size_t)length);
    return 1;
}

static int run_telemetry(void) {
    int stop = stop_signal_fd();
    int port = open_port("/dev/virtio-ports/fx.telemetry", O_RDWR | O_NONBLOCK);
    int timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    int status = 1;
    if (stop < 0 || port < 0 || timer < 0) goto cleanup;
    struct itimerspec interval = { .it_interval = { .tv_sec = 1 }, .it_value = { .tv_nsec = 1 } };
    if (timerfd_settime(timer, 0, &interval, NULL) < 0) goto cleanup;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) goto cleanup;
    unsigned long long request = (unsigned long long)now.tv_sec * 1000000000 + now.tv_nsec;
    struct pollfd pending[] = {
        { .fd = stop, .events = POLLIN }, { .fd = port, .events = POLLIN },
        { .fd = timer, .events = POLLIN },
    };
    char line[TELEMETRY_LINE_SIZE];
    size_t used = 0;
    int missed = 0;
    for (;;) {
        int ready = poll(pending, 3, -1);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) break;
        if (pending[0].revents) { status = 0; break; }
        if (pending[1].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (pending[1].revents & POLLIN) {
            char bytes[256];
            ssize_t count = read(port, bytes, sizeof(bytes));
            if (count == 0 || (count < 0 && errno != EINTR && errno != EAGAIN)) break;
            for (ssize_t i = 0; i < count; ++i) {
                if (bytes[i] != '\n') {
                    if (used < sizeof(line) - 1) line[used++] = bytes[i];
                    else used = sizeof(line);
                    continue;
                }
                if (used < sizeof(line)) {
                    line[used] = '\0';
                    if (publish_telemetry(line, request)) missed = 0;
                }
                used = 0;
            }
        }
        if (pending[2].revents & POLLIN) {
            uint64_t ticks;
            if (read(timer, &ticks, sizeof(ticks)) != sizeof(ticks)) break;
            if (ticks >= 3 || (missed += (int)ticks) >= 3) {
                clear_telemetry();
                missed = 3;
            }
            char message[48];
            int len = snprintf(message, sizeof(message), "hardware %llu\n", ++request);
            ssize_t sent = write(port, message, (size_t)len);
            if (sent < 0 && errno != EINTR && errno != EAGAIN) break;
        }
    }
cleanup:
    clear_telemetry();
    if (timer >= 0) close(timer);
    if (port >= 0) close(port);
    if (stop >= 0) close(stop);
    return status;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "pad") == 0) return run_pad();
    if (argc == 2 && strcmp(argv[1], "telemetry") == 0) return run_telemetry();
    return run_progress();
}
