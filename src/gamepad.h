#pragma once
#include <QObject>
#include <QSet>
#include <QString>

class QSocketNotifier;
class QTimer;

namespace c2d {

// A USB gamepad, read straight from the Linux joystick device.
//
// Deliberately the /dev/input/js* API rather than evdev: it is eight bytes per
// event with no library behind it, and on a stock desktop the js nodes are
// world-readable while the event nodes are not (root:input). That means a SNES
// clone works when it is plugged in, with no udev rule and no group to join.
//
// The device is found by scanning js0..js31 and is watched for: unplugging is
// not an error, and plugging in later is picked up without a restart. Anything
// with fewer than two buttons is skipped -- a MacBook's `applesmc` motion
// sensor also presents as a joystick, with two axes and no buttons, and jogging
// a machine from the lid accelerometer would be a memorable way to lose a part.
class Gamepad : public QObject
{
    Q_OBJECT
public:
    explicit Gamepad(QObject *parent = nullptr);
    ~Gamepad() override;

    bool isConnected() const { return m_fd >= 0; }
    QString deviceName() const { return m_name; }   // as the device reports it
    QString devicePath() const { return m_path; }

    // Watch for a device and keep watching. Safe to call more than once.
    void start();
    void stop();

    // Open one specific device instead of scanning. Used by the tests, which
    // point it at a pipe carrying synthetic events.
    bool openPath(const QString &path, bool checkButtons = true);

signals:
    void connected(const QString &name);
    void disconnected();
    // `pressed` is the new state; `number` is the pad's own button numbering.
    void buttonChanged(int number, bool pressed);
    // `value` is the raw axis reading, -32767..32767.
    void axisChanged(int number, int value);

private:
    void poll();          // look for a device to open
    void readEvents();    // drain everything the device has queued
    void closeDevice();

    int m_fd = -1;
    // Nodes already inspected and turned down (a MacBook's accelerometer, a
    // steering wheel, anything with fewer than two buttons). Without this the
    // rescan reopens and re-interrogates them every second and a half for as
    // long as the application runs. Cleared when a node disappears, so the
    // same number being reused by a real pad is picked up.
    QSet<QString> m_rejected;
    QString m_name;
    QString m_path;
    QSocketNotifier *m_notifier = nullptr;
    QTimer *m_scan = nullptr;
};

} // namespace c2d
