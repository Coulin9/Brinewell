#ifndef NPT_UNIX_WAYLAND_HDR_H
#define NPT_UNIX_WAYLAND_HDR_H

struct wl_proxy;
struct wl_interface;
struct wl_message {
   const char *name, *signature;
   const struct wl_interface **types;
};
struct wl_interface {
   const char *name;
   int version, method_count;
   const struct wl_message *methods;
   int event_count;
   const struct wl_message *events;
};

static const struct wl_interface npt_dmabuf_iface, npt_params_iface;
static const struct wl_interface npt_factory_iface, npt_swapchain_iface;
static const struct wl_interface *npt_params_new_types[] = { &npt_params_iface };
static const struct wl_interface *npt_buffer_new_types[] = { NULL, NULL, NULL, NULL, NULL };
static const struct wl_interface *npt_factory_new_types[] = { NULL, &npt_swapchain_iface };
static pthread_once_t npt_wl_types_once = PTHREAD_ONCE_INIT;

static void
npt_wl_init_types(void)
{
   void *lib = dlopen("libwayland-client.so.0", RTLD_LAZY | RTLD_LOCAL);
   if (!lib) return;
   npt_buffer_new_types[0] = dlsym(lib, "wl_buffer_interface");
   npt_factory_new_types[0] = dlsym(lib, "wl_surface_interface");
}

static const struct wl_message npt_dmabuf_requests[] = {
   { "destroy", "", NULL }, { "create_params", "n", npt_params_new_types },
};
static const struct wl_message npt_dmabuf_events[] = {
   { "format", "u", NULL }, { "modifier", "3uuu", NULL },
};
static const struct wl_message npt_params_requests[] = {
   { "destroy", "", NULL }, { "add", "huuuuu", NULL },
   { "create", "iiuu", NULL }, { "create_immed", "2niiuu", npt_buffer_new_types },
};
static const struct wl_message npt_params_events[] = {
   { "created", "n", npt_buffer_new_types }, { "failed", "", NULL },
};
static const struct wl_message npt_factory_requests[] = {
   { "destroy", "", NULL }, { "create_swapchain", "on", npt_factory_new_types },
};
static const struct wl_message npt_swapchain_requests[] = {
   { "destroy", "", NULL }, { "override_window_content", "uu", NULL },
   { "swapchain_feedback", "uuuuuus", NULL }, { "set_present_mode", "u", NULL },
   { "set_hdr_metadata", "uuuuuuuuuuuu", NULL }, { "set_present_time", "uuu", NULL },
};
static const struct wl_message npt_swapchain_events[] = {
   { "past_present_timing", "uuuuuuuuu", NULL }, { "refresh_cycle", "uu", NULL },
   { "retired", "", NULL },
};
static const struct wl_interface npt_dmabuf_iface = {
   "zwp_linux_dmabuf_v1", 4, 2, npt_dmabuf_requests, 2, npt_dmabuf_events,
};
static const struct wl_interface npt_params_iface = {
   "zwp_linux_buffer_params_v1", 4, 4, npt_params_requests, 2, npt_params_events,
};
static const struct wl_interface npt_factory_iface = {
   "gamescope_swapchain_factory_v2", 1, 2, npt_factory_requests, 0, NULL,
};
static const struct wl_interface npt_swapchain_iface = {
   "gamescope_swapchain", 1, 6, npt_swapchain_requests, 3, npt_swapchain_events,
};

struct npt_wl_image {
   struct npt_unix_wsi *w;
   uint32_t index;
};

struct npt_wayland_hdr {
   void *lib;
   void *(*connect)(const char *);
   void (*disconnect)(void *);
   int (*roundtrip)(void *);
   int (*flush)(void *);
   int (*dispatch_pending)(void *);
   int (*prepare_read)(void *);
   int (*read_events)(void *);
   void (*cancel_read)(void *);
   int (*get_fd)(void *);
   struct wl_proxy *(*marshal)(struct wl_proxy *, uint32_t, const struct wl_interface *, uint32_t, uint32_t, ...);
   int (*add_listener)(struct wl_proxy *, void (**)(void), void *);
   void (*destroy_proxy)(struct wl_proxy *);
   uint32_t (*version)(struct wl_proxy *);
   const struct wl_interface *registry_iface, *compositor_iface, *surface_iface, *buffer_iface;
   struct wl_proxy *display, *registry, *compositor, *dmabuf, *factory, *surface, *swapchain;
   struct wl_proxy *buffers[NPT_WSI_MAX_IMAGES];
   struct npt_wl_image image[NPT_WSI_MAX_IMAGES];
   uint32_t releases[NPT_WSI_MAX_IMAGES];
   uint32_t format_flags;
   struct npt_unix_wsi *w;
};

