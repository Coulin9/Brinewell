#ifndef BRINEWELL_X11_FOCUS_H
#define BRINEWELL_X11_FOCUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>

struct brinewell_x11_focus {
   xcb_window_t window, focus, cursor;
   unsigned sequence, stage, depth;
   uint64_t query_time;
   bool focused;
   bool gamescope, owns_control;
   xcb_connection_t *control;
   xcb_window_t control_root;
   xcb_atom_t display_atom, window_atom;
   char display_name[4];
};

static inline bool
brinewell_x11_cardinal(const xcb_get_property_reply_t *reply, uint32_t *value)
{
   if (!reply || reply->type != XCB_ATOM_CARDINAL || reply->format != 32 ||
       reply->value_len != 1 || reply->bytes_after)
      return false;
   memcpy(value, reply + 1, sizeof(*value));
   return true;
}

static inline xcb_atom_t
brinewell_x11_atom(xcb_connection_t *conn, const char *name)
{
   xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(
      conn, xcb_intern_atom(conn, true, strlen(name), name), NULL);
   xcb_atom_t atom = reply ? reply->atom : XCB_NONE;
   free(reply);
   return atom;
}

static inline bool
brinewell_x11_identity(xcb_connection_t *conn, xcb_window_t root,
                      uint32_t *server, uint32_t *pid)
{
   xcb_atom_t server_atom = brinewell_x11_atom(conn, "GAMESCOPE_XWAYLAND_SERVER_ID");
   xcb_atom_t pid_atom = brinewell_x11_atom(conn, "GAMESCOPE_PID");
   if (!server_atom || !pid_atom)
      return false;
   xcb_get_property_reply_t *server_reply = xcb_get_property_reply(conn,
      xcb_get_property(conn, false, root, server_atom, XCB_ATOM_CARDINAL, 0, 1), NULL);
   xcb_get_property_reply_t *pid_reply = xcb_get_property_reply(conn,
      xcb_get_property(conn, false, root, pid_atom, XCB_ATOM_CARDINAL, 0, 1), NULL);
   bool valid = brinewell_x11_cardinal(server_reply, server) &&
                brinewell_x11_cardinal(pid_reply, pid) && *pid;
   free(server_reply);
   free(pid_reply);
   return valid;
}

static inline void
brinewell_x11_focus_init(xcb_connection_t *conn, struct brinewell_x11_focus *focus,
                        xcb_window_t window)
{
   memset(focus, 0, sizeof(*focus));
   focus->window = window;
   xcb_screen_iterator_t screen = xcb_setup_roots_iterator(xcb_get_setup(conn));
   uint32_t server, pid;
   if (!screen.rem || !brinewell_x11_identity(conn, screen.data->root, &server, &pid))
      return;
   focus->gamescope = true;

   struct sockaddr_un peer = {0};
   socklen_t length = sizeof(peer);
   if (getpeername(xcb_get_file_descriptor(conn), (struct sockaddr *)&peer, &length) ||
       peer.sun_family != AF_UNIX)
      return;
   peer.sun_path[sizeof(peer.sun_path) - 1] = 0;
   const char *path = peer.sun_path + (peer.sun_path[0] == 0);
   unsigned display;
   char trailing;
   if (sscanf(path, "/tmp/.X11-unix/X%u%c", &display, &trailing) != 1 || display > 99)
      return;
   snprintf(focus->display_name, sizeof(focus->display_name), ":%u", display);

   xcb_connection_t *control = server == 0 ? conn : xcb_connect(":0", NULL);
   bool valid = control && !xcb_connection_has_error(control);
   xcb_window_t root = screen.data->root;
   if (valid && server != 0) {
      screen = xcb_setup_roots_iterator(xcb_get_setup(control));
      uint32_t control_server, control_pid;
      valid = screen.rem && brinewell_x11_identity(control, screen.data->root,
                                                  &control_server, &control_pid) &&
              control_server == 0 && control_pid == pid;
      if (valid)
         root = screen.data->root;
   }
   if (valid) {
      focus->display_atom = brinewell_x11_atom(control, "GAMESCOPE_FOCUS_DISPLAY");
      focus->window_atom = brinewell_x11_atom(control, "GAMESCOPE_FOCUSED_WINDOW");
      valid = focus->display_atom && focus->window_atom;
   }
   if (!valid) {
      if (control && control != conn)
         xcb_disconnect(control);
      return;
   }
   focus->control = control;
   focus->control_root = root;
   focus->owns_control = control != conn;
}

static inline bool
brinewell_x11_display_matches(const xcb_get_property_reply_t *reply, const char *name)
{
   if (!reply || reply->type != XCB_ATOM_CARDINAL || reply->format != 32 ||
       reply->value_len < 1)
      return false;
   const char *value = (const char *)(reply + 1);
   const char *end = memchr(value, 0, 4);
   if (!end || value[0] != ':' || end < value + 2 || end > value + 3)
      return false;
   for (const char *digit = value + 1; digit < end; digit++)
      if (*digit < '0' || *digit > '9')
         return false;
   return !strcmp(value, name);
}

