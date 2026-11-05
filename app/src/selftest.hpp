// End-to-end self-test: drives the real window with synthetic mouse/keyboard
// events through the M1 acceptance flow (magnet base by UI), takes screenshots
// and writes report.json. Used by ctest under Xvfb + Lavapipe and on Windows.
#pragma once
#include <QElapsedTimer>
#include <QJsonArray>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QTimer>

#include <functional>
#include <vector>

class QQuickItem;
class QQuickWindow;

namespace anvil {

class AppController;
class ViewportItem;

class SelfTest : public QObject {
    Q_OBJECT
  public:
    SelfTest(QQuickWindow* w, ViewportItem* v, AppController* c, QString out_dir);
    int exitCode() const { return failed_ ? 1 : 0; }

  private:
    struct Step {
        QString name;
        std::function<void()> act;
        std::function<bool()> done;  // polled until true (timeout = failure)
        int timeout_ms = 8000;
    };
    void add(QString name, std::function<void()> act, std::function<bool()> done = {}, int timeout = 8000);
    void tick();
    void finish();

    // input helpers (scene coordinates in logical pixels)
    QQuickItem* item(const QString& objectName) const;
    QPointF center(QQuickItem* it) const;
    QPointF project(double x, double y, double z) const;
    void mouseMove(QPointF p, Qt::MouseButtons held = Qt::NoButton);
    void mousePress(QPointF p, Qt::MouseButton b, Qt::KeyboardModifiers m = Qt::NoModifier);
    void mouseRelease(QPointF p, Qt::MouseButton b, Qt::KeyboardModifiers m = Qt::NoModifier);
    void click(QPointF p, Qt::KeyboardModifiers m = Qt::NoModifier);
    void clickItem(const QString& objectName, Qt::KeyboardModifiers m = Qt::NoModifier);
    void typeInto(const QString& objectName, const QString& text);
    void key(int k, Qt::KeyboardModifiers m = Qt::NoModifier, const QString& text = {});
    void flick(QPointF from, double angle_deg, double dist, Qt::MouseButton b = Qt::RightButton);
    void screenshot(const QString& name);
    bool idle() const;
    bool hasObject(const QString& name) const;
    double objectValue(const QString& name, const QString& key) const;
    double vol_before_ = 0;
    void check(bool ok, const QString& what);

    QQuickWindow* win_;
    ViewportItem* view_;
    AppController* app_;
    QString out_;
    std::vector<Step> steps_;
    std::size_t cur_ = 0;
    bool started_ = false;
    QElapsedTimer step_clock_;
    QTimer timer_;
    QJsonArray log_;
    bool failed_ = false;
    int frames_ = 0;
    int radial_frames_ = -1;
    int radial_press_frame_ = -1;
};

}  // namespace anvil