static bool
npt_root_cardinal(xcb_connection_t *conn, const char *name, uint32_t *value)
{
   const xcb_setup_t *setup = xcb_get_setup(conn);
   if (!setup) return false;
   xcb_screen_iterator_t screen = xcb_setup_roots_iterator(setup);
   if (!screen.rem) return false;
   xcb_intern_atom_reply_t *atom = xcb_intern_atom_reply(conn,
      xcb_intern_atom(conn, 1, strlen(name), name), NULL);
   if (!atom) return false;
   xcb_atom_t id = atom->atom;
   free(atom);
   if (!id) return false;
   xcb_get_property_reply_t *reply = xcb_get_property_reply(conn,
      xcb_get_property(conn, 0, screen.data->root, id, XCB_ATOM_CARDINAL, 0, 1), NULL);
   bool ok = reply && reply->type == XCB_ATOM_CARDINAL && reply->format == 32 &&
             reply->value_len == 1 && xcb_get_property_value_length(reply) == 4;
   if (ok) memcpy(value, xcb_get_property_value(reply), 4);
   free(reply);
   return ok;
}

static uint32_t npt_wl_query_formats(const char *display, const char **reason);

static const char *
npt_gamescope_socket(char *path, size_t size)
{
   const char *display = getenv("GAMESCOPE_WAYLAND_DISPLAY");
   const char *runtime = getenv("XDG_RUNTIME_DIR");
   if (!display || !*display) display = "gamescope-0";
   if (display[0] != '/' && (!runtime || !*runtime)) return NULL;
   int len = display[0] == '/' ? snprintf(path, size, "%s", display) :
                                 snprintf(path, size, "%s/%s", runtime, display);
   return len >= 0 && (size_t)len < size ? display : NULL;
}

static const char *
npt_json_space(const char *s)
{
   while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
   return s;
}

static bool
npt_hdr_json(const char *json, bool *supported, unsigned *max, unsigned *fall, unsigned *min)
{
   bool have_supported = false, have_max = false;
   const char *s = npt_json_space(json);
   if (*s++ != '{') return false;
   for (;;) {
      s = npt_json_space(s);
      if (*s == '}') { s++; break; }
      if (*s++ != '"') return false;
      const char *key = s;
      while (*s && *s != '"' && *s != '\\' && (unsigned char)*s >= 32) s++;
      if (*s != '"') return false;
      size_t len = s++ - key;
      s = npt_json_space(s);
      if (*s++ != ':') return false;
      s = npt_json_space(s);
      bool is_supported = len == 9 && !memcmp(key, "supported", len);
      bool is_max = len == 8 && !memcmp(key, "max_nits", len);
      bool is_fall = len == 13 && !memcmp(key, "max_fall_nits", len);
      bool is_min = len == 17 && !memcmp(key, "min_nits_10000ths", len);
      if (is_supported) {
         if (!strncmp(s, "true", 4)) { *supported = true; s += 4; }
         else if (!strncmp(s, "false", 5)) { *supported = false; s += 5; }
         else return false;
         have_supported = true;
      } else {
         if (*s < '0' || *s > '9') return false;
         unsigned value = 0;
         do {
            if (value > 100000000u) return false;
            value = value * 10 + (*s++ - '0');
         } while (*s >= '0' && *s <= '9');
         if (is_max) { *max = value; have_max = true; }
         if (is_fall) *fall = value;
         if (is_min) *min = value;
      }
      s = npt_json_space(s);
      if (*s == '}') { s++; break; }
      if (*s++ != ',') return false;
   }
   return !*npt_json_space(s) && have_supported && ((have_max && *max) || !*supported) &&
          *max <= 10000 && *min <= 100000000u && (!*max || *min <= *max * 10000u);
}

