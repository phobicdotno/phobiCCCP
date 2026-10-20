#pragma once
// Stand-in for Qt Serial Port where it is not available (Android, iPad): no
// ports are ever listed and opening one always fails, so the Machine panel
// says there is nothing to connect to. A pad sends its jobs to a computer
// running phobiCCCP instead.
#include <QIODevice>
#include <QString>
#include <QStringList>

class QSerialPort : public QIODevice
{
    Q_OBJECT
public:
    enum SerialPortError { NoError, DeviceNotFoundError, PermissionError, OpenError, NotOpenError,
                           WriteError, ReadError, ResourceError, UnsupportedOperationError,
                           UnknownError, TimeoutError };
    QSerialPort(const QString &, QObject *parent = nullptr) : QIODevice(parent) {}
    bool setBaudRate(int) { return true; }
    bool open(OpenMode) override
    {
        setErrorString(QStringLiteral("this device has no serial ports"));
        return false;
    }
    bool isSequential() const override { return true; }

signals:
    void errorOccurred(QSerialPort::SerialPortError error);

protected:
    qint64 readData(char *, qint64) override { return -1; }
    qint64 writeData(const char *, qint64) override { return -1; }
};

class QSerialPortInfo
{
public:
    static QList<QSerialPortInfo> availablePorts() { return {}; }
    QString portName() const { return {}; }
    QString description() const { return {}; }
    QString manufacturer() const { return {}; }
};
