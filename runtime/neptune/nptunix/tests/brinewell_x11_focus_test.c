#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>

static bool ready = true;
static xcb_window_t input_focus, queried_window;
static unsigned kind, sequence, focus_requests, discarded;
static unsigned property_requests, display_reads, disconnects;
static xcb_atom_t queried_atom;
static xcb_window_t global_window = 100;
static char global_display[4] = ":1";
static bool global_valid = true, display_changes;
static xcb_connection_t *pending_flush;

static xcb_get_input_focus_cookie_t test_get_input_focus(xcb_connection_t *conn)
{
   assert(conn == (xcb_connection_t *)(uintptr_t)1 && !pending_flush);
   pending_flush = conn;
   kind = 1;
   focus_requests++;
   return (xcb_get_input_focus_cookie_t) {++sequence};
}

static xcb_query_tree_cookie_t test_query_tree(xcb_connection_t *conn, xcb_window_t window)
{
   assert(conn == (xcb_connection_t *)(uintptr_t)1 && !pending_flush);
   pending_flush = conn;
   kind = 2;
   queried_window = window;
   return (xcb_query_tree_cookie_t) {++sequence};
}

static xcb_get_property_cookie_t
test_get_property(xcb_connection_t *conn, uint8_t remove, xcb_window_t window,
                  xcb_atom_t atom, xcb_atom_t type, uint32_t offset, uint32_t length)
{
   assert(conn == (xcb_connection_t *)(uintptr_t)2 && !pending_flush);
   pending_flush = conn;
   assert(!remove && window == 20 && type == XCB_ATOM_CARDINAL && !offset && length == 1);
   queried_atom = atom;
   kind = 3;
   property_requests++;
   return (xcb_get_property_cookie_t) {++sequence};
}

static int test_flush(xcb_connection_t *conn)
{
   assert(conn == pending_flush);
   pending_flush = NULL;
   return 1;
}

static void test_disconnect(xcb_connection_t *conn)
{
   assert(conn == (xcb_connection_t *)(uintptr_t)2);
   disconnects++;
}

static int test_connection_has_error(xcb_connection_t *conn)
{
   (void)conn;
   return 0;
}

static void test_discard_reply(xcb_connection_t *conn, unsigned int seq)
{
   (void)conn;
   assert(seq == sequence);
   discarded++;
}

static int test_poll_for_reply(xcb_connection_t *conn, unsigned int seq,
                               void **reply, xcb_generic_error_t **error)
{
   assert(!pending_flush);
   assert(seq == sequence);
   *reply = NULL;
   *error = NULL;
   if (!ready)
      return 0;
   if (kind == 1) {
      xcb_get_input_focus_reply_t *focus = calloc(1, sizeof(*focus));
      focus->focus = input_focus;
      *reply = focus;
   } else if (kind == 2) {
      xcb_query_tree_reply_t *tree = calloc(1, sizeof(*tree));
      tree->root = 10;
      tree->parent = queried_window == 100 || queried_window == 120 ? 90 :
                     queried_window == 110 ? 100 : queried_window == 10 ? 0 : 10;
      *reply = tree;
   } else {
      assert(conn == (xcb_connection_t *)(uintptr_t)2);
      xcb_get_property_reply_t *property = calloc(1, sizeof(*property) + 4);
      property->type = global_valid ? XCB_ATOM_CARDINAL : XCB_NONE;
      property->format = global_valid ? 32 : 0;
      property->value_len = global_valid ? 1 : 0;
      if (queried_atom == 1000) {
         const char *display = display_changes && display_reads ? ":0\0" : global_display;
         memcpy(property + 1, display, 4);
         property->bytes_after = 8;
         display_reads++;
      } else {
         assert(queried_atom == 2000);
         memcpy(property + 1, &global_window, 4);
      }
      *reply = property;
   }
   return 1;
}