static float
npt_hdr_cta_luminance(unsigned code)
{
   static const float powers[32] = {
      1.00000000f, 1.02189715f, 1.04427378f, 1.06714040f,
      1.09050773f, 1.11438674f, 1.13878863f, 1.16372486f,
      1.18920712f, 1.21524736f, 1.24185781f, 1.26905096f,
      1.29683955f, 1.32523664f, 1.35425555f, 1.38390988f,
      1.41421356f, 1.44518081f, 1.47682615f, 1.50916443f,
      1.54221083f, 1.57598085f, 1.61049033f, 1.64575548f,
      1.68179283f, 1.71861930f, 1.75625216f, 1.79470908f,
      1.83400809f, 1.87416763f, 1.91520656f, 1.95714412f,
   };
   return 50.0f * powers[code % 32] * (1u << (code / 32));
}

static bool
npt_hdr_edid(struct npt_unix_wsi_hdr_caps_args *p)
{
   unsigned char edid[128 * 256];
   FILE *f = fopen("/sys/class/drm/card0-Virtual-1/edid", "rb");
   if (!f) return false;
   size_t size = fread(edid, 1, sizeof(edid), f);
   fclose(f);
   if (size < 256) return false;
   for (size_t block = 1; block <= edid[126] && (block + 1) * 128 <= size; block++) {
      const unsigned char *e = edid + block * 128;
      if (e[0] != 0x02 || e[2] < 4 || e[2] > 127) continue;
      for (unsigned i = 4; i < e[2];) {
         unsigned len = e[i] & 31, tag = e[i] >> 5;
         if (i + 1 + len > e[2]) break;
         if (tag == 7 && len >= 4 && e[i + 1] == 6 && e[i + 4]) {
            p->max_luminance = npt_hdr_cta_luminance(e[i + 4]);
            p->max_full_frame_luminance = len >= 5 && e[i + 5] ?
               npt_hdr_cta_luminance(e[i + 5]) : p->max_luminance;
            if (p->max_full_frame_luminance > p->max_luminance)
               p->max_full_frame_luminance = p->max_luminance;
            if (len >= 6) {
               float ratio = e[i + 6] / 255.0f;
               p->min_luminance = p->max_luminance * ratio * ratio / 100.0f;
            }
            return true;
         }
         i += len + 1;
      }
   }
   return false;
}

static const char *
npt_hdr_luminance(struct npt_unix_wsi_hdr_caps_args *p)
{
   char buf[4096];
   FILE *f = fopen("/run/brinewell/hdr/display.json", "rb");
   if (f) {
      size_t len = fread(buf, 1, sizeof(buf) - 1, f);
      bool complete = !ferror(f) && feof(f);
      fclose(f);
      buf[len] = 0;
      bool supported = true;
      unsigned max = 0, fall = 0, min = 50;
      if (complete && npt_hdr_json(buf, &supported, &max, &fall, &min)) {
         if (!supported) p->hdr_supported = 0;
         if (max) {
            p->max_luminance = max;
            p->max_full_frame_luminance = fall && fall <= max ? fall : max;
            p->min_luminance = min / 10000.0f;
         }
         return supported ? "json" : "host supported=0";
      }
   }
   return npt_hdr_edid(p) ? "edid" : "default";
}

