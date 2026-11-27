#ifndef BRINEWELL_FRAME_STATS_H
#define BRINEWELL_FRAME_STATS_H

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define BRINEWELL_FRAME_BATCH_MAX 128
#define BRINEWELL_FRAME_MESSAGE_MAX 4096

struct brinewell_frame_stats {
   int fd;
   uint32_t pid, width, height;
   const char *backend;
   uint64_t stream, last_present, last_report, intervals, duration;
   uint64_t sequence;
   uint64_t frame_intervals[BRINEWELL_FRAME_BATCH_MAX];
   bool focused;
};

static inline void
brinewell_frame_stats_init(struct brinewell_frame_stats *stats,
                          const char *backend, uint32_t width, uint32_t height)
{
   memset(stats, 0, sizeof(*stats));
   stats->fd = -1;
   stats->backend = backend;
   stats->width = width;
   stats->height = height;
   int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
   if (fd < 0)
      return;
   struct sockaddr_un address = {.sun_family = AF_UNIX};
   const char *name = "brinewell.game-frame.v1";
   memcpy(address.sun_path + 1, name, strlen(name));
   socklen_t length = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name);
   if (connect(fd, (const struct sockaddr *)&address, length)) {
      close(fd);
      return;
   }

   char process[64] = {0};
   FILE *comm = fopen("/proc/self/comm", "r");
   if (comm) {
      if (fgets(process, sizeof(process), comm))
         process[strcspn(process, "\n")] = 0;
      fclose(comm);
   }
   const char *excluded[] = {
      "gamescope", "kwin_wayland", "kwin_x11", "plasmashell",
      "steam", "steamwebhelper", "mangoapp", "Xwayland",
   };
   for (unsigned i = 0; i < sizeof(excluded) / sizeof(excluded[0]); i++) {
      if (!strcmp(process, excluded[i])) {
         close(fd);
         return;
      }
   }
   stats->fd = fd;
   stats->pid = (uint32_t)getpid();
}

static inline uint64_t
brinewell_frame_stats_now(void)
{
   struct timespec now;
   if (clock_gettime(CLOCK_MONOTONIC, &now))
      return 0;
   return (uint64_t)now.tv_sec * 1000000000ull + now.tv_nsec;
}

static inline void
brinewell_frame_stats_send(struct brinewell_frame_stats *stats, bool focused)
{
   if (stats->fd < 0 || !stats->stream)
      return;
   char line[BRINEWELL_FRAME_MESSAGE_MAX];
   bool complete = focused && stats->intervals > 0 &&
                   stats->intervals <= BRINEWELL_FRAME_BATCH_MAX;
   int length = snprintf(line, sizeof(line),
      "game-frame 2 %s %u %" PRIu64 " %u %u %u %" PRIu64 " %" PRIu64
      " %" PRIu64 " %u ",
      stats->backend, stats->pid,
      stats->stream, stats->width, stats->height, focused ? 1 : 0,
      focused ? stats->intervals : 0, focused ? stats->duration : 0,
      ++stats->sequence, complete ? 1 : 0);
   if (length <= 0 || (size_t)length >= sizeof(line))
      return;
   if (complete) {
      for (uint64_t i = 0; i < stats->intervals; i++) {
         int appended = snprintf(line + length, sizeof(line) - length,
                                  "%s%" PRIu64, i ? "," : "",
                                  stats->frame_intervals[i]);
         if (appended <= 0 || (size_t)appended >= sizeof(line) - length)
            return;
         length += appended;
      }
   } else {
      line[length++] = '-';
   }
   if ((size_t)length + 1 >= sizeof(line))
      return;
   line[length++] = '\n';
   ssize_t written = send(stats->fd, line, (size_t)length, MSG_NOSIGNAL);
   if ((written >= 0 && written != length) ||
       (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
      close(stats->fd);
      stats->fd = -1;
   }
}

static inline void
brinewell_frame_stats_record(struct brinewell_frame_stats *stats,
                            uint64_t present_ns, bool focused)
{
   if (stats->fd < 0 || !present_ns)
      return;
   if (!stats->stream) {
      stats->stream = stats->last_present = stats->last_report = present_ns;
      stats->focused = focused;
      return;
   }
   if (present_ns <= stats->last_present)
      return;
   if (focused != stats->focused) {
      stats->focused = focused;
      stats->last_present = stats->last_report = present_ns;
      stats->intervals = stats->duration = 0;
      if (!focused)
         brinewell_frame_stats_send(stats, false);
      return;
   }
   if (!focused) {
      stats->last_present = stats->last_report = present_ns;
      return;
   }
   uint64_t interval = present_ns - stats->last_present;
   if (stats->intervals < BRINEWELL_FRAME_BATCH_MAX)
      stats->frame_intervals[stats->intervals] = interval;
   stats->intervals++;
   stats->duration += interval;
   stats->last_present = present_ns;
   if (present_ns - stats->last_report < 250000000ull)
      return;
   brinewell_frame_stats_send(stats, focused);
   stats->intervals = stats->duration = 0;
   stats->last_report = present_ns;
}

static inline void
brinewell_frame_stats_finish(struct brinewell_frame_stats *stats)
{
   if (stats->fd >= 0) {
      brinewell_frame_stats_send(stats, false);
      if (stats->fd >= 0)
         close(stats->fd);
      stats->fd = -1;
   }
}

#endif
