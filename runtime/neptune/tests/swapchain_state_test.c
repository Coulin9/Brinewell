#include <stdio.h>
#include <string.h>

#include "swapchain_test_support.h"

int
main(int argc, char **argv)
{
   const char *group = "all";

   if (argc == 3 && strcmp(argv[1], "--group") == 0) {
      group = argv[2];
   } else if (argc != 1) {
      fprintf(stderr, "Usage: %s [--group all|common|d3d11|d3d12]\n", argv[0]);
      return 2;
   }

   if (strcmp(group, "all") != 0 && strcmp(group, "common") != 0 &&
       strcmp(group, "d3d11") != 0 && strcmp(group, "d3d12") != 0) {
      fprintf(stderr, "Usage: %s [--group all|common|d3d11|d3d12]\n", argv[0]);
      return 2;
   }

   if (strcmp(group, "all") == 0 || strcmp(group, "common") == 0) {
      puts("Starting common group");
      fflush(stdout);
      test_common_group();
   }
   if (strcmp(group, "all") == 0 || strcmp(group, "d3d11") == 0) {
      puts("Starting d3d11 group");
      fflush(stdout);
      test_d3d11_group();
   }
   if (strcmp(group, "all") == 0 || strcmp(group, "d3d12") == 0) {
      puts("Starting d3d12 group");
      fflush(stdout);
      test_d3d12_group();
   }

   puts("NPT_SWAPCHAIN_STATE_TEST: PASS");
   return 0;
}