NTSTATUS npt_do_wsi_query_hdr_caps(void *args)
{
   struct npt_unix_wsi_hdr_caps_args *p = args;
   static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
   static char previous[512];
   static char cached_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
   static uint32_t cached_formats;
   static struct timespec cached_time;
   static const char *cached_reason;
   char path[sizeof(((struct sockaddr_un *)0)->sun_path)] = "<unresolved>";
   const char *reason = "ok", *source = "none";
   const char *enabled = getenv("BRINEWELL_HDR");
   const char *display = npt_gamescope_socket(path, sizeof(path));
   memset(p, 0, sizeof(*p));
   pthread_mutex_lock(&lock);
   if (enabled && !strcmp(enabled, "0")) { reason = "BRINEWELL_HDR=0"; goto done; }
   if (!display) { reason = "socket path unresolved"; goto done; }
   if (access(path, R_OK | W_OK)) { reason = "access failed"; goto done; }
   xcb_connection_t *conn = xcb_connect(NULL, NULL);
   uint32_t feedback, server;
   if (!conn || xcb_connection_has_error(conn)) reason = "xcb_connect failed";
   else if (!npt_root_cardinal(conn, "GAMESCOPE_HDR_OUTPUT_FEEDBACK", &feedback) || !feedback)
      reason = "hdr_feedback=0 or unavailable";
   else if (!npt_root_cardinal(conn, "GAMESCOPE_XWAYLAND_SERVER_ID", &server))
      reason = "server id unavailable";
   if (conn) xcb_disconnect(conn);
   if (strcmp(reason, "ok")) goto done;
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   int64_t elapsed = (int64_t)(now.tv_sec - cached_time.tv_sec) * 1000000000ll +
                     now.tv_nsec - cached_time.tv_nsec;
   if (!cached_reason || strcmp(path, cached_path) || elapsed < 0 || elapsed >= 500000000ll) {
      cached_formats = npt_wl_query_formats(display, &cached_reason);
      cached_time = now;
      snprintf(cached_path, sizeof(cached_path), "%s", path);
   }
   p->format_flags = cached_formats;
   if (!p->format_flags) { reason = cached_reason; goto done; }
   p->hdr_supported = 1;
   p->max_luminance = p->max_full_frame_luminance = 1000.0f;
   p->min_luminance = 0.005f;
   source = npt_hdr_luminance(p);
done:;
   char message[512];
   snprintf(message, sizeof(message), "HDR=%u socket=%s formats=0x%x luminance=%.3f/%.3f/%.4f source=%s reason=%s",
            p->hdr_supported, path, p->format_flags, p->max_luminance,
            p->max_full_frame_luminance, p->min_luminance, source, reason);
   const char *diagnostics = getenv("NPT_WSI_DIAGNOSTICS");
   if ((getenv("NPT_DEBUG") || (diagnostics && !strcmp(diagnostics, "1"))) &&
       strcmp(message, previous)) {
      fprintf(stderr, "neptune unixlib wsi: %s\n", message);
      memcpy(previous, message, sizeof(previous));
   }
   pthread_mutex_unlock(&lock);
   return STATUS_SUCCESS;
}

static void npt_wl_format(void *data, struct wl_proxy *proxy, uint32_t format)
{ (void)data; (void)proxy; (void)format; }
static void npt_wl_modifier(void *data, struct wl_proxy *proxy, uint32_t format, uint32_t hi, uint32_t lo)
{
   struct npt_wayland_hdr *h = data;
   (void)proxy;
   if (hi || lo) return;
   if (format == 0x30334241) h->format_flags |= NPT_HDR_FMT_R10G10B10A2;
   if (format == 0x48344241) h->format_flags |= NPT_HDR_FMT_R16G16B16A16F;
}

