// A real input method in the engine's X11 windows: IBus, over its own D-Bus
// bus (ibus-daemon's, and fcitx5's through its IBus frontend).
//
// Chromium gets a Linux input method from its toolkit (GTK, Qt) or from
// Ozone (Wayland's text-input). zebkit has no toolkit, and Ozone's X11 gives
// none, so windows had Chromium's fake context: no Chinese, Japanese or Korean
// could be typed (docs/ARCHITECTURE.md). This is the context Ozone lacks,
// given to ui::CreateLinuxInputMethodContext() through the factory it asks.
//
// One private connection to IBus's bus, on the engine's thread, with libdbus
// itself: a key is asked about with a blocking call (as GTK's IBus module does
// in its synchronous mode), since Chromium wants to know at once whether the
// input method took it; what IBus says in between (the text to commit, the
// text being composed) is read when the connection's descriptor is readable.

#include "zebkit/engine/ime_ibus_linux.h"

#include <dbus/dbus.h>
#include <stdlib.h>
#include <unistd.h>

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/memory/raw_ptr.h"
#include "base/message_loop/message_pump_for_ui.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/current_thread.h"
#include "base/time/time.h"
#include "ui/base/ime/composition_text.h"
#include "ui/base/ime/ime_text_span.h"
#include "ui/base/ime/linux/linux_input_method_context.h"
#include "ui/base/ime/linux/linux_input_method_context_factory.h"
#include "ui/display/screen.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#if defined(ZEBKIT_X11)
#include "ui/events/keycodes/keyboard_code_conversion_x.h"
#endif
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/range/range.h"