static inline void
brinewell_x11_focus_cancel(xcb_connection_t *conn, struct brinewell_x11_focus *focus)
{
   if (focus->sequence)
      xcb_discard_reply(focus->stage >= 4 ? focus->control : conn, focus->sequence);
   focus->sequence = focus->stage = 0;
   focus->focused = false;
}

static inline void
brinewell_x11_focus_finish(xcb_connection_t *conn, struct brinewell_x11_focus *focus)
{
   brinewell_x11_focus_cancel(conn, focus);
   if (focus->owns_control)
      xcb_disconnect(focus->control);
   focus->control = NULL;
   focus->owns_control = false;
}

static inline void
brinewell_x11_focus_property(struct brinewell_x11_focus *focus, xcb_atom_t atom,
                            unsigned stage)
{
   focus->stage = stage;
   focus->sequence = xcb_get_property(focus->control, false, focus->control_root,
                                      atom, XCB_ATOM_CARDINAL, 0, 1).sequence;
   xcb_flush(focus->control);
}

static inline bool
brinewell_x11_focus_update(xcb_connection_t *conn,
                          struct brinewell_x11_focus *focus, uint64_t now)
{
   if (!now || !conn || !focus->window || xcb_connection_has_error(conn)) {
      focus->focused = false;
      return false;
   }
   if (focus->gamescope &&
       (!focus->control || xcb_connection_has_error(focus->control))) {
      focus->focused = false;
      return false;
   }
   if (focus->stage && now - focus->query_time >= 1000000000ull) {
      brinewell_x11_focus_cancel(conn, focus);
      focus->query_time = now;
   }
   if (!focus->stage && now - focus->query_time >= 250000000ull) {
      if (focus->gamescope) {
         brinewell_x11_focus_property(focus, focus->display_atom, 4);
      } else {
         focus->sequence = xcb_get_input_focus(conn).sequence;
         xcb_flush(conn);
         focus->stage = 1;
      }
      focus->query_time = now;
   }

   for (unsigned i = 0; focus->stage && i < 4; i++) {
      void *reply = NULL;
      xcb_generic_error_t *error = NULL;
      xcb_connection_t *source = focus->stage >= 4 ? focus->control : conn;
      if (!xcb_poll_for_reply(source, focus->sequence, &reply, &error))
         break;
      focus->sequence = 0;
      if (error || !reply) {
         free(error);
         free(reply);
         focus->stage = 0;
         focus->focused = false;
         break;
      }
      if (focus->stage >= 4) {
         bool valid;
         if (focus->stage == 5)
            valid = brinewell_x11_cardinal(reply, &focus->focus) &&
                    focus->focus > XCB_INPUT_FOCUS_POINTER_ROOT;
         else
            valid = brinewell_x11_display_matches(reply, focus->display_name);
         free(reply);
         if (!valid) {
            focus->stage = 0;
            focus->focused = false;
            break;
         }
         if (focus->stage == 4) {
            brinewell_x11_focus_property(focus, focus->window_atom, 5);
            continue;
         }
         if (focus->stage == 5) {
            brinewell_x11_focus_property(focus, focus->display_atom, 6);
            continue;
         }
         focus->cursor = focus->window;
         focus->stage = 2;
         focus->depth = 0;
      } else if (focus->stage == 1) {
         focus->focus = ((xcb_get_input_focus_reply_t *)reply)->focus;
         free(reply);
         if (focus->focus <= XCB_INPUT_FOCUS_POINTER_ROOT) {
            focus->focused = false;
            focus->stage = 0;
            break;
         }
         focus->cursor = focus->window;
         focus->stage = 2;
         focus->depth = 0;
      } else {
         xcb_query_tree_reply_t *tree = reply;
         bool root = focus->cursor == tree->root;
         bool match = !root && (focus->stage == 2
                               ? focus->cursor == focus->focus
                               : focus->cursor == focus->window);
         xcb_window_t parent = tree->parent;
         bool end = root || !parent || parent == tree->root || ++focus->depth >= 16;
         free(reply);
         if (match) {
            focus->focused = true;
            focus->stage = 0;
            break;
         }
         if (end && focus->stage == 2 && !root) {
            focus->cursor = focus->focus;
            focus->stage = 3;
            focus->depth = 0;
         } else if (end) {
            focus->focused = false;
            focus->stage = 0;
            break;
         } else {
            focus->cursor = parent;
         }
      }
      focus->sequence = xcb_query_tree(conn, focus->cursor).sequence;
      xcb_flush(conn);
   }
   return focus->focused;
}

#endif