static void npt_wl_global(void *data, struct wl_proxy *registry, uint32_t name,
                          const char *interface, uint32_t version)
{
   struct npt_wayland_hdr *h = data;
   const struct wl_interface *iface = NULL;
   struct wl_proxy **out = NULL;
   uint32_t max = 1;
   if (!strcmp(interface, "wl_compositor")) { iface = h->compositor_iface; out = &h->compositor; max = 4; }
   else if (!strcmp(interface, "zwp_linux_dmabuf_v1") && version >= 3) { iface = &npt_dmabuf_iface; out = &h->dmabuf; max = 3; }
   else if (!strcmp(interface, "gamescope_swapchain_factory_v2")) { iface = &npt_factory_iface; out = &h->factory; }
   if (out && !*out) {
      if (version > max) version = max;
      *out = h->marshal(registry, 0, iface, version, 0, name, iface->name, version, NULL);
      if (*out && out == &h->dmabuf) {
         static void (*listener[])(void) = { (void (*)(void))npt_wl_format, (void (*)(void))npt_wl_modifier };
         h->add_listener(*out, listener, h);
      }
   }
}
static void npt_wl_global_remove(void *data, struct wl_proxy *registry, uint32_t name)
{ (void)data; (void)registry; (void)name; }
static uint32_t npt_wl_query_formats(const char *display, const char **reason)
{
   struct npt_wayland_hdr h = {0};
   uint32_t flags = 0;
   pthread_once(&npt_wl_types_once, npt_wl_init_types);
   if (!npt_buffer_new_types[0] || !npt_factory_new_types[0]) {
      *reason = "wayland interface unavailable";
      return 0;
   }
   h.lib = dlopen("libwayland-client.so.0", RTLD_LAZY | RTLD_LOCAL);
   if (!h.lib) { *reason = "dlopen failed"; return 0; }
   *reason = "wayland query failed";
#define NPT_QUERY_LOAD(field, symbol) do { *(void **)(&h.field) = dlsym(h.lib, symbol); if (!h.field) goto out; } while (0)
   NPT_QUERY_LOAD(connect, "wl_display_connect");
   NPT_QUERY_LOAD(disconnect, "wl_display_disconnect");
   NPT_QUERY_LOAD(roundtrip, "wl_display_roundtrip");
   NPT_QUERY_LOAD(marshal, "wl_proxy_marshal_flags");
   NPT_QUERY_LOAD(add_listener, "wl_proxy_add_listener");
   NPT_QUERY_LOAD(destroy_proxy, "wl_proxy_destroy");
   NPT_QUERY_LOAD(registry_iface, "wl_registry_interface");
   NPT_QUERY_LOAD(compositor_iface, "wl_compositor_interface");
#undef NPT_QUERY_LOAD
   h.display = h.connect(display);
   if (!h.display) goto out;
   h.registry = h.marshal(h.display, 1, h.registry_iface, 1, 0, NULL);
   static void (*listener[])(void) = { (void (*)(void))npt_wl_global, (void (*)(void))npt_wl_global_remove };
   if (!h.registry || h.add_listener(h.registry, listener, &h) ||
       h.roundtrip(h.display) < 0 || !h.dmabuf || !h.factory ||
       h.roundtrip(h.display) < 0) goto out;
   flags = h.format_flags;
   *reason = flags ? "ok" : "dmabuf no hdr formats";
out:
   if (h.compositor) h.destroy_proxy(h.compositor);
   if (h.dmabuf) h.destroy_proxy(h.dmabuf);
   if (h.factory) h.destroy_proxy(h.factory);
   if (h.registry) h.destroy_proxy(h.registry);
   if (h.display) h.disconnect(h.display);
   dlclose(h.lib);
   return flags;
}

static void npt_wl_release(void *data, struct wl_proxy *buffer)
{
   (void)buffer;
   struct npt_wl_image *image = data;
   image->w->wayland->releases[image->index]++;
}
static void npt_wl_timing(void *data, struct wl_proxy *proxy, uint32_t a, uint32_t b, uint32_t c,
                          uint32_t d, uint32_t e, uint32_t f, uint32_t g, uint32_t h, uint32_t i)
{ (void)data; (void)proxy; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; }
static void npt_wl_refresh(void *data, struct wl_proxy *proxy, uint32_t a, uint32_t b)
{ (void)data; (void)proxy; (void)a; (void)b; }
static void npt_wl_retired(void *data, struct wl_proxy *proxy)
{
   struct npt_wayland_hdr *h = data;
   (void)proxy;
   atomic_store(&h->w->retired, 1);
   (void)write(h->w->wake_pipe[1], "r", 1);
}

static void npt_wl_metadata(struct npt_wayland_hdr *h, const struct npt_unix_hdr10_metadata *m)
{
   h->marshal(h->swapchain, 4, NULL, h->version(h->swapchain), 0,
      (uint32_t)m->red_primary[0], (uint32_t)m->red_primary[1],
      (uint32_t)m->green_primary[0], (uint32_t)m->green_primary[1],
      (uint32_t)m->blue_primary[0], (uint32_t)m->blue_primary[1],
      (uint32_t)m->white_point[0], (uint32_t)m->white_point[1],
      m->max_mastering_luminance > 65535u ? 65535u : m->max_mastering_luminance,
      m->min_mastering_luminance > 65535u ? 65535u : m->min_mastering_luminance,
      (uint32_t)m->max_cll, (uint32_t)m->max_fall);
}

