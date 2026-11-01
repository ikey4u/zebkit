// GTK owns the window and the main loop; zebkit only renders the web pane.
#include "model.h"
#include <gtk/gtk.h>
#include <climits>
#include <cstdio>
#include <mutex>

struct Host {
  native_host::Model model;
  GtkWidget *window = nullptr, *web = nullptr, *pdf = nullptr;
  GtkIMContext* ime = nullptr;
  std::mutex mutex;
  gint64 deadline = G_MAXINT64;
  bool queued = false, active = true;
  guint timer = 0, pdf_timer = 0;
  std::string smoke;
  int pumps = 0;

  static void Wake(int64_t delay, void* data) {
    auto& self = *static_cast<Host*>(data);
    std::lock_guard<std::mutex> lock(self.mutex);
    if (!self.active) return;
    self.deadline = std::min(self.deadline, g_get_monotonic_time() + std::min<int64_t>(delay, INT_MAX) * 1000);
    if (!self.queued) {
      self.queued = true;
      g_idle_add_full(G_PRIORITY_DEFAULT, [](void* data) -> gboolean {
        auto& host = *static_cast<Host*>(data);
        host.Dispatch();
        return G_SOURCE_REMOVE;
      }, &self, nullptr);
    }
  }
  void AnimatePdf() {
    if (pdf_timer || !model.pane.Animating()) return;
    pdf_timer = g_timeout_add(16, [](void* data) -> gboolean {
      auto& host = *static_cast<Host*>(data);
      host.model.pane.Tick();
      if (host.model.pane.Animating()) return G_SOURCE_CONTINUE;
      host.pdf_timer = 0;
      return G_SOURCE_REMOVE;
    }, this);
  }
  void Dispatch() {
    gint64 due;
    {
      std::lock_guard<std::mutex> lock(mutex);
      queued = false;
      if (!active) return;
      due = deadline;
      if (due <= g_get_monotonic_time()) deadline = G_MAXINT64;
    }
    if (timer) { g_source_remove(timer); timer = 0; }
    if (due == G_MAXINT64) return;
    const gint64 remaining = due - g_get_monotonic_time();
    if (remaining > 0) {
      timer = g_timeout_add(static_cast<guint>((remaining + 999) / 1000), [](void* data) -> gboolean {
        auto& host = *static_cast<Host*>(data);
        host.timer = 0;
        host.Dispatch();
        return G_SOURCE_REMOVE;
      }, this);
    } else {
      ++pumps;
      zebkit_pump();
    }
  }
  static unsigned Modifiers(guint state) {
    unsigned flags = 0;
    if (state & GDK_SHIFT_MASK) flags |= ZEBKIT_MODIFIER_SHIFT;
    if (state & GDK_CONTROL_MASK) flags |= ZEBKIT_MODIFIER_CONTROL;
    if (state & GDK_MOD1_MASK) flags |= ZEBKIT_MODIFIER_ALT;
    if (state & GDK_SUPER_MASK) flags |= ZEBKIT_MODIFIER_META;
    if (state & GDK_BUTTON1_MASK) flags |= ZEBKIT_MODIFIER_LEFT_BUTTON;
    if (state & GDK_BUTTON2_MASK) flags |= ZEBKIT_MODIFIER_MIDDLE_BUTTON;
    if (state & GDK_BUTTON3_MASK) flags |= ZEBKIT_MODIFIER_RIGHT_BUTTON;
    return flags;
  }
  static void Paint(cairo_t* cr, const native_host::Image& image, bool fit, int width = 0, int height = 0) {
    if (image.bgra.empty()) return;
    cairo_surface_t* surface = cairo_image_surface_create_for_data(
      const_cast<uint8_t*>(image.bgra.data()), CAIRO_FORMAT_ARGB32, image.width, image.height, image.width * 4);
    cairo_save(cr);
    if (fit) {
      const double scale = std::min(static_cast<double>(width - 20) / image.width,
                                    static_cast<double>(height - 20) / image.height);
      cairo_translate(cr, (width - image.width * scale) / 2, (height - image.height * scale) / 2);
      cairo_scale(cr, scale, scale);
    } else {
      cairo_translate(cr, image.bounds.x, image.bounds.y);
      cairo_scale(cr, 1.0 / image.scale, 1.0 / image.scale);
    }
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_surface_destroy(surface);
  }
  void Build() {
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "zebkit — GTK host window");
    gtk_window_set_default_size(GTK_WINDOW(window), 1200, 740);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_add(GTK_CONTAINER(window), box);
    GtkWidget* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(box), toolbar, FALSE, FALSE, 4);
    for (int delta : {-1, 1}) {
      GtkWidget* button = gtk_button_new_with_label(delta < 0 ? "PDF: a screen up" : "PDF: a screen down");
      g_object_set_data(G_OBJECT(button), "delta", GINT_TO_POINTER(delta));
      g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton* button, void* data) {
        auto& host = *static_cast<Host*>(data);
        host.model.pane.Pages(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "delta")));
        host.AnimatePdf();
      }), this);
      gtk_box_pack_start(GTK_BOX(toolbar), button, FALSE, FALSE, 0);
    }
    GtkWidget* split = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(box), split, TRUE, TRUE, 0);
    web = gtk_drawing_area_new(); pdf = gtk_drawing_area_new();
    gtk_paned_pack1(GTK_PANED(split), web, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(split), pdf, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(split), 600);
    gtk_widget_set_can_focus(web, TRUE);
    gtk_widget_add_events(web, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
      GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_KEY_PRESS_MASK |
      GDK_KEY_RELEASE_MASK | GDK_FOCUS_CHANGE_MASK);
    gtk_widget_add_events(pdf, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    model.redraw = [this] { gtk_widget_queue_draw(web); gtk_widget_queue_draw(pdf); };
    g_signal_connect(pdf, "scroll-event", G_CALLBACK(+[](GtkWidget*, GdkEventScroll* event, void* data) -> gboolean {
      auto& host = *static_cast<Host*>(data);
      double dx = 0; double dy = 0;
      // A real wheel gives whole notches, in steps of one; a trackpad less, often.
      if (gdk_event_get_scroll_deltas(reinterpret_cast<GdkEvent*>(event), &dx, &dy)) {
        const bool precise = dy != std::floor(dy);
        if (precise) host.model.pane.Wheel(dy * 40, true);
        else host.model.pane.WheelNotches(dy);
      } else if (event->direction == GDK_SCROLL_UP || event->direction == GDK_SCROLL_DOWN) {
        host.model.pane.WheelNotches(event->direction == GDK_SCROLL_DOWN ? 1 : -1);
      }
      host.AnimatePdf();
      return TRUE;
    }), this);
    g_signal_connect(pdf, "size-allocate", G_CALLBACK(+[](GtkWidget*, GtkAllocation* size, void* data) {
      static_cast<Host*>(data)->model.pane.Resize(std::max(1, size->width), std::max(1, size->height));
    }), this);
    g_signal_connect(web, "draw", G_CALLBACK(+[](GtkWidget*, cairo_t* cr, void* data) -> gboolean {
      auto& host = *static_cast<Host*>(data);
      Paint(cr, host.model.web, false);
      if (host.model.popup_shown) Paint(cr, host.model.popup, false);
      return TRUE;
    }), this);
    g_signal_connect(pdf, "draw", G_CALLBACK(+[](GtkWidget* widget, cairo_t* cr, void* data) -> gboolean {
      auto& pane = static_cast<Host*>(data)->model.pane;
      pane.Resize(std::max(1, gtk_widget_get_allocated_width(widget)), std::max(1, gtk_widget_get_allocated_height(widget)));
      Paint(cr, pane.Canvas(), false);
      return TRUE;
    }), this);
    g_signal_connect(web, "size-allocate", G_CALLBACK(+[](GtkWidget* widget, GtkAllocation* size, void* data) {
      auto& host = *static_cast<Host*>(data);
      if (host.model.view) zebkit_view_set_size(host.model.view, std::max(1, size->width), std::max(1, size->height),
                                                gtk_widget_get_scale_factor(widget));
    }), this);
    g_signal_connect(web, "event", G_CALLBACK(+[](GtkWidget* widget, GdkEvent* event, void* data) -> gboolean {
      auto& host = *static_cast<Host*>(data);
      auto* view = host.model.view;
      if (!view) return FALSE;
      switch (event->type) {
        // A double click is press, release, press, 2press, release. The second
        // press is the page's second mouse down, so 2press is not another one.
        case GDK_2BUTTON_PRESS: return TRUE;
        case GDK_BUTTON_PRESS: case GDK_BUTTON_RELEASE: {
          const auto& e = event->button;
          static guint32 last_press = 0;
          static guint last_button = 0;
          static int clicks = 1;
          if (event->type == GDK_BUTTON_PRESS) {
            clicks = e.button == last_button && e.time - last_press < 400 ? clicks + 1 : 1;
            last_press = e.time;
            last_button = e.button;
          }
          gtk_widget_grab_focus(widget);
          const auto button = e.button == 1 ? ZEBKIT_BUTTON_LEFT : e.button == 2 ? ZEBKIT_BUTTON_MIDDLE :
                              e.button == 3 ? ZEBKIT_BUTTON_RIGHT : ZEBKIT_BUTTON_NONE;
          zebkit_view_send_mouse(view, event->type == GDK_BUTTON_RELEASE ? ZEBKIT_MOUSE_UP : ZEBKIT_MOUSE_DOWN,
            e.x, e.y, button, clicks, Modifiers(e.state));
          return TRUE;
        }
        case GDK_MOTION_NOTIFY:
          zebkit_view_send_mouse(view, ZEBKIT_MOUSE_MOVE, event->motion.x, event->motion.y,
            ZEBKIT_BUTTON_NONE, 0, Modifiers(event->motion.state)); return TRUE;
        case GDK_LEAVE_NOTIFY:
          zebkit_view_send_mouse(view, ZEBKIT_MOUSE_LEAVE, event->crossing.x, event->crossing.y,
            ZEBKIT_BUTTON_NONE, 0, Modifiers(event->crossing.state)); return TRUE;
        case GDK_SCROLL: {
          double dx = 0; double dy = 0;
          const bool smooth = gdk_event_get_scroll_deltas(event, &dx, &dy);
          if (!smooth) {
            dx = event->scroll.direction == GDK_SCROLL_LEFT ? -1 : event->scroll.direction == GDK_SCROLL_RIGHT ? 1 : 0;
            dy = event->scroll.direction == GDK_SCROLL_UP ? -1 : event->scroll.direction == GDK_SCROLL_DOWN ? 1 : 0;
          }
          zebkit_view_send_wheel(view, event->scroll.x, event->scroll.y, dx * 40, dy * 40,
            smooth, ZEBKIT_WHEEL_PHASE_NONE, Modifiers(event->scroll.state)); return TRUE;
        }
        case GDK_FOCUS_CHANGE:
          zebkit_view_set_focus(view, event->focus_change.in);
          if (event->focus_change.in) gtk_im_context_focus_in(host.ime); else gtk_im_context_focus_out(host.ime);
          return FALSE;
        case GDK_KEY_PRESS: case GDK_KEY_RELEASE: {
          const auto& e = event->key;
          zebkit_key_event key{};
          key.type = event->type == GDK_KEY_PRESS ? ZEBKIT_KEY_DOWN : ZEBKIT_KEY_UP;
          key.modifiers = Modifiers(e.state); key.native_key_code = e.hardware_keycode;
          const gunichar character = gdk_keyval_to_unicode(e.keyval);
          key.key_code = character >= 'a' && character <= 'z' ? character - 'a' + 'A' : character < 128 ? character : 0;
          switch (e.keyval) {
            case GDK_KEY_Return: key.key_code = 13; break; case GDK_KEY_BackSpace: key.key_code = 8; break;
            case GDK_KEY_Tab: key.key_code = 9; break; case GDK_KEY_Escape: key.key_code = 27; break;
            case GDK_KEY_Left: key.key_code = 37; break; case GDK_KEY_Up: key.key_code = 38; break;
            case GDK_KEY_Right: key.key_code = 39; break; case GDK_KEY_Down: key.key_code = 40; break;
            case GDK_KEY_Delete: key.key_code = 46; break;
          }
          zebkit_view_send_key(view, &key);
          gtk_im_context_filter_keypress(host.ime, &event->key);
          return TRUE;
        }
        default: return FALSE;
      }
    }), this);
    ime = gtk_im_multicontext_new();
    g_signal_connect(web, "realize", G_CALLBACK(+[](GtkWidget* widget, void* data) {
      gtk_im_context_set_client_window(static_cast<Host*>(data)->ime, gtk_widget_get_window(widget));
    }), this);
    g_signal_connect(ime, "commit", G_CALLBACK(+[](GtkIMContext*, const char* text, void* data) {
      auto* view = static_cast<Host*>(data)->model.view;
      if (view) zebkit_view_ime_commit(view, text);
    }), this);
    g_signal_connect(ime, "preedit-changed", G_CALLBACK(+[](GtkIMContext* ime, void* data) {
      gchar* text = nullptr; gint cursor = 0;
      gtk_im_context_get_preedit_string(ime, &text, nullptr, &cursor);
      auto* view = static_cast<Host*>(data)->model.view;
      if (view) {
        const char* end = g_utf8_offset_to_pointer(text, cursor);
        glong utf16 = 0; gunichar2* prefix = g_utf8_to_utf16(text, end - text, nullptr, &utf16, nullptr);
        zebkit_view_ime_set_composition(view, text, utf16, utf16);
        g_free(prefix);
      }
      g_free(text);
    }), this);
    g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget*, void*) { gtk_main_quit(); }), nullptr);
  }
};

