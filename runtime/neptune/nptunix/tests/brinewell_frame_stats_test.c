#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static unsigned writes, closes, sockets, connects;
static bool real_transport;
static ssize_t write_result = -2;
static int write_error;
static char output[16][4096];

static int test_socket(int domain, int type, int protocol)
{
   if (real_transport)
      return socket(domain, type, protocol);
   assert(domain == AF_UNIX && type == (SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC));
   assert(protocol == 0);
   sockets++;
   return 99;
}

static int test_connect(int fd, const struct sockaddr *address, socklen_t length)
{
   if (real_transport)
      return connect(fd, address, length);
   const struct sockaddr_un *un = (const struct sockaddr_un *)address;
   const char *name = "brinewell.game-frame.v1";
   assert(fd == 99 && un->sun_family == AF_UNIX && un->sun_path[0] == 0);
   assert(length == offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name));
   assert(!memcmp(un->sun_path + 1, name, strlen(name)));
   connects++;
   errno = ECONNREFUSED;
   return -1;
}

static ssize_t test_send(int fd, const void *data, size_t size, int flags)
{
   if (real_transport)
      return send(fd, data, size, flags);
   assert(fd == 99 && writes < 16 && size < sizeof(output[0]) && flags == MSG_NOSIGNAL);
   memcpy(output[writes], data, size);
   output[writes++][size] = 0;
   errno = write_error;
   return write_result == -2 ? (ssize_t)size : write_result;
}

static int test_close(int fd)
{
   if (real_transport)
      return close(fd);
   assert(fd == 99);
   closes++;
   return 0;
}

#define socket test_socket
#define connect test_connect
#define send test_send
#define close test_close
#include "../brinewell_frame_stats.h"
#undef socket
#undef connect
#undef send
#undef close

static struct brinewell_frame_stats stream(const char *backend, uint32_t pid)
{
   return (struct brinewell_frame_stats) {
      .fd = 99, .backend = backend, .pid = pid, .width = 1280, .height = 800,
   };
}

static void reset(void)
{
   writes = closes = sockets = connects = 0;
   write_result = -2;
   write_error = 0;
   memset(output, 0, sizeof(output));
}

static void test_aggregation(void)
{
   reset();
   struct brinewell_frame_stats a = stream("neptune", 42);
   brinewell_frame_stats_record(&a, 1000000000, true);
   brinewell_frame_stats_record(&a, 1100000000, true);
   assert(writes == 0);
   brinewell_frame_stats_record(&a, 1300000000, true);
   assert(writes == 1);
   assert(!strcmp(output[0], "game-frame 2 neptune 42 1000000000 1280 800 1 2 300000000 1 1 100000000,200000000\n"));
   brinewell_frame_stats_record(&a, 1450000000, true);
   brinewell_frame_stats_record(&a, 1600000000, true);
   assert(writes == 2);
   assert(!strcmp(output[1], "game-frame 2 neptune 42 1000000000 1280 800 1 2 300000000 2 1 150000000,150000000\n"));
   brinewell_frame_stats_record(&a, 1900000000, false);
   assert(!strcmp(output[2], "game-frame 2 neptune 42 1000000000 1280 800 0 0 0 3 0 -\n"));
   brinewell_frame_stats_finish(&a);
   assert(writes == 4 && closes == 1 && a.fd == -1);
}

static void test_separate_streams(void)
{
   reset();
   struct brinewell_frame_stats a = stream("vulkan", 71);
   struct brinewell_frame_stats b = stream("vulkan", 71);
   brinewell_frame_stats_record(&a, 1000000000, true);
   brinewell_frame_stats_record(&b, 1100000000, true);
   brinewell_frame_stats_record(&a, 1250000000, true);
   brinewell_frame_stats_record(&b, 1200000000, true);
   brinewell_frame_stats_record(&b, 1400000000, true);
   assert(writes == 2);
   assert(!strcmp(output[0], "game-frame 2 vulkan 71 1000000000 1280 800 1 1 250000000 1 1 250000000\n"));
   assert(!strcmp(output[1], "game-frame 2 vulkan 71 1100000000 1280 800 1 2 300000000 1 1 100000000,200000000\n"));
}

static void test_unavailable_and_failed_writes(void)
{
   reset();
   struct brinewell_frame_stats a;
   brinewell_frame_stats_init(&a, "vulkan", 1280, 800);
   for (unsigned i = 0; i < 1000; i++)
      brinewell_frame_stats_record(&a, 1000000000ull + i * 1000000ull, true);
   brinewell_frame_stats_finish(&a);
   assert(sockets == 1 && connects == 1 && writes == 0 && closes == 1 && a.fd == -1);

   reset();
   a = stream("vulkan", 42);
   brinewell_frame_stats_record(&a, 1000000000, true);
   write_result = -1;
   write_error = EAGAIN;
   brinewell_frame_stats_record(&a, 1250000000, true);
   assert(writes == 1 && a.fd == 99 && closes == 0);
   write_result = -2;
   brinewell_frame_stats_record(&a, 1500000000, true);
   assert(!strcmp(output[1], "game-frame 2 vulkan 42 1000000000 1280 800 1 1 250000000 2 1 250000000\n"));
   write_result = -1;
   write_error = EIO;
   brinewell_frame_stats_record(&a, 1750000000, true);
   assert(a.fd == -1 && closes == 1);
   brinewell_frame_stats_record(&a, 3000000000, true);
   brinewell_frame_stats_finish(&a);
   assert(writes == 3 && closes == 1);

   reset();
   a = stream("neptune", 42);
   brinewell_frame_stats_record(&a, 1000000000, true);
   write_result = 7;
   brinewell_frame_stats_record(&a, 1250000000, true);
   brinewell_frame_stats_record(&a, 1500000000, true);
   assert(writes == 1 && closes == 1 && a.fd == -1);
}