namespace zebkit {

namespace {


constexpr char kService[] = "org.freedesktop.IBus";
constexpr char kPath[] = "/org/freedesktop/IBus";
constexpr char kContext[] = "org.freedesktop.IBus.InputContext";
// IBus's capabilities: the text being composed is the client's to draw, and
// the client says when it has focus.
constexpr uint32_t kCapabilities = (1u << 0) | (1u << 3);
// IBus's modifiers are X11's, with one of its own for a key's release.
constexpr uint32_t kShift = 1u << 0, kCapsLock = 1u << 1, kControl = 1u << 2, kAlt = 1u << 3,
                   kNumLock = 1u << 4, kSuper = 1u << 6, kRelease = 1u << 30;
// How long a key waits for the input method's answer before it is the page's.
constexpr int kKeyTimeoutMs = 500;

// Where ibus-daemon listens: IBUS_ADDRESS, or the file it writes for this
// machine and display, ~/.config/ibus/bus/<machine id>-<host>-<display>.
std::string BusAddress() {
  auto env = base::Environment::Create();
  if (std::optional<std::string> address = env->GetVar("IBUS_ADDRESS"); address && !address->empty())
    return *address;
  std::string machine;
  if (!base::ReadFileToString(base::FilePath("/var/lib/dbus/machine-id"), &machine) &&
      !base::ReadFileToString(base::FilePath("/etc/machine-id"), &machine)) {
    return std::string();
  }
  base::TrimWhitespaceASCII(machine, base::TRIM_ALL, &machine);
  std::string display = env->GetVar("DISPLAY").value_or(std::string());
  std::string host = "unix", number = "0";
  const size_t colon = display.rfind(':');
  if (colon != std::string::npos) {
    if (colon > 0)
      host = display.substr(0, colon);
    number = display.substr(colon + 1);
    if (const size_t dot = number.find('.'); dot != std::string::npos)
      number.resize(dot);
  }
  base::FilePath config;
  if (std::optional<std::string> xdg = env->GetVar("XDG_CONFIG_HOME"); xdg && !xdg->empty())
    config = base::FilePath(*xdg);
  else
    config = base::FilePath(env->GetVar("HOME").value_or(std::string())).Append(".config");
  std::string file;
  if (!base::ReadFileToString(config.Append("ibus/bus").Append(machine + "-" + host + "-" + number), &file))
    return std::string();
  for (const std::string& line : base::SplitString(file, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (base::StartsWith(line, "IBUS_ADDRESS="))
      return line.substr(sizeof("IBUS_ADDRESS=") - 1);
  }
  return std::string();
}

// The string of an IBusText, a variant of (s a{sv} s v): its second string.
std::string TextOf(DBusMessageIter* variant) {
  if (dbus_message_iter_get_arg_type(variant) != DBUS_TYPE_VARIANT)
    return std::string();
  DBusMessageIter inner;
  dbus_message_iter_recurse(variant, &inner);
  if (dbus_message_iter_get_arg_type(&inner) != DBUS_TYPE_STRUCT)
    return std::string();
  DBusMessageIter fields;
  dbus_message_iter_recurse(&inner, &fields);
  int strings = 0;
  do {
    if (dbus_message_iter_get_arg_type(&fields) == DBUS_TYPE_STRING && ++strings == 2) {
      const char* text = nullptr;
      dbus_message_iter_get_basic(&fields, &text);
      return text ? text : std::string();
    }
  } while (dbus_message_iter_next(&fields));
  return std::string();
}

class IBusContext;

// The connection to IBus's bus, shared by the windows' contexts.
class IBus : public base::MessagePumpForUI::FdWatcher {
 public:
  static IBus& Get() {
    static base::NoDestructor<IBus> bus;
    return *bus;
  }

  // Connected, trying again when it is not: an input method started after the
  // application is found at a later focus, not asked for at every key.
  bool Connected() {
    if (connection_ && dbus_connection_get_is_connected(connection_))
      return true;
    Drop();
    const base::TimeTicks now = base::TimeTicks::Now();
    if (!last_try_.is_null() && now - last_try_ < base::Seconds(5))
      return false;
    last_try_ = now;
    const std::string address = BusAddress();
    if (address.empty())
      return false;
    DBusError error;
    dbus_error_init(&error);
    connection_ = dbus_connection_open_private(address.c_str(), &error);
    if (!connection_ || !dbus_bus_register(connection_, &error)) {
      dbus_error_free(&error);
      Drop();
      return false;
    }
    dbus_connection_set_exit_on_disconnect(connection_, false);
    dbus_connection_add_filter(connection_, &IBus::Filter, this, nullptr);
    // ibus-daemon sends a context's signals to its client alone; a bus that
    // broadcasts them (fcitx5's frontend on the session bus) wants a match.
    dbus_bus_add_match(connection_, "type='signal',interface='org.freedesktop.IBus.InputContext'", nullptr);
    int fd = -1;
    if (dbus_connection_get_unix_fd(connection_, &fd) && fd >= 0) {
      base::CurrentUIThread::Get()->WatchFileDescriptor(fd, true, base::MessagePumpForUI::WATCH_READ, &watch_,
                                                       this);
    }
    return true;
  }

  // A new input context's path, or nothing.
  std::string CreateContext() {
    if (!Connected())
      return std::string();
    DBusMessage* call = dbus_message_new_method_call(kService, kPath, kService, "CreateInputContext");
    const char* name = "zebkit";
    dbus_message_append_args(call, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(connection_, call, 2000, nullptr);
    dbus_message_unref(call);
    std::string path;
    if (reply) {
      const char* made = nullptr;
      if (dbus_message_get_args(reply, nullptr, DBUS_TYPE_OBJECT_PATH, &made, DBUS_TYPE_INVALID) && made)
        path = made;
      dbus_message_unref(reply);
    }
    return path;
  }

  // A call nobody waits for. @append adds the arguments.
  template <typename Append>
  void Send(const std::string& path, const char* method, Append append) {
    if (path.empty() || !connection_ || !dbus_connection_get_is_connected(connection_))
      return;
    DBusMessage* call = dbus_message_new_method_call(kService, path.c_str(), kContext, method);
    append(call);
    dbus_message_set_no_reply(call, true);
    dbus_connection_send(connection_, call, nullptr);
    dbus_connection_flush(connection_);
    dbus_message_unref(call);
  }

  // Whether the input method took the key. What it says while deciding (the
  // text to commit) is with the caller when this returns.
  bool ProcessKey(const std::string& path, uint32_t keyval, uint32_t keycode, uint32_t state) {
    if (path.empty() || !Connected())
      return false;
    DBusMessage* call = dbus_message_new_method_call(kService, path.c_str(), kContext, "ProcessKeyEvent");
    dbus_message_append_args(call, DBUS_TYPE_UINT32, &keyval, DBUS_TYPE_UINT32, &keycode, DBUS_TYPE_UINT32, &state,
                             DBUS_TYPE_INVALID);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(connection_, call, kKeyTimeoutMs, nullptr);
    dbus_message_unref(call);
    dbus_bool_t handled = false;
    if (reply) {
      dbus_message_get_args(reply, nullptr, DBUS_TYPE_BOOLEAN, &handled, DBUS_TYPE_INVALID);
      dbus_message_unref(reply);
    }
    Dispatch();
    return handled;
  }

  void Add(IBusContext* context) { contexts_.insert(context); }
  void Remove(IBusContext* context) { contexts_.erase(context); }

  // base::MessagePumpForUI::FdWatcher
  void OnFileCanReadWithoutBlocking(int fd) override {
    if (!connection_)
      return;
    dbus_connection_read_write(connection_, 0);
    Dispatch();
    if (connection_ && !dbus_connection_get_is_connected(connection_))
      Drop();
  }
  void OnFileCanWriteWithoutBlocking(int fd) override {}

 private:
  friend class base::NoDestructor<IBus>;
  IBus() = default;

  void Dispatch() {
    while (connection_ && dbus_connection_dispatch(connection_) == DBUS_DISPATCH_DATA_REMAINS) {
    }
  }

  void Drop() {
    watch_.StopWatchingFileDescriptor();
    if (connection_) {
      dbus_connection_remove_filter(connection_, &IBus::Filter, this);
      dbus_connection_close(connection_);
      dbus_connection_unref(connection_);
      connection_ = nullptr;
    }
    Lost();
  }

  void Lost();
  static DBusHandlerResult Filter(DBusConnection* connection, DBusMessage* message, void* data);

  raw_ptr<DBusConnection> connection_ = nullptr;
  base::MessagePumpForUI::FdWatchController watch_{FROM_HERE};
  base::TimeTicks last_try_;
  std::set<raw_ptr<IBusContext, SetExperimental>> contexts_;
};

class IBusContext : public ui::LinuxInputMethodContext {
 public:
  explicit IBusContext(ui::LinuxInputMethodContextDelegate* delegate) : delegate_(delegate) { IBus::Get().Add(this); }
  ~IBusContext() override {
    IBus::Get().Remove(this);
    IBus::Get().Send(path_, "Destroy", [](DBusMessage*) {});
  }

  const std::string& path() const { return path_; }
  void Forget() {
    path_.clear();
    if (composing_)
      EndComposition();
  }

  // ui::LinuxInputMethodContext
  bool DispatchKeyEvent(const ui::KeyEvent& event) override {
    if (!focused_ || !Ready())
      return false;
    uint32_t state = 0;
    const int flags = event.flags();
    if (flags & ui::EF_SHIFT_DOWN) state |= kShift;
    if (flags & ui::EF_CAPS_LOCK_ON) state |= kCapsLock;
    if (flags & ui::EF_CONTROL_DOWN) state |= kControl;
    if (flags & ui::EF_ALT_DOWN) state |= kAlt;
    if (flags & ui::EF_NUM_LOCK_ON) state |= kNumLock;
    if (flags & ui::EF_COMMAND_DOWN) state |= kSuper;
    if (event.type() == ui::EventType::kKeyReleased) state |= kRelease;
    // The key as X11 names it: a character's own keysym (Latin-1 as it is,
    // the rest as Unicode keysyms), a named key's by its key code.
    uint32_t keyval = 0;
    const ui::DomKey key = event.GetDomKey();
    // Enter, Tab, Backspace and Escape are characters to Chromium (control
    // characters) and named keys to X11.
    const uint32_t character = key.IsCharacter() ? key.ToCharacter() : 0;
    if (character >= 0x20 && character != 0x7f) {
      keyval = character <= 0x7e || (character >= 0xa0 && character <= 0xff) ? character : 0x01000000 | character;
    } else {
#if defined(ZEBKIT_X11)
      keyval = static_cast<uint32_t>(ui::XKeysymForWindowsKeyCode(event.key_code(), event.IsShiftDown()));
#endif
    }
    // X11's key code is the kernel's plus 8; IBus takes the kernel's.
    const int native = ui::KeycodeConverter::DomCodeToNativeKeycode(event.code());
    const uint32_t keycode = native >= 8 ? static_cast<uint32_t>(native - 8) : 0;
    return IBus::Get().ProcessKey(path_, keyval, keycode, state);
  }

  bool IsPeekKeyEvent(const ui::KeyEvent& event) override { return false; }

  // The caret, in screen DIPs: where the input method puts its candidates,
  // in the screen's pixels.
  void SetCursorLocation(const gfx::Rect& rect) override {
    caret_ = rect;
    if (!focused_ || path_.empty())
      return;
    float scale = 1;
    if (display::Screen* screen = display::Screen::Get())
      scale = screen->GetDisplayMatching(rect).device_scale_factor();
    const int32_t x = static_cast<int32_t>(rect.x() * scale), y = static_cast<int32_t>(rect.y() * scale),
                  width = static_cast<int32_t>(rect.width() * scale),
                  height = static_cast<int32_t>(rect.height() * scale);
    IBus::Get().Send(path_, "SetCursorLocation", [&](DBusMessage* call) {
      dbus_message_append_args(call, DBUS_TYPE_INT32, &x, DBUS_TYPE_INT32, &y, DBUS_TYPE_INT32, &width,
                               DBUS_TYPE_INT32, &height, DBUS_TYPE_INVALID);
    });
  }

  void SetSurroundingText(const std::u16string& text, const gfx::Range& text_range,
                          const gfx::Range& composition_range, const gfx::Range& selection_range) override {}

  void Reset() override {
    IBus::Get().Send(path_, "Reset", [](DBusMessage*) {});
    composing_ = false;
  }

  void UpdateFocus(bool has_client, ui::TextInputType old_type, const TextInputClientAttributes& attributes,
                   ui::TextInputClient::FocusReason reason) override {
    // Not for what is not text, and not for passwords, which no input method
    // is to see or to learn.
    const bool wanted = has_client && attributes.input_type != ui::TEXT_INPUT_TYPE_NONE &&
                        attributes.input_type != ui::TEXT_INPUT_TYPE_PASSWORD;
    if (wanted == focused_)
      return;
    focused_ = wanted;
    if (focused_) {
      if (!Ready()) {
        return;
      }
      IBus::Get().Send(path_, "FocusIn", [](DBusMessage*) {});
      SetCursorLocation(caret_);
    } else {
      IBus::Get().Send(path_, "FocusOut", [](DBusMessage*) {});
      if (composing_)
        EndComposition();
    }
  }

  ui::VirtualKeyboardController* GetVirtualKeyboardController() override { return nullptr; }

  // What IBus says to this context.
  void Signal(DBusMessage* message) {
    DBusMessageIter args;
    const bool has_args = dbus_message_iter_init(message, &args);
    if (dbus_message_is_signal(message, kContext, "CommitText")) {
      if (!has_args)
        return;
      const std::string text = TextOf(&args);
      if (composing_)
        EndComposition();
      if (!text.empty())
        delegate_->OnCommit(base::UTF8ToUTF16(text));
    } else if (dbus_message_is_signal(message, kContext, "UpdatePreeditText") ||
               dbus_message_is_signal(message, kContext, "UpdatePreeditTextWithMode")) {
      if (!has_args)
        return;
      const std::string text = TextOf(&args);
      uint32_t cursor = 0;
      dbus_bool_t visible = true;
      if (dbus_message_iter_next(&args) && dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_UINT32)
        dbus_message_iter_get_basic(&args, &cursor);
      if (dbus_message_iter_next(&args) && dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_BOOLEAN)
        dbus_message_iter_get_basic(&args, &visible);
      preedit_ = base::UTF8ToUTF16(text);
      // IBus counts the cursor in characters; Chromium in UTF-16 units.
      size_t units = 0;
      for (uint32_t i = 0; i < cursor && units < preedit_.size(); ++i)
        units += (preedit_[units] >= 0xd800 && preedit_[units] <= 0xdbff && units + 1 < preedit_.size()) ? 2 : 1;
      preedit_cursor_ = units;
      if (visible && !preedit_.empty())
        ShowComposition();
      else if (composing_)
        EndComposition();
    } else if (dbus_message_is_signal(message, kContext, "ShowPreeditText")) {
      if (!preedit_.empty())
        ShowComposition();
    } else if (dbus_message_is_signal(message, kContext, "HidePreeditText")) {
      if (composing_)
        EndComposition();
    } else if (dbus_message_is_signal(message, kContext, "DeleteSurroundingText")) {
      int32_t offset = 0;
      uint32_t count = 0;
      if (dbus_message_get_args(message, nullptr, DBUS_TYPE_INT32, &offset, DBUS_TYPE_UINT32, &count,
                                DBUS_TYPE_INVALID) &&
          offset <= 0) {
        const size_t before = static_cast<size_t>(-offset);
        delegate_->OnDeleteSurroundingText(before, count > before ? count - before : 0);
      }
    }
  }

 private:
  // An input context of IBus's, made when first needed and again after the
  // connection was lost.
  bool Ready() {
    if (!path_.empty() && IBus::Get().Connected())
      return true;
    path_ = IBus::Get().CreateContext();
    if (path_.empty())
      return false;
    const uint32_t capabilities = kCapabilities;
    IBus::Get().Send(path_, "SetCapabilities", [&](DBusMessage* call) {
      dbus_message_append_args(call, DBUS_TYPE_UINT32, &capabilities, DBUS_TYPE_INVALID);
    });
    return true;
  }

  void ShowComposition() {
    if (!composing_) {
      composing_ = true;
      delegate_->OnPreeditStart();
    }
    ui::CompositionText composition;
    composition.text = preedit_;
    composition.selection = gfx::Range(preedit_cursor_);
    composition.ime_text_spans.push_back(ui::ImeTextSpan(
        ui::ImeTextSpan::Type::kComposition, 0, preedit_.size(), ui::ImeTextSpan::Thickness::kThin,
        ui::ImeTextSpan::UnderlineStyle::kSolid, SK_ColorTRANSPARENT));
    delegate_->OnPreeditChanged(composition);
  }

  void EndComposition() {
    composing_ = false;
    preedit_.clear();
    delegate_->OnPreeditEnd();
  }

  raw_ptr<ui::LinuxInputMethodContextDelegate> delegate_;
  std::string path_;
  bool focused_ = false;
  bool composing_ = false;
  std::u16string preedit_;
  size_t preedit_cursor_ = 0;
  gfx::Rect caret_;
};

void IBus::Lost() {
  for (IBusContext* context : contexts_)
    context->Forget();
}

// static
DBusHandlerResult IBus::Filter(DBusConnection* connection, DBusMessage* message, void* data) {
  auto* self = static_cast<IBus*>(data);
  if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL)
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  const char* path = dbus_message_get_path(message);
  if (!path)
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  // The context can go while it is told (a commit that closes the view).
  for (IBusContext* context : std::set<raw_ptr<IBusContext, SetExperimental>>(self->contexts_)) {
    if (self->contexts_.contains(context) && context->path() == path) {
      context->Signal(message);
      return DBUS_HANDLER_RESULT_HANDLED;
    }
  }
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

std::unique_ptr<ui::LinuxInputMethodContext> CreateContext(ui::LinuxInputMethodContextDelegate* delegate) {
  // Also without IBus's bus now: the context looks for it again when a field
  // takes focus and at keys, so an input method started later is found, and
  // until then keys are the page's, as with Chromium's fallback.
  return std::make_unique<IBusContext>(delegate);
}

}  // namespace

void InstallIBusInputMethod() {
  // Ozone's Wayland has its own (text-input); X11 has none.
  if (!ui::GetInputMethodContextFactoryForOzone())
    ui::GetInputMethodContextFactoryForOzone() = base::BindRepeating(&CreateContext);
}

}  // namespace zebkit