static void npt_wl_free(struct npt_unix_wsi *w)
{
   struct npt_wayland_hdr *h = w->wayland;
   if (!h) return;
   for (uint32_t i = 0; i < w->num_images; i++)
      if (h->buffers[i]) h->marshal(h->buffers[i], 0, NULL, h->version(h->buffers[i]), 1);
   if (h->swapchain) h->marshal(h->swapchain, 0, NULL, h->version(h->swapchain), 1);
   if (h->surface) h->marshal(h->surface, 0, NULL, h->version(h->surface), 1);
   if (h->factory) h->marshal(h->factory, 0, NULL, h->version(h->factory), 1);
   if (h->dmabuf) h->marshal(h->dmabuf, 0, NULL, h->version(h->dmabuf), 1);
   if (h->compositor) h->destroy_proxy(h->compositor);
   if (h->registry) h->destroy_proxy(h->registry);
   if (h->display) h->disconnect(h->display);
   if (h->lib) dlclose(h->lib);
   free(h);
   w->wayland = NULL;
}

static bool npt_wl_init(struct npt_unix_wsi *w, struct npt_unix_wsi_init_params *p)
{
   uint32_t server, fourcc, vkformat;
   if (!npt_root_cardinal(w->conn, "GAMESCOPE_XWAYLAND_SERVER_ID", &server)) return false;
   switch (p->virgl_format) {
   case 8: fourcc = 0x30334241; vkformat = 64; break;
   case 94: fourcc = 0x48344241; vkformat = 97; break;
   case 1: fourcc = 0x34325241; vkformat = 44; break;
   case 67: fourcc = 0x34324241; vkformat = 37; break;
   default: return false;
   }
   struct npt_wayland_hdr *h = calloc(1, sizeof(*h));
   if (!h) return false;
   w->wayland = h;
   h->w = w;
   h->lib = dlopen("libwayland-client.so.0", RTLD_LAZY | RTLD_LOCAL);
   if (!h->lib) return false;
#define NPT_WL_LOAD(field, symbol) do { *(void **)(&h->field) = dlsym(h->lib, symbol); if (!h->field) return false; } while (0)
   NPT_WL_LOAD(connect, "wl_display_connect"); NPT_WL_LOAD(disconnect, "wl_display_disconnect");
   NPT_WL_LOAD(roundtrip, "wl_display_roundtrip"); NPT_WL_LOAD(flush, "wl_display_flush");
   NPT_WL_LOAD(dispatch_pending, "wl_display_dispatch_pending"); NPT_WL_LOAD(prepare_read, "wl_display_prepare_read");
   NPT_WL_LOAD(read_events, "wl_display_read_events"); NPT_WL_LOAD(cancel_read, "wl_display_cancel_read");
   NPT_WL_LOAD(get_fd, "wl_display_get_fd"); NPT_WL_LOAD(marshal, "wl_proxy_marshal_flags");
   NPT_WL_LOAD(add_listener, "wl_proxy_add_listener"); NPT_WL_LOAD(destroy_proxy, "wl_proxy_destroy");
   NPT_WL_LOAD(version, "wl_proxy_get_version"); NPT_WL_LOAD(registry_iface, "wl_registry_interface");
   NPT_WL_LOAD(compositor_iface, "wl_compositor_interface"); NPT_WL_LOAD(surface_iface, "wl_surface_interface");
   NPT_WL_LOAD(buffer_iface, "wl_buffer_interface");
#undef NPT_WL_LOAD
   pthread_once(&npt_wl_types_once, npt_wl_init_types);
   if (!npt_buffer_new_types[0] || !npt_factory_new_types[0]) return false;
   char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
   const char *display = npt_gamescope_socket(path, sizeof(path));
   if (!display) return false;
   h->display = h->connect(display);
   if (!h->display) return false;
   h->registry = h->marshal(h->display, 1, h->registry_iface, 1, 0, NULL);
   static void (*registry_listener[])(void) = { (void (*)(void))npt_wl_global, (void (*)(void))npt_wl_global_remove };
   if (!h->registry || h->add_listener(h->registry, registry_listener, h) || h->roundtrip(h->display) < 0 ||
       !h->compositor || !h->dmabuf || !h->factory || h->version(h->dmabuf) < 3 ||
       h->roundtrip(h->display) < 0) return false;
   if ((p->virgl_format == 8 && !(h->format_flags & NPT_HDR_FMT_R10G10B10A2)) ||
       (p->virgl_format == 94 && !(h->format_flags & NPT_HDR_FMT_R16G16B16A16F))) return false;
   h->surface = h->marshal(h->compositor, 0, h->surface_iface, h->version(h->compositor), 0, NULL);
   if (!h->surface) return false;
   h->swapchain = h->marshal(h->factory, 1, &npt_swapchain_iface, 1, 0, h->surface, NULL);
   if (!h->swapchain) return false;
   static void (*swapchain_listener[])(void) = { (void (*)(void))npt_wl_timing, (void (*)(void))npt_wl_refresh, (void (*)(void))npt_wl_retired };
   if (h->add_listener(h->swapchain, swapchain_listener, h)) return false;
   h->marshal(h->swapchain, 1, NULL, 1, 0, server, p->x11_window);
   uint32_t colorspace = p->colorspace == NPT_DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? 1000104008 :
                         p->colorspace == NPT_DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 ? 1000104002 : 0;
   h->marshal(h->swapchain, 2, NULL, 1, 0, p->num_images, vkformat, colorspace, 1u, 1u, 1u, "Neptune");
   h->marshal(h->swapchain, 3, NULL, 1, 0, 0u);
   if (p->hdr_meta.valid) npt_wl_metadata(h, &p->hdr_meta);
   else if (p->colorspace == NPT_DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
      const struct npt_unix_hdr10_metadata m = { 1, {35400,14600}, {8500,39850}, {6550,2300}, {15635,16450}, 1000, 50, 1000, 400 };
      npt_wl_metadata(h, &m);
   }
   static void (*buffer_listener[])(void) = { (void (*)(void))npt_wl_release };
   for (uint32_t i = 0; i < p->num_images; i++) {
      struct wl_proxy *params = h->marshal(h->dmabuf, 1, &npt_params_iface, h->version(h->dmabuf), 0, NULL);
      if (!params) return false;
      int fd = dup(p->fds[i]);
      if (fd < 0) {
         h->marshal(params, 0, NULL, h->version(params), 1);
         return false;
      }
      h->marshal(params, 1, NULL, h->version(params), 0, fd, 0u, 0u, p->strides[i], 0u, 0u);
      close(fd);
      h->buffers[i] = h->marshal(params, 3, h->buffer_iface, h->version(params), 0, NULL, p->width, p->height, fourcc, 0u);
      h->marshal(params, 0, NULL, h->version(params), 1);
      h->image[i].w = w; h->image[i].index = i;
      if (!h->buffers[i] || h->add_listener(h->buffers[i], buffer_listener, &h->image[i])) return false;
   }
   return h->roundtrip(h->display) >= 0;
}

