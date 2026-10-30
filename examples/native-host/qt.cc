// Qt owns these widgets and its event loop; no zebkit window is created.
#include "model.h"
#include <QApplication>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QImage>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <chrono>
#include <cstdio>
#include <mutex>

using Clock = std::chrono::steady_clock;
static unsigned Modifiers(Qt::KeyboardModifiers keys, Qt::MouseButtons buttons = {}) {
  unsigned flags = 0;
  if (keys & Qt::ShiftModifier) flags |= ZEBKIT_MODIFIER_SHIFT;
  if (keys & Qt::ControlModifier) flags |= ZEBKIT_MODIFIER_CONTROL;
  if (keys & Qt::AltModifier) flags |= ZEBKIT_MODIFIER_ALT;
  if (keys & Qt::MetaModifier) flags |= ZEBKIT_MODIFIER_META;
  if (buttons & Qt::LeftButton) flags |= ZEBKIT_MODIFIER_LEFT_BUTTON;
  if (buttons & Qt::MiddleButton) flags |= ZEBKIT_MODIFIER_MIDDLE_BUTTON;
  if (buttons & Qt::RightButton) flags |= ZEBKIT_MODIFIER_RIGHT_BUTTON;
  return flags;
}
class Pane : public QWidget {
 public:
  Pane(native_host::Model& model, bool document) : model_(model), document_(document) {
    setFocusPolicy(document ? Qt::NoFocus : Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_InputMethodEnabled, !document);
  }
  QRect caret;
 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.fillRect(rect(), document_ ? QColor(220, 220, 220) : Qt::white);
    if (document_) {
      const auto& image = model_.document;
      if (image.bgra.empty()) return;
      QSize size(image.width, image.height);
      size.scale(this->size() - QSize(20, 20), Qt::KeepAspectRatio);
      QRect target(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
      painter.drawImage(target, QImage(image.bgra.data(), image.width, image.height, image.width * 4,
                                     QImage::Format_ARGB32));
      return;
    }
    auto paint = [&](const native_host::Image& image) {
      if (image.bgra.empty()) return;
      painter.drawImage(QRectF(image.bounds.x, image.bounds.y, image.bounds.width, image.bounds.height),
        QImage(image.bgra.data(), image.width, image.height, image.width * 4, QImage::Format_ARGB32_Premultiplied));
    };
    paint(model_.web);
    if (model_.popup_shown) paint(model_.popup);
  }
  void resizeEvent(QResizeEvent*) override {
    if (!document_ && model_.view)
      zebkit_view_set_size(model_.view, std::max(1, width()), std::max(1, height()), devicePixelRatioF());
  }
  void mouse(QMouseEvent* event, zebkit_mouse_type type, int count = 0) {
    if (document_ || !model_.view) return;
    if (type == ZEBKIT_MOUSE_DOWN) setFocus(Qt::MouseFocusReason);
    auto button = event->button() == Qt::LeftButton ? ZEBKIT_BUTTON_LEFT :
                  event->button() == Qt::MiddleButton ? ZEBKIT_BUTTON_MIDDLE :
                  event->button() == Qt::RightButton ? ZEBKIT_BUTTON_RIGHT : ZEBKIT_BUTTON_NONE;
    zebkit_view_send_mouse(model_.view, type, event->position().x(), event->position().y(), button,
                          count, Modifiers(event->modifiers(), event->buttons()));
  }
  void mousePressEvent(QMouseEvent* event) override { mouse(event, ZEBKIT_MOUSE_DOWN, 1); }
  void mouseReleaseEvent(QMouseEvent* event) override { mouse(event, ZEBKIT_MOUSE_UP, 1); }
  void mouseDoubleClickEvent(QMouseEvent* event) override { mouse(event, ZEBKIT_MOUSE_DOWN, 2); }
  void mouseMoveEvent(QMouseEvent* event) override { mouse(event, ZEBKIT_MOUSE_MOVE); }
  void leaveEvent(QEvent*) override {
    if (!document_ && model_.view)
      zebkit_view_send_mouse(model_.view, ZEBKIT_MOUSE_LEAVE, 0, 0, ZEBKIT_BUTTON_NONE, 0, 0);
  }
  void wheelEvent(QWheelEvent* event) override {
    if (document_) { model_.Turn(event->angleDelta().y() < 0 ? 1 : -1); return; }
    if (!model_.view) return;
    const bool precise = !event->pixelDelta().isNull();
    const QPointF delta = precise ? QPointF(event->pixelDelta()) : QPointF(event->angleDelta()) / 3;
    zebkit_view_send_wheel(model_.view, event->position().x(), event->position().y(), -delta.x(), -delta.y(),
                          precise, ZEBKIT_WHEEL_PHASE_NONE, Modifiers(event->modifiers(), event->buttons()));
  }
  void focusInEvent(QFocusEvent*) override { if (!document_ && model_.view) zebkit_view_set_focus(model_.view, true); }
  void focusOutEvent(QFocusEvent*) override { if (!document_ && model_.view) zebkit_view_set_focus(model_.view, false); }
  void key(QKeyEvent* event, zebkit_key_type type) {
    if (document_ || !model_.view) return;
    int code = event->key() < 128 ? event->key() : 0;
    switch (event->key()) {
      case Qt::Key_Return: case Qt::Key_Enter: code = 13; break;
      case Qt::Key_Backspace: code = 8; break; case Qt::Key_Tab: code = 9; break;
      case Qt::Key_Escape: code = 27; break; case Qt::Key_Left: code = 37; break;
      case Qt::Key_Up: code = 38; break; case Qt::Key_Right: code = 39; break;
      case Qt::Key_Down: code = 40; break; case Qt::Key_Delete: code = 46; break;
      case Qt::Key_Home: code = 36; break; case Qt::Key_End: code = 35; break;
      case Qt::Key_PageUp: code = 33; break; case Qt::Key_PageDown: code = 34; break;
    }
    zebkit_key_event key{};
    key.type = type; key.key_code = code; key.native_key_code = event->nativeScanCode();
    key.modifiers = Modifiers(event->modifiers()) | (event->isAutoRepeat() ? ZEBKIT_MODIFIER_AUTO_REPEAT : 0);
    zebkit_view_send_key(model_.view, &key);
    if (type == ZEBKIT_KEY_DOWN && !(event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) &&
        !event->text().isEmpty() && code != 8 && code != 9 && code != 13 && code != 27 && code != 46)
      zebkit_view_ime_commit(model_.view, event->text().toUtf8().constData());
    event->accept();
  }
  // Tab is the page's, not Qt's focus chain.
  bool focusNextPrevChild(bool) override { return false; }
  void keyPressEvent(QKeyEvent* event) override { key(event, ZEBKIT_KEY_DOWN); }
  void keyReleaseEvent(QKeyEvent* event) override { key(event, ZEBKIT_KEY_UP); }
  void inputMethodEvent(QInputMethodEvent* event) override {
    if (!model_.view || document_) return;
    if (!event->commitString().isEmpty())
      zebkit_view_ime_commit(model_.view, event->commitString().toUtf8().constData());
    if (event->preeditString().isEmpty()) zebkit_view_ime_cancel(model_.view);
    else {
      int cursor = event->preeditString().size();
      for (const auto& attribute : event->attributes())
        if (attribute.type == QInputMethodEvent::Cursor) cursor = attribute.start;
      zebkit_view_ime_set_composition(model_.view, event->preeditString().toUtf8().constData(), cursor, cursor);
    }
    event->accept();
  }
  QVariant inputMethodQuery(Qt::InputMethodQuery query) const override {
    if (query == Qt::ImCursorRectangle) return caret;
    if (query == Qt::ImEnabled) return !document_ && model_.view;
    return QWidget::inputMethodQuery(query);
  }
 private:
  native_host::Model& model_;
  bool document_;
};