static void test_invalid_timestamps(void)
{
   reset();
   struct brinewell_frame_stats a = stream("neptune", 42);
   brinewell_frame_stats_record(&a, 0, true);
   brinewell_frame_stats_record(&a, 1000000000, true);
   brinewell_frame_stats_record(&a, 999999999, true);
   brinewell_frame_stats_record(&a, 1000000000, true);
   brinewell_frame_stats_record(&a, 1250000000, true);
   assert(writes == 1);
   assert(!strcmp(output[0], "game-frame 2 neptune 42 1000000000 1280 800 1 1 250000000 1 1 250000000\n"));
}

static void test_history_bounds_and_focus_reset(void)
{
   reset();
   struct brinewell_frame_stats a = stream("neptune", 42);
   brinewell_frame_stats_record(&a, 1000000000, true);
   for (unsigned i = 1; i < 128; i++)
      brinewell_frame_stats_record(&a, 1000000000 + i * 1000000, true);
   assert(writes == 0);
   brinewell_frame_stats_record(&a, 1250000000, true);
   assert(writes == 1 && strlen(output[0]) < 4096);
   assert(strstr(output[0], " 128 250000000 1 1 "));
   unsigned commas = 0;
   for (char *p = output[0]; *p; p++) commas += *p == ',';
   assert(commas == 127 && strstr(output[0], ",123000000\n"));

   for (unsigned i = 1; i <= 250; i++)
      brinewell_frame_stats_record(&a, 1250000000 + i * 1000000, true);
   assert(writes == 2);
   assert(strstr(output[1], " 250 250000000 2 0 -\n"));

   reset();
   a = stream("neptune", 42);
   brinewell_frame_stats_record(&a, 1000000000, true);
   brinewell_frame_stats_record(&a, 1100000000, true);
   brinewell_frame_stats_record(&a, 1200000000, false);
   assert(writes == 1 && strstr(output[0], " 0 0 0 1 0 -\n"));
   brinewell_frame_stats_record(&a, 1300000000, false);
   assert(writes == 1);
   brinewell_frame_stats_record(&a, 1400000000, true);
   brinewell_frame_stats_record(&a, 1650000000, true);
   assert(writes == 2);
   assert(strstr(output[1], " 1 1 250000000 2 1 250000000\n"));
}

static void test_datagram_receiver(void)
{
   real_transport = true;
   int receiver = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
   assert(receiver >= 0);
   struct sockaddr_un address = {.sun_family = AF_UNIX};
   const char *name = "brinewell.game-frame.v1";
   memcpy(address.sun_path + 1, name, strlen(name));
   assert(!bind(receiver, (struct sockaddr *)&address,
                offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name)));
   struct brinewell_frame_stats a, b;
   brinewell_frame_stats_init(&a, "vulkan", 1280, 800);
   brinewell_frame_stats_init(&b, "neptune", 1920, 1080);
   assert(a.fd >= 0 && b.fd >= 0 && a.fd != b.fd);
   brinewell_frame_stats_record(&a, 1000000000, true);
   brinewell_frame_stats_record(&b, 2000000000, true);
   brinewell_frame_stats_record(&a, 1300000000, true);
   brinewell_frame_stats_record(&b, 2500000000, true);
   char line[256], expected[256];
   ssize_t count = recv(receiver, line, sizeof(line) - 1, MSG_DONTWAIT);
   assert(count > 0);
   line[count] = 0;
   snprintf(expected, sizeof(expected),
      "game-frame 2 vulkan %u 1000000000 1280 800 1 1 300000000 1 1 300000000\n", (unsigned)getpid());
   assert(!strcmp(line, expected));
   count = recv(receiver, line, sizeof(line) - 1, MSG_DONTWAIT);
   assert(count > 0);
   line[count] = 0;
   snprintf(expected, sizeof(expected),
      "game-frame 2 neptune %u 2000000000 1920 1080 1 1 500000000 1 1 500000000\n", (unsigned)getpid());
   assert(!strcmp(line, expected));
   brinewell_frame_stats_finish(&a);
   brinewell_frame_stats_finish(&b);
   close(receiver);
   real_transport = false;
}

int main(void)
{
   test_aggregation();
   test_separate_streams();
   test_unavailable_and_failed_writes();
   test_invalid_timestamps();
   test_history_bounds_and_focus_reset();
   test_datagram_receiver();
   puts("brinewell frame stats: passed");
   return 0;
}