static uint64_t monotonic_ns(void);

static void npt_wl_present(struct npt_unix_wsi *w, struct npt_unix_wsi_present_params *p)
{
   struct npt_wayland_hdr *h = w->wayland;
   if (p->wait_fence_fd >= 0) {
      struct pollfd fds[2] = {
         { .fd = p->wait_fence_fd, .events = POLLIN },
         { .fd = w->cancel_pipe[0], .events = POLLIN },
      };
      int rc;
      do { rc = poll(fds, 2, 30000); } while (rc < 0 && errno == EINTR);
      close(p->wait_fence_fd);
      p->wait_fence_consumed = 1;
      if (rc <= 0 || fds[1].revents || !(fds[0].revents & POLLIN) ||
          (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))) return;
   }
   pthread_mutex_lock(&w->mtx);
   if (atomic_load(&w->retired)) {
      fprintf(stderr, "neptune unixlib wsi: Wayland swapchain retired during present\n");
      p->result = -2;
   } else if (!atomic_load(&w->cancelled) && !atomic_load(&w->terminal_err)) {
      uint32_t version = h->version(h->surface);
      h->marshal(h->surface, 1, NULL, version, 0, h->buffers[p->image_index], 0, 0);
      h->marshal(h->surface, version >= 4 ? 9 : 2, NULL, version, 0,
                 0, 0, (int32_t)w->width, (int32_t)w->height);
      h->marshal(h->surface, 6, NULL, version, 0);
      if (h->flush(h->display) < 0 && errno != EAGAIN) {
         atomic_store(&w->terminal_err, -2);
         p->result = -2;
      } else p->result = 0;
   } else if (atomic_load(&w->terminal_err)) p->result = -2;
   pthread_mutex_unlock(&w->mtx);
}