int main(int argc, char** argv) {
  Host host;
  std::string url, pdf;
  bool print_bounds = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--print-bounds") print_bounds = true;
    if (arg.rfind("--url=", 0) == 0) url = arg.substr(6);
    if (arg.rfind("--pdf=", 0) == 0) pdf = arg.substr(6);
    if (arg.rfind("--smoke-test=", 0) == 0) host.smoke = arg.substr(13);
  }
  if (!gtk_init_check(nullptr, nullptr)) { std::fprintf(stderr, "GTK needs a display\n"); return 1; }
  host.Build();
  zebkit_set_wakeup(&Host::Wake, &host);
  char* error = nullptr;
  if (zebkit_init_with_settings(argc, argv, "{\"headless\":true}", &error)) {
    std::fprintf(stderr, "%s\n", error ? error : "Engine startup failed"); zebkit_free(error); return 1;
  }
  try { host.model.Open(url, pdf); }
  catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); host.model.Close(); zebkit_shutdown(); return 1; }
  zebkit_view_on_caret(host.model.view, [](zebkit_view*, const zebkit_rect* caret, void* data) {
    if (!caret) return;
    GdkRectangle rectangle{caret->x, caret->y, caret->width, caret->height};
    gtk_im_context_set_cursor_location(static_cast<Host*>(data)->ime, &rectangle);
  }, &host);
  zebkit_view_on_cursor(host.model.view, [](zebkit_view*, const char* name, const zebkit_cursor_image*, void* data) {
    auto* widget = static_cast<Host*>(data)->web;
    GdkCursor* cursor = gdk_cursor_new_from_name(gtk_widget_get_display(widget), name);
    if (gtk_widget_get_window(widget)) gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
    if (cursor) g_object_unref(cursor);
  }, &host);
  gtk_widget_show_all(host.window);
  Host::Wake(0, &host);
  if (print_bounds) {
    gint x = 0, y = 0;
    gtk_widget_translate_coordinates(host.web, host.window, 0, 0, &x, &y);
    std::printf("host-web-bounds: %d %d %d\n", x, y, gtk_widget_get_scale_factor(host.web));
    std::fflush(stdout);
  }
  if (!host.smoke.empty()) g_timeout_add(1500, [](void* data) -> gboolean {
    auto& host = *static_cast<Host*>(data);
    if (!host.model.frames) return G_SOURCE_CONTINUE;
    cairo_surface_t* image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
      gtk_widget_get_allocated_width(host.window), gtk_widget_get_allocated_height(host.window));
    cairo_t* cr = cairo_create(image);
    gtk_widget_draw(host.window, cr);
    const cairo_status_t saved = cairo_surface_write_to_png(image, host.smoke.c_str());
    cairo_destroy(cr); cairo_surface_destroy(image);
    if (saved != CAIRO_STATUS_SUCCESS)
      std::fprintf(stderr, "Cannot save GTK screenshot: %s\n", cairo_status_to_string(saved));
    gtk_main_quit(); return G_SOURCE_REMOVE;
  }, &host);
  gtk_main();
  { std::lock_guard<std::mutex> lock(host.mutex); host.active = false; }
  if (host.timer) g_source_remove(host.timer);
  if (host.pdf_timer) g_source_remove(host.pdf_timer);
  // The widgets went with the window; what closing calls back must not draw.
  host.model.redraw = nullptr;
  host.model.Close();
  const bool stopped = zebkit_shutdown();
  g_object_unref(host.ime);
  std::printf("GTK host: %d frames, %d event-driven pumps, shutdown=%s\n", host.model.frames, host.pumps, stopped ? "ok" : "failed");
  return stopped && host.model.frames > 0 ? 0 : 1;
}