class Host : public QWidget {
 public:
  native_host::Model model;
  Pane *web, *pdf;
  QTimer timer;
  std::mutex mutex;
  Clock::time_point deadline = Clock::time_point::max();
  bool queued = false, active = true;
  int pumps = 0;
  Host() {
    setWindowTitle("zebkit — Qt host window"); resize(1200, 740);
    auto* layout = new QVBoxLayout(this);
    auto* toolbar = new QHBoxLayout;
    layout->addLayout(toolbar);
    for (int delta : {-1, 1}) {
      auto* button = new QPushButton(delta < 0 ? "Previous PDF page" : "Next PDF page");
      toolbar->addWidget(button);
      connect(button, &QPushButton::clicked, this, [this, delta] { model.Turn(delta); });
    }
    auto* split = new QSplitter;
    layout->addWidget(split);
    web = new Pane(model, false); pdf = new Pane(model, true);
    split->addWidget(web); split->addWidget(pdf); split->setSizes({600, 600});
    model.redraw = [this] { web->update(); pdf->update(); };
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, this, [this] { Dispatch(); });
  }
  static void Wake(int64_t delay, void* data) {
    auto& host = *static_cast<Host*>(data);
    std::lock_guard<std::mutex> lock(host.mutex);
    if (!host.active) return;
    host.deadline = std::min(host.deadline, Clock::now() + std::chrono::milliseconds(delay));
    if (!host.queued) {
      host.queued = true;
      QMetaObject::invokeMethod(&host, [&host] { host.Dispatch(); }, Qt::QueuedConnection);
    }
  }
  void Dispatch() {
    Clock::time_point due;
    {
      std::lock_guard<std::mutex> lock(mutex);
      queued = false;
      if (!active) return;
      due = deadline;
      if (due <= Clock::now()) deadline = Clock::time_point::max();
    }
    timer.stop();
    if (due == Clock::time_point::max()) return;
    const auto delay = std::chrono::ceil<std::chrono::milliseconds>(due - Clock::now()).count();
    if (delay > 0) timer.start(static_cast<int>(std::min<int64_t>(delay, INT_MAX)));
    else { ++pumps; zebkit_pump(); }
  }
};
int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  std::string url, pdf, smoke;
  bool print_bounds = false;
  for (int i = 0; i < argc; ++i) {
    arguments.emplace_back(argv[i]);
    if (arguments.back() == "--print-bounds") print_bounds = true;
    if (arguments.back().rfind("--url=", 0) == 0) url = arguments.back().substr(6);
    if (arguments.back().rfind("--pdf=", 0) == 0) pdf = arguments.back().substr(6);
    if (arguments.back().rfind("--smoke-test=", 0) == 0) smoke = arguments.back().substr(13);
  }
  std::vector<char*> engine_args;
  for (auto& argument : arguments) engine_args.push_back(argument.data());
  QApplication app(argc, argv);
  Host host;
  zebkit_set_wakeup(&Host::Wake, &host);
  char* error = nullptr;
  if (zebkit_init_with_settings(engine_args.size(), engine_args.data(), "{\"headless\":true}", &error)) {
    std::fprintf(stderr, "%s\n", error ? error : "Engine startup failed"); zebkit_free(error); return 1;
  }
  try { host.model.Open(url, pdf); }
  catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); host.model.Close(); zebkit_shutdown(); return 1; }
  zebkit_view_on_caret(host.model.view, [](zebkit_view*, const zebkit_rect* caret, void* data) {
    auto* pane = static_cast<Pane*>(data);
    if (caret) pane->caret = QRect(caret->x, caret->y, caret->width, caret->height);
    QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle);
  }, host.web);
  zebkit_view_on_cursor(host.model.view, [](zebkit_view*, const char* name, const zebkit_cursor_image*, void* data) {
    const std::string cursor(name);
    Qt::CursorShape shape = cursor == "pointer" ? Qt::PointingHandCursor : cursor == "text" ? Qt::IBeamCursor :
      cursor == "wait" ? Qt::WaitCursor : cursor == "crosshair" ? Qt::CrossCursor : cursor == "none" ? Qt::BlankCursor :
      cursor == "ew-resize" ? Qt::SizeHorCursor : cursor == "ns-resize" ? Qt::SizeVerCursor : Qt::ArrowCursor;
    static_cast<Pane*>(data)->setCursor(shape);
  }, host.web);
  host.show(); Host::Wake(0, &host);
  if (print_bounds) {
    const QPoint point = host.web->mapTo(&host, QPoint());
    std::printf("host-web-bounds: %d %d %.2f\n", point.x(), point.y(), host.web->devicePixelRatioF());
    std::fflush(stdout);
  }
  bool captured = smoke.empty();
  QTimer smoke_timer;
  if (!smoke.empty()) {
    smoke_timer.setInterval(1500);
    QObject::connect(&smoke_timer, &QTimer::timeout, &host, [&] {
      if (!host.model.frames) return;
      captured = host.grab().save(QString::fromStdString(smoke));
      app.quit();
    });
    smoke_timer.start();
  }
  app.exec();
  { std::lock_guard<std::mutex> lock(host.mutex); host.active = false; }
  host.timer.stop(); host.model.Close();
  const bool stopped = zebkit_shutdown();
  std::printf("Qt host: %d frames, %d event-driven pumps, shutdown=%s\n", host.model.frames, host.pumps, stopped ? "ok" : "failed");
  return stopped && captured && host.model.frames > 0 ? 0 : 1;
}