static void npt_wl_drain(struct npt_unix_wsi *w, struct npt_unix_wsi_drain_params *p)
{
   struct npt_wayland_hdr *h = w->wayland;
   uint64_t deadline = monotonic_ns() + (uint64_t)p->timeout_ms * 1000000ull;
   bool polled = false;
   pthread_mutex_lock(&w->drain_mtx);
   for (;;) {
      pthread_mutex_lock(&w->mtx);
      if (atomic_load(&w->cancelled)) { pthread_mutex_unlock(&w->mtx); break; }
      if (h->dispatch_pending(h->display) < 0) goto terminal_locked;
      if (atomic_load(&w->retired)) {
         fprintf(stderr, "neptune unixlib wsi: Wayland swapchain retired during drain\n");
         goto terminal_locked;
      }
      bool any = false;
      for (uint32_t i = 0; i < w->num_images; i++) {
         p->released_count[i] = h->releases[i];
         h->releases[i] = 0;
         any |= p->released_count[i] != 0;
      }
      uint64_t now = monotonic_ns();
      if (any || (now >= deadline && polled)) {
         p->result = 0;
         pthread_mutex_unlock(&w->mtx);
         break;
      }
      if (h->prepare_read(h->display) < 0) {
         pthread_mutex_unlock(&w->mtx);
         continue;
      }
      int flushed = h->flush(h->display);
      if (flushed < 0 && errno != EAGAIN) {
         h->cancel_read(h->display);
         goto terminal_locked;
      }
      struct pollfd fds[3] = {
         { .fd = h->get_fd(h->display), .events = POLLIN | (flushed < 0 ? POLLOUT : 0) },
         { .fd = w->cancel_pipe[0], .events = POLLIN },
         { .fd = w->wake_pipe[0], .events = POLLIN },
      };
      uint64_t ms = now >= deadline ? 0 : (deadline - now + 999999) / 1000000;
      if (ms > INT32_MAX) ms = INT32_MAX;
      pthread_mutex_unlock(&w->mtx);
      int rc = poll(fds, 3, (int)ms);
      polled = true;
      pthread_mutex_lock(&w->mtx);
      if (rc > 0 && (fds[0].revents & POLLIN) && !fds[1].revents) {
         if (h->read_events(h->display) < 0) goto terminal_locked;
      } else h->cancel_read(h->display);
      if (atomic_load(&w->cancelled) || fds[1].revents) {
         pthread_mutex_unlock(&w->mtx);
         break;
      }
      if ((rc < 0 && errno != EINTR) || (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)))
         goto terminal_locked;
      pthread_mutex_unlock(&w->mtx);
   }
   pthread_mutex_unlock(&w->drain_mtx);
   return;
terminal_locked:
   atomic_store(&w->terminal_err, -2);
   p->result = -2;
   pthread_mutex_unlock(&w->mtx);
   pthread_mutex_unlock(&w->drain_mtx);
}

NTSTATUS npt_do_wsi_set_hdr_metadata(void *args)
{
   struct npt_unix_wsi_set_hdr_metadata_args *p = args;
   struct npt_unix_wsi *w = (struct npt_unix_wsi *)(uintptr_t)p->handle;
   p->result = -1;
   if (!w || !w->wayland) return STATUS_SUCCESS;
   pthread_mutex_lock(&w->mtx);
   if (!atomic_load(&w->cancelled) && !atomic_load(&w->terminal_err)) {
      npt_wl_metadata(w->wayland, &p->meta);
      if (w->wayland->flush(w->wayland->display) >= 0 || errno == EAGAIN) p->result = 0;
      else atomic_store(&w->terminal_err, -2);
   }
   pthread_mutex_unlock(&w->mtx);
   return STATUS_SUCCESS;
}

#endif
