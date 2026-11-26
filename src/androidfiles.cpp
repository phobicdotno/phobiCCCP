#include "androidfiles.h"

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QMetaObject>
#include <QtCore/qcoreapplication_platform.h>
#endif

namespace c2d::androidfiles {

Hub *Hub::instance()
{
    static Hub *hub = new Hub;
    return hub;
}

#ifdef Q_OS_ANDROID

namespace {

QJniObject activity()
{
    return QNativeInterface::QAndroidApplication::context();
}

// Called by PhobicActivity on its own threads: handed to the GUI thread.
void JNICALL fileArrived(JNIEnv *, jclass, jstring path, jstring name, jint purpose)
{
    const QString p = QJniObject(path).toString();
    const QString n = QJniObject(name).toString();
    const int what = int(purpose);
    QMetaObject::invokeMethod(Hub::instance(), [p, n, what] {
        emit Hub::instance()->arrived(p, n, what);
    }, Qt::QueuedConnection);
}

void JNICALL fileMessage(JNIEnv *, jclass, jstring text, jboolean error)
{
    const QString t = QJniObject(text).toString();
    const bool e = error;
    QMetaObject::invokeMethod(Hub::instance(), [t, e] {
        emit Hub::instance()->message(t, e);
    }, Qt::QueuedConnection);
}

bool registerNatives()
{
    static const bool ok = [] {
        // The hub lives on the GUI thread, so the queued calls run there.
        Hub::instance()->moveToThread(QCoreApplication::instance()->thread());
        const JNINativeMethod methods[] = {
            {"fileArrived", "(Ljava/lang/String;Ljava/lang/String;I)V", reinterpret_cast<void *>(fileArrived)},
            {"fileMessage", "(Ljava/lang/String;Z)V", reinterpret_cast<void *>(fileMessage)},
        };
        QJniEnvironment env;
        return env.registerNativeMethods("no/phobic/phobicccp/PhobicActivity", methods, 2);
    }();
    return ok;
}

} // namespace

bool available()
{
    return true;
}

void start()
{
    if (registerNatives())
        activity().callMethod<void>("nativeReady", "()V");
}

void pick(Purpose purpose)
{
    if (registerNatives())
        activity().callMethod<void>("pickFile", "(I)V", jint(purpose));
}

void share(const QStringList &paths, const QString &mimeType, const QString &title)
{
    if (registerNatives())
        activity().callMethod<void>("shareFiles", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
                                    QJniObject::fromString(paths.join(QLatin1Char('\n'))).object<jstring>(),
                                    QJniObject::fromString(mimeType).object<jstring>(),
                                    QJniObject::fromString(title).object<jstring>());
}

void saveCopy(const QString &path, const QString &mimeType, const QString &suggestedName)
{
    if (registerNatives())
        activity().callMethod<void>("saveCopy", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
                                    QJniObject::fromString(path).object<jstring>(),
                                    QJniObject::fromString(mimeType).object<jstring>(),
                                    QJniObject::fromString(suggestedName).object<jstring>());
}

QString oldPadDesignsDir()
{
    // Flutter's getApplicationSupportDirectory() is the app's files folder.
    const QJniObject dir = activity().callObjectMethod("getFilesDir", "()Ljava/io/File;");
    if (!dir.isValid())
        return {};
    return dir.callObjectMethod("getAbsolutePath", "()Ljava/lang/String;").toString()
           + QStringLiteral("/designs");
}

#else

bool available() { return false; }
void start() {}
void pick(Purpose) {}
void share(const QStringList &, const QString &, const QString &) {}
void saveCopy(const QString &, const QString &, const QString &) {}
QString oldPadDesignsDir() { return {}; }

#endif

} // namespace c2d::androidfiles
