#include "gamepad.h"

#include <QDir>
#include <QFile>
#include <QSocketNotifier>
#include <QTimer>

#include <cerrno>
#include <cstring>

#ifdef __linux__
#include <fcntl.h>
#include <linux/joystick.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace c2d {

namespace {
constexpr int kScanIntervalMs = 1500;   // hotplug poll; nothing is urgent here
constexpr int kMaxJsNodes = 32;
}

Gamepad::Gamepad(QObject *parent)
    : QObject(parent), m_scan(new QTimer(this))
{
    m_scan->setInterval(kScanIntervalMs);
    connect(m_scan, &QTimer::timeout, this, &Gamepad::poll);
}

Gamepad::~Gamepad()
{
    closeDevice();
}

void Gamepad::start()
{
    poll();                 // try immediately, then keep looking
    m_scan->start();
}

void Gamepad::stop()
{
    m_scan->stop();
    closeDevice();
}

void Gamepad::closeDevice()
{
#ifdef __linux__
    if (m_notifier) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
        m_name.clear();
        m_path.clear();
        emit disconnected();
    }
#endif
}

bool Gamepad::openPath(const QString &path, bool checkButtons)
{
#ifdef __linux__
    if (m_fd >= 0)
        return false;
    const int fd = ::open(path.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return false;

    QString name = QStringLiteral("gamepad");
    if (checkButtons) {
        // A joystick with no buttons is not a gamepad. The `applesmc` lid
        // accelerometer on a MacBook is exactly that, and it is usually js0.
        unsigned char buttons = 0;
        if (::ioctl(fd, JSIOCGBUTTONS, &buttons) < 0 || buttons < 2) {
            ::close(fd);
            return false;
        }
        char buf[128] = {0};
        if (::ioctl(fd, JSIOCGNAME(sizeof(buf) - 1), buf) >= 0 && buf[0])
            name = QString::fromLocal8Bit(buf).trimmed();
    }

    m_fd = fd;
    m_path = path;
    m_name = name;
    m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &Gamepad::readEvents);
    emit connected(m_name);
    return true;
#else
    Q_UNUSED(path);
    Q_UNUSED(checkButtons);
    return false;
#endif
}

void Gamepad::poll()
{
#ifdef __linux__
    if (m_fd >= 0)
        return;
    for (int i = 0; i < kMaxJsNodes; ++i) {
        const QString path = QStringLiteral("/dev/input/js%1").arg(i);
        if (!QFile::exists(path)) {
            m_rejected.remove(path);      // gone; judge it afresh if it returns
            continue;
        }
        if (m_rejected.contains(path))
            continue;
        if (openPath(path))
            return;
        m_rejected.insert(path);
    }
#endif
}

void Gamepad::readEvents()
{
#ifdef __linux__
    if (m_fd < 0)
        return;
    js_event e;
    for (;;) {
        const ssize_t n = ::read(m_fd, &e, sizeof(e));
        if (n == sizeof(e)) {
            // JS_EVENT_INIT events are the driver reporting the resting state
            // when the device is opened. Acting on them would fire a button
            // press for every button the moment the pad is plugged in.
            const bool synthetic = (e.type & JS_EVENT_INIT) != 0;
            const int type = e.type & ~JS_EVENT_INIT;
            if (synthetic)
                continue;
            if (type == JS_EVENT_BUTTON)
                emit buttonChanged(e.number, e.value != 0);
            else if (type == JS_EVENT_AXIS)
                emit axisChanged(e.number, e.value);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;                       // drained
        if (n == 0 || n < 0) {
            // Unplugged, or the pipe a test was feeding us closed. Not an
            // error: go back to looking for a device.
            closeDevice();
            return;
        }
        // A short read of a fixed-size record: the stream is out of step and
        // there is nothing sane to do with the remainder.
        closeDevice();
        return;
    }
#endif
}

} // namespace c2d