#define xcb_get_input_focus test_get_input_focus
#define xcb_query_tree test_query_tree
#define xcb_get_property test_get_property
#define xcb_flush test_flush
#define xcb_disconnect test_disconnect
#define xcb_connection_has_error test_connection_has_error
#define xcb_discard_reply test_discard_reply
#define xcb_poll_for_reply test_poll_for_reply
#include "../brinewell_x11_focus.h"

static void check_focus(xcb_window_t window, xcb_window_t target, bool expected)
{
   struct brinewell_x11_focus focus = {.window = window};
   xcb_connection_t *conn = (xcb_connection_t *)(uintptr_t)1;
   input_focus = target;
   for (unsigned i = 0; i < 8; i++)
      brinewell_x11_focus_update(conn, &focus, 1000000000);
   assert(focus.stage == 0 && focus.focused == expected);
   unsigned requests = focus_requests;
   for (unsigned i = 1; i < 250; i++)
      brinewell_x11_focus_update(conn, &focus, 1000000000 + i * 1000000);
   assert(requests == focus_requests);
   brinewell_x11_focus_finish(conn, &focus);
}

static void check_gamescope(const char *display, xcb_window_t window, bool expected)
{
   struct brinewell_x11_focus focus = {
      .window = 100, .gamescope = true,
      .control = (xcb_connection_t *)(uintptr_t)2, .control_root = 20,
      .display_atom = 1000, .window_atom = 2000, .display_name = ":1",
   };
   xcb_connection_t *conn = (xcb_connection_t *)(uintptr_t)1;
   input_focus = 100;
   memcpy(global_display, display, 4);
   global_window = window;
   display_reads = 0;
   unsigned requests = focus_requests;
   for (unsigned i = 0; i < 12; i++)
      brinewell_x11_focus_update(conn, &focus, 1000000000);
   assert(focus.stage == 0 && focus.focused == expected && focus_requests == requests);
   unsigned properties = property_requests;
   for (unsigned i = 1; i < 250; i++)
      brinewell_x11_focus_update(conn, &focus, 1000000000 + i * 1000000);
   assert(properties == property_requests);
   focus.owns_control = true;
   brinewell_x11_focus_finish(conn, &focus);
   assert(!focus.control && !focus.owns_control);
}

int main(void)
{
   check_focus(100, 100, true);
   check_focus(100, 110, true);
   check_focus(100, 90, true);
   check_focus(100, 120, false);
   check_focus(100, 200, false);
   check_focus(100, 10, false);
   check_focus(10, 10, false);
   check_focus(100, XCB_INPUT_FOCUS_POINTER_ROOT, false);
   check_focus(100, XCB_NONE, false);

   check_gamescope(":1\0", 100, true);
   check_gamescope(":1\0", 110, true);
   check_gamescope(":1\0", 90, true);
   check_gamescope(":0\0", 100, false);
   check_gamescope(":1\0", 200, false);
   check_gamescope(":1\0", 10, false);
   check_gamescope(":1\0", 0, false);
   check_gamescope(":100", 100, false);
   check_gamescope(":x\0", 100, false);
   display_changes = true;
   check_gamescope(":1\0", 100, false);
   display_changes = false;
   global_valid = false;
   check_gamescope(":1\0", 100, false);
   global_valid = true;
   assert(disconnects == 11);

   struct brinewell_x11_focus unavailable = {.window = 100, .gamescope = true};
   assert(!brinewell_x11_focus_update((xcb_connection_t *)(uintptr_t)1,
                                     &unavailable, 1000000000));

   struct brinewell_x11_focus focus = {.window = 100, .focused = true};
   xcb_connection_t *conn = (xcb_connection_t *)(uintptr_t)1;
   ready = false;
   brinewell_x11_focus_update(conn, &focus, 1000000000);
   assert(focus.stage == 1);
   brinewell_x11_focus_update(conn, &focus, 2000000000);
   assert(!focus.focused && focus.stage == 0 && discarded == 1);
   puts("brinewell X11 focus: passed");
   return 0;
}
