#include "mainwindow.h"
#include "importers.h"
#include "cccpfile.h"
#include "shopdialog.h"
#include "aboutdialog.h"
#include "toolstrip.h"
#include "brandlogo.h"

#include "exportprogress.h"

#include <utility>
#include "backgrounddialog.h"
#include "gcodeexport.h"
#include "isopreview.h"
#include "machinepanel.h"
#include "modelpanel.h"
#include "projectreceiver.h"
#include "machineprofiles.h"
#include "padlink.h"
#include "sendtopaddialog.h"
#include <QActionGroup>
#include <QJsonDocument>
#include <QUuid>
#include <QRandomGenerator>
#include <QScopeGuard>
#include "tiling.h"
#include <QInputDialog>
#include "parameterspanel.h"
#include "propertiespanel.h"
#include "simpanel.h"
#include "toolpathpanel.h"
#include "vectoractions.h"
#include "importui.h"
#include "setupsheet.h"
#include <QDesktopServices>
#include <QUrl>

#include <QApplication>
#include <QAbstractSpinBox>
#include <QFile>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QDateTime>
#include <QDir>
#include <QCloseEvent>
#include <QFileInfo>
#include <QScrollArea>
#include "dragscroll.h"
#include <QTabWidget>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QSplitter>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QVersionNumber>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QMenu>
#include <QMenuBar>
#include <QToolButton>
#include <functional>
#include <QSet>
#include <QSaveFile>
#include <QSettings>
#include <QCoreApplication>
#include <QProcess>
#include <QRegularExpression>
#include <QProgressBar>
#include <memory>
#include <QDialog>
#include <QPushButton>
#include <QStandardPaths>
#include "sendtodialog.h"
#include "settingsdialog.h"
#include "toolbarorder.h"
#include "toolpathfactory.h"
#include <QTimer>
#include "heightmodel.h"
#include <atomic>
#include <QThread>
#include <QLocale>
#include <QSysInfo>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QStringList>
#include <QDebug>
#include <QSqlDatabase>
#include <QActionGroup>
#include <QLineEdit>
#include <QLabel>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QIcon>
#include <QPainterPath>
#include <QPolygonF>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QToolBar>
#include <QUndoStack>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <QHash>
#include <QKeyEvent>
#include <QPointer>

namespace c2d {

// Simple flat tool icons drawn at runtime — no resource files needed.
// The sketch-tool variants (Circle > 2-Point, ...): the shape in the normal
// pen, the points the user clicks as blue dots and helper geometry (radius,
// tangent lines, the circle a polygon is drawn in) thin and blue. Returns
// false for a kind that is not a variant.
static bool drawModeIcon(QPainter &p, const QString &kind)
{
    const QColor ink(0xd8, 0xdc, 0xe4), blue(0x9f, 0xc8, 0xf2);
    const QPen shape(ink, 1.6), helper(blue, 1.0), dashed(blue, 1.0, Qt::DashLine);
    auto dots = [&](std::initializer_list<QPointF> pts) {
        p.save();
        p.setPen(Qt::NoPen);
        p.setBrush(blue);
        for (const QPointF &v : pts)
            p.drawEllipse(v, 1.7, 1.7);
        p.restore();
    };
    auto hexagon = [](double r) {
        QPolygonF hex;                 // flat top and bottom
        for (int i = 0; i < 6; ++i)
            hex << QPointF(10 + r * qCos(M_PI / 3 * i), 10 + r * qSin(M_PI / 3 * i));
        return hex;
    };
    const QRectF circle(4, 4, 12, 12);
    const QRectF slotBox(2.5, 6, 15, 8);
    p.setBrush(Qt::NoBrush);

    if (kind == "circle-center") {
        p.setPen(shape);   p.drawEllipse(circle);
        p.setPen(dashed);  p.drawLine(QLineF(10, 10, 16, 10));
        dots({QPointF(10, 10)});
    } else if (kind == "circle-2pt") {
        p.setPen(shape);   p.drawEllipse(circle);
        p.setPen(dashed);  p.drawLine(QLineF(4, 10, 16, 10));
        dots({QPointF(4, 10), QPointF(16, 10)});
    } else if (kind == "circle-3pt") {
        p.setPen(shape);   p.drawEllipse(circle);
        dots({QPointF(10, 4), QPointF(4.8, 13), QPointF(15.2, 13)});
    } else if (kind == "circle-2tan") {
        p.setPen(helper);
        p.drawLine(QLineF(3, 2, 3, 18));
        p.drawLine(QLineF(2, 17, 18, 17));
        p.setPen(shape);   p.drawEllipse(QPointF(10, 10), 7, 7);
    } else if (kind == "circle-3tan") {
        p.setPen(helper);
        p.drawPolygon(QPolygonF({QPointF(1.5, 17.5), QPointF(18.5, 17.5), QPointF(10, 2)}));
        p.setPen(shape);   p.drawEllipse(QPointF(10, 12.6), 4.9, 4.9);
    } else if (kind == "rect-2pt") {
        p.setPen(shape);   p.drawRect(QRectF(4, 5, 12, 10));
        dots({QPointF(4, 5), QPointF(16, 15)});
    } else if (kind == "rect-3pt") {
        p.setPen(shape);
        p.drawPolygon(QPolygonF({QPointF(2.5, 11), QPointF(10, 3.5), QPointF(17.5, 11),
                                 QPointF(10, 18.5)}));
        dots({QPointF(2.5, 11), QPointF(10, 3.5), QPointF(17.5, 11)});
    } else if (kind == "rect-center") {
        p.setPen(shape);   p.drawRect(QRectF(4, 5, 12, 10));
        p.setPen(dashed);  p.drawLine(QLineF(10, 10, 16, 15));
        dots({QPointF(10, 10), QPointF(16, 15)});
    } else if (kind == "poly-inscribed") {
        p.setPen(dashed);  p.drawEllipse(QPointF(10, 10), 7.5, 7.5);
        p.setPen(shape);   p.drawPolygon(hexagon(7.5));
    } else if (kind == "poly-circumscribed") {
        p.setPen(shape);   p.drawPolygon(hexagon(8));
        p.setPen(dashed);  p.drawEllipse(QPointF(10, 10), 6.9, 6.9);
    } else if (kind == "poly-edge") {
        const QPolygonF hex = hexagon(7.5);
        p.setPen(shape);   p.drawPolygon(hex);
        dots({hex[1], hex[2]});        // the bottom edge it is built on
    } else if (kind == "slot-c2c") {
        p.setPen(shape);   p.drawRoundedRect(slotBox, 4, 4);
        p.setPen(dashed);  p.drawLine(QLineF(6.5, 10, 13.5, 10));
        dots({QPointF(6.5, 10), QPointF(13.5, 10)});
    } else if (kind == "slot-overall") {
        p.setPen(shape);   p.drawRoundedRect(slotBox, 4, 4);
        p.setPen(dashed);  p.drawLine(QLineF(2.5, 10, 17.5, 10));
        dots({QPointF(2.5, 10), QPointF(17.5, 10)});
    } else if (kind == "slot-center") {
        p.setPen(shape);   p.drawRoundedRect(slotBox, 4, 4);
        p.setPen(dashed);  p.drawLine(QLineF(10, 10, 13.5, 10));
        dots({QPointF(10, 10), QPointF(13.5, 10)});
    } else if (kind == "slot-arc") {
        QPainterPath centre;
        centre.arcMoveTo(QRectF(2, 6, 16, 16), 160);
        centre.arcTo(QRectF(2, 6, 16, 16), 160, -140);
        QPainterPathStroker stroker;
        stroker.setWidth(6);
        stroker.setCapStyle(Qt::RoundCap);
        p.setPen(shape);   p.drawPath(stroker.createStroke(centre).simplified());
        p.setPen(dashed);  p.drawPath(centre);
        dots({centre.pointAtPercent(0), centre.pointAtPercent(0.5), centre.pointAtPercent(1)});
    } else if (kind == "arc-3pt") {
        p.setPen(shape);   p.drawArc(QRectF(3, 5, 14, 14), 20 * 16, 140 * 16);
        dots({QPointF(3.4, 9.6), QPointF(10, 5), QPointF(16.6, 9.6)});
    } else if (kind == "arc-center") {
        p.setPen(shape);   p.drawArc(QRectF(3, 5, 14, 14), 20 * 16, 140 * 16);
        p.setPen(dashed);
        p.drawLine(QLineF(10, 12, 16.6, 9.6));
        p.drawLine(QLineF(10, 12, 3.4, 9.6));
        dots({QPointF(10, 12), QPointF(16.6, 9.6), QPointF(3.4, 9.6)});
    } else if (kind == "arc-tangent") {
        p.setPen(shape);
        p.drawLine(QLineF(1.5, 16, 9, 16));
        p.drawArc(QRectF(3, 4, 12, 12), 270 * 16, 150 * 16);
        dots({QPointF(9, 16)});
    } else {
        return false;
    }
    return true;
}

// One glyph per toolpath kind for the Toolpaths-mode bar: what the cut
// looks like from above, amber where material goes.
static QIcon toolpathIcon(const QString &type)
{
    QPixmap pm(78, 78);
    pm.setDevicePixelRatio(3.0);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor ink(0xd8, 0xdc, 0xe4), amber(0xf2, 0xa5, 0x36);
    QPen pen(ink, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    QPen cut(amber, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const QRectF box(5, 5, 16, 16);
    if (type == QLatin1String("contour")) {
        p.drawRoundedRect(box, 3, 3);
        p.setPen(QPen(amber, 1.4, Qt::DashLine, Qt::RoundCap));
        p.drawRoundedRect(box.adjusted(-2.5, -2.5, 2.5, 2.5), 4, 4);
    } else if (type == QLatin1String("pocket_toolpath")) {
        p.drawRoundedRect(box, 3, 3);
        p.setPen(QPen(amber, 1.2));
        for (int i = 0; i < 5; ++i)
            p.drawLine(QPointF(7, 8 + i * 2.6), QPointF(19, 8 + i * 2.6));
    } else if (type == QLatin1String("drilling_toolpath")) {
        p.setBrush(amber);
        p.setPen(Qt::NoPen);
        for (QPointF c : {QPointF(8, 8), QPointF(18, 8), QPointF(8, 18), QPointF(18, 18)})
            p.drawEllipse(c, 2.6, 2.6);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(13, 10), QPointF(13, 16));
        p.drawLine(QPointF(10, 13), QPointF(16, 13));
    } else if (type == QLatin1String("texture_toolpath")) {
        p.setPen(cut);
        for (int i = 0; i < 3; ++i) {
            QPainterPath w(QPointF(4, 8 + i * 5));
            w.cubicTo(QPointF(8, 4 + i * 5), QPointF(10, 12 + i * 5), QPointF(13, 8 + i * 5));
            w.cubicTo(QPointF(16, 4 + i * 5), QPointF(18, 12 + i * 5), QPointF(22, 8 + i * 5));
            p.drawPath(w);
        }
    } else if (type == QLatin1String("advanced_vcarve_toolpath")) {
        p.setPen(QPen(ink, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(QPolygonF({QPointF(5, 5), QPointF(13, 21), QPointF(21, 5)}));
        p.setPen(QPen(amber, 1.2));
        p.drawLine(QPointF(9, 5), QPointF(13, 15));
        p.drawLine(QPointF(17, 5), QPointF(13, 15));
    } else if (type == QLatin1String("keyhole_toolpath")) {
        QPainterPath k;
        k.addEllipse(QPointF(13, 9), 5, 5);
        QPainterPath slot;
        slot.addRoundedRect(QRectF(10.5, 9, 5, 12), 2.5, 2.5);
        p.setPen(cut);
        p.drawPath(k.united(slot));
    } else if (type == QLatin1String("cutout")) {
        p.setPen(cut);
        // outline with four tab gaps
        p.drawLine(QPointF(5, 5), QPointF(11, 5));   p.drawLine(QPointF(15, 5), QPointF(21, 5));
        p.drawLine(QPointF(5, 21), QPointF(11, 21)); p.drawLine(QPointF(15, 21), QPointF(21, 21));
        p.drawLine(QPointF(5, 5), QPointF(5, 11));   p.drawLine(QPointF(5, 15), QPointF(5, 21));
        p.drawLine(QPointF(21, 5), QPointF(21, 11)); p.drawLine(QPointF(21, 15), QPointF(21, 21));
        p.setPen(QPen(ink, 1.2));
        for (QPointF c : {QPointF(13, 5), QPointF(13, 21), QPointF(5, 13), QPointF(21, 13)})
            p.drawEllipse(c, 1.3, 1.3);
    } else if (type == QLatin1String("engrave_toolpath")) {
        p.setPen(QPen(amber, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath a(QPointF(4, 20));
        a.cubicTo(QPointF(6, 4), QPointF(12, 4), QPointF(13, 20));
        a.cubicTo(QPointF(14, 10), QPointF(18, 8), QPointF(22, 18));
        p.drawPath(a);
    } else if (type == QLatin1String("face_toolpath")) {
        p.drawRoundedRect(box, 2, 2);
        p.setPen(cut);
        QPainterPath z(QPointF(7, 8));
        z.lineTo(QPointF(19, 8)); z.lineTo(QPointF(7, 11.5)); z.lineTo(QPointF(19, 11.5));
        z.lineTo(QPointF(7, 15)); z.lineTo(QPointF(19, 15)); z.lineTo(QPointF(7, 18.5));
        z.lineTo(QPointF(19, 18.5));
        p.drawPath(z);
    } else if (type == QLatin1String("bore_toolpath")) {
        p.drawEllipse(QPointF(13, 13), 8, 8);
        p.setPen(cut);
        QPainterPath sp(QPointF(13, 13));
        for (int i = 1; i <= 24; ++i) {
            const double t = i * 0.55, r = i * 0.27;
            sp.lineTo(QPointF(13 + r * std::cos(t), 13 + r * std::sin(t)));
        }
        p.drawPath(sp);
    } else if (type == QLatin1String("chamfer_toolpath")) {
        p.drawPolygon(QPolygonF({QPointF(5, 5), QPointF(21, 5), QPointF(21, 21), QPointF(5, 21)}));
        p.setPen(cut);
        p.drawLine(QPointF(5, 13), QPointF(13, 5));
        p.drawLine(QPointF(5, 17), QPointF(17, 5));
    } else if (type == QLatin1String("adaptive_toolpath")) {
        p.setPen(cut);
        QPainterPath tr(QPointF(4, 13));
        for (int i = 1; i <= 60; ++i) {
            const double t = i * 0.42;
            tr.lineTo(QPointF(4 + i * 0.3 + 3 * std::cos(t), 13 + 5 * std::sin(t)));
        }
        p.drawPath(tr);
    } else if (type == QLatin1String("thread_toolpath")) {
        p.drawEllipse(QPointF(13, 13), 8, 8);
        p.setPen(cut);
        for (int i = 0; i < 4; ++i)
            p.drawLine(QPointF(7, 8 + i * 3.3), QPointF(19, 6.5 + i * 3.3));
    } else if (type == QLatin1String("slot_toolpath")) {
        p.drawRoundedRect(QRectF(4, 9, 18, 8), 4, 4);
        p.setPen(QPen(amber, 1.4, Qt::DashLine, Qt::RoundCap));
        p.drawLine(QPointF(8, 13), QPointF(18, 13));
    } else if (type == QLatin1String("circular_toolpath")) {
        p.setPen(cut);
        p.drawEllipse(QPointF(13, 13), 8, 8);
        p.drawEllipse(QPointF(13, 13), 5, 5);
        p.drawEllipse(QPointF(13, 13), 2, 2);
    } else {
        p.drawRoundedRect(box, 3, 3);
    }
    return QIcon(pm);
}

static QIcon toolIcon(const QString &kind)
{
    // Drawn at 3x so the icons stay sharp at the larger GUI scales.
    QPixmap pm(60, 60);
    pm.setDevicePixelRatio(3.0);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    if (drawModeIcon(p, kind))
        return QIcon(pm);
    // Constraints ("con-<type>") and the dimension tool: the canvas glyphs.
    if (kind.startsWith(QLatin1String("con-")) || kind == QLatin1String("dimension")) {
        const int t = kind == QLatin1String("dimension") ? int(Constraint::Distance)
                                                         : kind.mid(4).toInt();
        p.drawPixmap(QRectF(0, 0, 20, 20),
                     constraintPixmap(Constraint::Type(t), QColor(0xd8, 0xdc, 0xe4), 20, 3.0),
                     QRectF(0, 0, 60, 60));
        return QIcon(pm);
    }
    QPen pen(QColor(0xd8, 0xdc, 0xe4), 1.6);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    if (kind == "area") {
        // A closed shape, filled, with the arrow clicking inside it.
        QPainterPath s;
        s.moveTo(2.5, 6);
        s.cubicTo(2.5, 2.5, 9, 1.5, 13.5, 3.5);
        s.cubicTo(18, 5.5, 17.5, 11, 14.5, 13.5);
        s.cubicTo(11, 16.5, 2.5, 15.5, 2.5, 6);
        p.setBrush(QColor(0xf2, 0xa5, 0x36, 90));
        p.drawPath(s);
        QPolygonF arrow;
        arrow << QPointF(8, 6) << QPointF(8, 16) << QPointF(10.6, 13.4)
              << QPointF(12.6, 17.4) << QPointF(14, 16.7) << QPointF(12, 12.7) << QPointF(15, 12.5);
        p.setPen(QPen(QColor(0x1e, 0x21, 0x27), 1.0));
        p.setBrush(QColor(0xd8, 0xdc, 0xe4));
        p.drawPolygon(arrow);
    } else if (kind == "hand") {
        // An open hand, palm toward the viewer: four fingers and a thumb.
        QPainterPath h;
        h.moveTo(6.2, 17.5);
        h.cubicTo(4.5, 15.5, 3.2, 13.0, 2.6, 11.2);
        h.cubicTo(2.2, 10.0, 3.6, 9.4, 4.4, 10.3);
        h.lineTo(5.6, 11.9);
        h.lineTo(5.6, 4.6);
        h.cubicTo(5.6, 3.4, 7.4, 3.4, 7.4, 4.6);
        h.lineTo(7.4, 9.4);
        h.lineTo(7.4, 3.3);
        h.cubicTo(7.4, 2.1, 9.3, 2.1, 9.3, 3.3);
        h.lineTo(9.3, 9.4);
        h.lineTo(9.3, 3.9);
        h.cubicTo(9.3, 2.7, 11.2, 2.7, 11.2, 3.9);
        h.lineTo(11.2, 9.6);
        h.lineTo(11.2, 5.6);
        h.cubicTo(11.2, 4.4, 13.0, 4.4, 13.0, 5.6);
        h.lineTo(13.0, 12.8);
        h.cubicTo(13.0, 15.0, 12.0, 16.5, 11.2, 17.5);
        h.closeSubpath();
        p.drawPath(h);
    } else if (kind == "select") {
        QPolygonF arrow;
        arrow << QPointF(6, 3) << QPointF(6, 15) << QPointF(9.5, 11.5)
              << QPointF(12, 16) << QPointF(13.8, 15) << QPointF(11.4, 10.6)
              << QPointF(15, 10);
        p.setBrush(QColor(0xd8, 0xdc, 0xe4));
        p.drawPolygon(arrow);
    } else if (kind == "circle") {
        p.drawEllipse(QRectF(4, 4, 12, 12));
    } else if (kind == "rect") {
        p.drawRect(QRectF(4, 5, 12, 10));
    } else if (kind == "ellipse") {
        p.drawEllipse(QRectF(2.5, 5.5, 15, 9));
    } else if (kind == "slot") {
        p.drawRoundedRect(QRectF(2.5, 6, 15, 8), 4, 4);
        QPen thin(QColor(0x9f, 0xc8, 0xf2), 1.0);
        p.setPen(thin);
        p.drawLine(QLineF(6.5, 10, 13.5, 10));
    } else if (kind == "arc") {
        p.drawArc(QRectF(3, 5, 14, 14), 20 * 16, 140 * 16);
        p.setBrush(QColor(0xd8, 0xdc, 0xe4));
        for (const QPointF &v : {QPointF(3.6, 9.6), QPointF(10, 5), QPointF(16.4, 9.6)})
            p.drawEllipse(v, 1.5, 1.5);
    } else if (kind == "measure") {
        p.drawLine(QLineF(3, 15, 17, 5));
        p.drawLine(QLineF(1.5, 12.8, 4.5, 17.2));
        p.drawLine(QLineF(15.5, 2.8, 18.5, 7.2));
    } else if (kind == "trim") {
        p.drawLine(QLineF(10, 2, 10, 18));
        p.drawLine(QLineF(2, 10, 10, 10));
        QPen cut(QColor(0xe0, 0x50, 0x50), 1.6, Qt::DotLine);
        p.setPen(cut);
        p.drawLine(QLineF(10, 10, 18, 10));
    } else if (kind == "extend") {
        p.drawLine(QLineF(17, 2, 17, 18));
        p.drawLine(QLineF(2, 10, 9, 10));
        QPen ext(QColor(0x9f, 0xc8, 0xf2), 1.6, Qt::DashLine);
        p.setPen(ext);
        p.drawLine(QLineF(9, 10, 17, 10));
    } else if (kind == "break") {
        p.drawLine(QLineF(10, 2, 10, 18));
        p.drawLine(QLineF(2, 10, 8.5, 10));
        p.drawLine(QLineF(11.5, 10, 18, 10));
        p.setBrush(QColor(0xd8, 0xdc, 0xe4));
        p.drawEllipse(QPointF(10, 10), 1.6, 1.6);
    } else if (kind == "polygon") {
        QPolygonF hex;
        for (int i = 0; i < 6; ++i) {
            const double a = M_PI / 3 * i - M_PI / 6;
            hex << QPointF(10 + 6.5 * qCos(a), 10 + 6.5 * qSin(a));
        }
        p.drawPolygon(hex);
    } else if (kind == "point") {
        p.drawLine(QLineF(5, 5, 15, 15));
        p.drawLine(QLineF(5, 15, 15, 5));
    } else if (kind == "spline") {
        QPainterPath pp(QPointF(2.5, 14));
        pp.cubicTo(5, 4, 9, 4, 10, 10);
        pp.cubicTo(11, 16, 15, 16, 17.5, 6);
        p.drawPath(pp);
        p.setBrush(QColor(0xd8, 0xdc, 0xe4));
        for (const QPointF &v : {QPointF(2.5, 14), QPointF(10, 10), QPointF(17.5, 6)})
            p.drawEllipse(v, 1.5, 1.5);
    } else if (kind == "conic") {
        p.save();
        p.setPen(QPen(QColor(0x8a, 0x90, 0x9c), 1, Qt::DashLine));
        p.drawLine(QLineF(3, 16, 10, 4));
        p.drawLine(QLineF(10, 4, 17, 16));
        p.restore();
        QPainterPath pp(QPointF(3, 16));
        pp.cubicTo(6, 9, 14, 9, 17, 16);
        p.drawPath(pp);
    } else if (kind == "path") {
        QPainterPath pp(QPointF(3, 16));
        pp.lineTo(8, 6);
        pp.lineTo(12, 12);
        pp.lineTo(17, 4);
        p.drawPath(pp);
        p.setBrush(QColor(0xd8, 0xdc, 0xe4));
        for (const QPointF &v : {QPointF(3, 16), QPointF(8, 6), QPointF(12, 12), QPointF(17, 4)})
            p.drawEllipse(v, 1.6, 1.6);
    } else if (kind == "nodes") {
        // Node editor: a curve with an anchor square and a tangent handle.
        QPainterPath pp(QPointF(3, 16));
        pp.cubicTo(QPointF(6, 4), QPointF(14, 4), QPointF(17, 16));
        p.drawPath(pp);
        QPen thin(QColor(0x9f, 0xc8, 0xf2), 1.0);
        p.setPen(thin);
        p.drawLine(QLineF(5, 10, 15, 10));
        p.setBrush(QColor(0x9f, 0xc8, 0xf2));
        p.drawEllipse(QPointF(5, 10), 1.7, 1.7);
        p.drawEllipse(QPointF(15, 10), 1.7, 1.7);
        p.setPen(pen);
        p.setBrush(QColor(0xd8, 0xdc, 0xe4));
        p.drawRect(QRectF(8, 8, 4, 4));
    } else if (kind == "text") {
        QFont f;
        f.setPixelSize(15);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRectF(0, 0, 20, 20), Qt::AlignCenter, QStringLiteral("T"));
    } else if (kind == "gcode") {
        QFont f;
        f.setPixelSize(11);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRectF(0, 0, 20, 20), Qt::AlignCenter, QStringLiteral("G1"));
    } else if (kind == "delete") {          // waste bin
        p.drawLine(QLineF(4, 6, 16, 6));
        p.drawLine(QLineF(8, 6, 8, 4));
        p.drawLine(QLineF(8, 4, 12, 4));
        p.drawLine(QLineF(12, 4, 12, 6));
        p.drawPolyline(QPolygonF({QPointF(5.5, 6), QPointF(6.5, 17), QPointF(13.5, 17),
                                  QPointF(14.5, 6)}));
        p.drawLine(QLineF(8.5, 9, 8.8, 14));
        p.drawLine(QLineF(11.5, 9, 11.2, 14));
    } else if (kind == "fit") {
        p.drawRect(QRectF(4, 4, 12, 12));
        p.drawLine(QLineF(8, 10, 12, 10));
        p.drawLine(QLineF(10, 8, 10, 12));
    } else if (kind == "snap") {
        for (int x = 4; x <= 16; x += 6)
            for (int y = 4; y <= 16; y += 6)
                p.drawPoint(QPointF(x, y));
        p.drawEllipse(QPointF(10, 10), 3.2, 3.2);
    } else if (kind == "fx") {              // parameters: an italic ƒx
        QFont f;
        f.setPixelSize(14);
        f.setItalic(true);
        f.setBold(true);
        p.setFont(f);
        p.setPen(QColor(0xc3, 0xa6, 0xff));
        p.drawText(QRectF(0, 0, 20, 20), Qt::AlignCenter, QStringLiteral("\u0192x"));
    } else if (kind == "properties") {      // three sliders
        for (int i = 0; i < 3; ++i) {
            const double y = 5 + i * 5, k = (i == 1) ? 13 : (i == 0 ? 7 : 10);
            p.drawLine(QLineF(3, y, 17, y));
            p.setBrush(QColor(0xd8, 0xdc, 0xe4));
            p.drawEllipse(QPointF(k, y), 1.8, 1.8);
            p.setBrush(Qt::NoBrush);
        }
    } else if (kind == "toolpaths") {       // pocket zig-zag inside a boundary
        p.drawRect(QRectF(3, 3, 14, 14));
        QPainterPath zz(QPointF(6, 6));
        zz.lineTo(14, 6); zz.lineTo(14, 10); zz.lineTo(6, 10); zz.lineTo(6, 14); zz.lineTo(14, 14);
        p.setPen(QPen(QColor(0xf0, 0xa0, 0x30), 1.6));
        p.drawPath(zz);
    } else if (kind == "document") {        // page with a folded corner
        QPainterPath pg(QPointF(5, 2.5));
        pg.lineTo(12, 2.5); pg.lineTo(16, 6.5); pg.lineTo(16, 17.5); pg.lineTo(5, 17.5);
        pg.closeSubpath();
        p.drawPath(pg);
        p.drawLine(QLineF(12, 2.5, 12, 6.5));
        p.drawLine(QLineF(12, 6.5, 16, 6.5));
        p.drawLine(QLineF(7.5, 10, 13.5, 10));
        p.drawLine(QLineF(7.5, 13.5, 13.5, 13.5));
    } else if (kind == "machine") {         // gantry, spindle and bit over the bed
        p.drawLine(QLineF(2, 17, 18, 17));
        p.drawLine(QLineF(4, 17, 4, 4));
        p.drawLine(QLineF(16, 17, 16, 4));
        p.drawLine(QLineF(4, 4, 16, 4));
        p.drawRect(QRectF(8, 5, 4, 6));
        p.drawLine(QLineF(10, 11, 10, 14.5));
    } else if (kind == "preview") {         // isometric cube
        const QPointF t(10, 2.5), l(3.5, 6.2), r(16.5, 6.2), c(10, 10),
                      bl(3.5, 13.8), br(16.5, 13.8), b(10, 17.5);
        p.drawPolygon(QPolygonF({t, r, br, b, bl, l}));
        p.drawLine(QLineF(l, c)); p.drawLine(QLineF(r, c)); p.drawLine(QLineF(c, b));
    } else if (kind == "simulation") {      // play button
        p.drawEllipse(QRectF(2.5, 2.5, 15, 15));
        p.setBrush(QColor(0xd8, 0xdc, 0xe4));
        p.drawPolygon(QPolygonF({QPointF(8, 6.5), QPointF(14, 10), QPointF(8, 13.5)}));
    } else if (kind == "model") {           // relief rising off the stock
        QPainterPath m(QPointF(2, 15));
        m.cubicTo(QPointF(6, 15), QPointF(7, 5), QPointF(10, 5));
        m.cubicTo(QPointF(13, 5), QPointF(14, 15), QPointF(18, 15));
        p.drawPath(m);
        p.drawLine(QLineF(2, 17.5, 18, 17.5));
    }
    return QIcon(pm);
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("phobiCCCP — Carbide Create .c2d (Linux)"));
    // The shop screen hides its bottom fifth of a text line; keep the status
    // line a line clear of the edge.
    setContentsMargins(0, 0, 0, 16);
    resize(1100, 780);
    // Edits come in bursts (typing, dragging, stepping a field): the
    // preview export starts once they pause.
    m_previewTimer = new QTimer(this);
    m_previewTimer->setSingleShot(true);
    m_previewTimer->setInterval(250);
    connect(m_previewTimer, &QTimer::timeout, this, [this] {
        if (!m_previewRun)        // else it restarts when the busy run ends
            startPreviewRun();
    });
    // The menus live in a header of our own (menus · brand · mode switch),
    // so this is the bar every menu goes on; see the end of the constructor.
    m_menuBar = new QMenuBar(this);

    m_canvas = new Canvas(this);
    // One page per mode (see setMode): the canvas for Design and Toolpaths,
    // then the simulation and the machine as full pages of their own.
    m_stack = new QStackedWidget(this);
    setCentralWidget(m_stack);
    m_canvas->setBackgroundImage(&m_bg);

    // Right-hand column of the canvas page: one page per canvas mode, in a
    // splitter with the canvas. Plain widgets, not docks: docks that are
    // hidden and shown again re-split the column every time, and their
    // minimum heights outgrow the 800 px shop screen.
    m_props = new PropertiesPanel(m_canvas, this);
    m_parameters = new ParametersPanel(m_canvas, this);
    m_tp = new ToolpathPanel(m_canvas, this);
    m_tp->setTypeIcons(toolpathIcon);
    m_info = new QPlainTextEdit(this);
    m_info->setReadOnly(true);
    m_model = new ModelPanel(this);
    m_machine = new MachinePanel(this);
    m_iso = new IsoPreview(this);
    m_sim = new SimPanel(this);
    m_toolStrip = new ToolStrip(this);
    m_toolStrip->setMaximumWidth(130);
    connect(m_iso, &IsoPreview::currentToolChanged, m_toolStrip, &ToolStrip::setCurrent);
    connect(m_toolStrip, &ToolStrip::toolClicked, m_iso, &IsoPreview::seekToTool);

    auto scrolled = [](QWidget *w) {
        auto *sa = new QScrollArea;
        sa->setWidgetResizable(true);
        sa->setFrameShape(QFrame::NoFrame);
        sa->setWidget(w);
        return sa;
    };
    auto titled = [this](const QString &title, const QString &icon, QWidget *body) {
        auto *page = new QWidget(this);
        auto *v = new QVBoxLayout(page);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);
        auto *hl = new QHBoxLayout;
        hl->setContentsMargins(10, 0, 10, 0);
        hl->setSpacing(8);
        auto *ic = new QLabel(page);
        ic->setPixmap(toolIcon(icon).pixmap(18, 18));
        auto *tx = new QLabel(title, page);
        tx->setObjectName(QStringLiteral("panelTitle"));
        hl->addWidget(ic);
        hl->addWidget(tx);
        hl->addStretch(1);
        auto *headW = new QWidget(page);
        headW->setObjectName(QStringLiteral("panelHead"));
        headW->setAttribute(Qt::WA_StyledBackground);
        headW->setLayout(hl);
        headW->setFixedHeight(34);
        v->addWidget(headW);
        v->addWidget(body, 1);
        return page;
    };
    // Design: Properties over Document / Model tabs.
    m_docTabs = new QTabWidget(this);
    m_docTabs->setDocumentMode(true);
    m_docTabs->setIconSize(QSize(18, 18));
    m_docTabs->addTab(m_info, toolIcon(QStringLiteral("document")), QStringLiteral("Document"));
    m_docTabs->addTab(scrolled(m_model), toolIcon(QStringLiteral("model")), QStringLiteral("Model"));
    m_docTabs->addTab(scrolled(m_parameters), toolIcon(QStringLiteral("fx")), QStringLiteral("Parameters"));
    m_designPage = new QSplitter(Qt::Vertical, this);
    m_designPage->addWidget(titled(QStringLiteral("Properties"), QStringLiteral("properties"),
                                   scrolled(m_props)));
    m_designPage->addWidget(m_docTabs);
    m_designPage->setChildrenCollapsible(false);
    m_designPage->setStretchFactor(0, 1);
    m_designPage->setStretchFactor(1, 1);
    // Toolpaths: the toolpath list and editor.
    // The Toolpaths side scrolls by dragging its open areas too (touch screens).
    auto *tpScroll = scrolled(m_tp);
    DragScroll::install(tpScroll);
    m_tpPage = titled(QStringLiteral("Toolpaths"), QStringLiteral("toolpaths"), tpScroll);
    m_rightStack = new QStackedWidget(this);
    m_rightStack->addWidget(m_designPage);
    m_rightStack->addWidget(m_tpPage);
    m_rightStack->setMinimumWidth(300);
    m_canvasPage = new QSplitter(Qt::Horizontal, this);
    m_canvasPage->addWidget(m_canvas);
    // Zoom - / + and the zoom % / X Y readout under the right-hand panel,
    // in Design and Toolpaths alike.
    auto *rightCol = new QWidget(this);
    auto *rcl = new QVBoxLayout(rightCol);
    rcl->setContentsMargins(0, 0, 0, 6);
    rcl->setSpacing(6);
    rcl->addWidget(m_rightStack, 1);
    rcl->addWidget(m_canvas->zoomBar(), 0, Qt::AlignLeft);
    m_canvasPage->addWidget(rightCol);
    m_canvasPage->setChildrenCollapsible(false);
    m_canvasPage->setStretchFactor(0, 3);
    m_canvasPage->setStretchFactor(1, 1);
    m_canvasPage->setHandleWidth(10);
    m_stack->addWidget(m_canvasPage);
    // A splitter that is hidden (another page on top) and shown again hands
    // the space out by size hints, which starves the canvas. Remember the
    // sizes ourselves and put them back whenever a canvas mode is entered.
    for (const QVariant &v : QSettings().value(QStringLiteral("ui/canvasSizes")).toList())
        m_canvasSizes << v.toInt();
    for (const QVariant &v : QSettings().value(QStringLiteral("ui/designSizes")).toList())
        m_designSizes << v.toInt();
    connect(m_canvasPage, &QSplitter::splitterMoved, this, [this] {
        m_canvasSizes = m_canvasPage->sizes();
        QVariantList l;
        for (int v : m_canvasSizes) l << v;
        QSettings().setValue(QStringLiteral("ui/canvasSizes"), l);
    });
    connect(m_designPage, &QSplitter::splitterMoved, this, [this] {
        m_designSizes = m_designPage->sizes();
        QVariantList l;
        for (int v : m_designSizes) l << v;
        QSettings().setValue(QStringLiteral("ui/designSizes"), l);
    });

    m_stack->addWidget(m_machine);

    // Simulate page: the 3D route preview beside the material-removal view.
    m_simPage = new QSplitter(Qt::Horizontal, this);
    m_simPage->addWidget(m_toolStrip);
    m_simPage->addWidget(m_iso);
    m_simPage->addWidget(m_sim);
    m_simPage->setStretchFactor(0, 0);
    m_simPage->setStretchFactor(1, 1);
    m_simPage->setStretchFactor(2, 1);
    m_simPage->setChildrenCollapsible(false);
    m_stack->addWidget(m_simPage);

    setStyleSheet(styleSheet() + QStringLiteral(
        "#panelHead { background: #23262e; border-bottom: 1px solid #2a2e36; }"
        "#panelTitle { font-weight: bold; font-size: 13px; }"
        "QTabWidget::pane { border: 0; }"
        "QTabBar::tab { font-weight: bold; padding: 7px 14px; font-size: 13px; }"
        // Wide enough to grab with a finger; follows the GUI scale.
        "QSplitter::handle { background: #2a2e36; }"
        "QSplitter::handle:hover { background: #4a90d9; }"));
    // The 3D preview and the simulation's program are only rebuilt while the
    // Simulate page is showing; setMode catches up when it is entered.

    connect(m_canvas, &Canvas::selectionChangedIds,
            m_props, &PropertiesPanel::setSelection);
    connect(m_canvas, &Canvas::selectionChangedIds,
            m_model, &ModelPanel::setSelection);
    // Component edits dirty the document (they are saved into the .c2d) and
    // change what the 3D toolpaths cut.
    connect(m_model, &ModelPanel::modelChanged, this, [this] {
        m_dirty = true;
        updateTitle();
    });
    connect(m_model, &ModelPanel::modelRebuilt, this, [this] {
        if (m_previewAct->isChecked())
            refreshPreview();
        else
            markIsoStale();
    });
    // Real machine work position → orange crosshair in the 3D preview.
    connect(m_machine, &MachinePanel::livePosition,
            m_iso, &IsoPreview::setLivePosition);

    // Shortcuts are set on the returned QAction rather than passed to
    // addAction: the (text, receiver, method, shortcut) overload is deprecated
    // from Qt 6.4 and the (text, shortcut, receiver, method) replacement only
    // exists from 6.3, so neither is both warning-free and version-neutral.
    auto *fileMenu = m_menuBar->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(QStringLiteral("&New…"), this, &MainWindow::onNew)
            ->setShortcut(QKeySequence::New);
    fileMenu->addAction(QStringLiteral("&Open…"), this, &MainWindow::onOpen)
            ->setShortcut(QKeySequence::Open);
    m_recentMenu = fileMenu->addMenu(QStringLiteral("Open &Recent"));
    m_recentMenu->setToolTipsVisible(true);   // full path, since names repeat
    rebuildRecentMenu();
    fileMenu->addAction(QStringLiteral("&Save"), this, &MainWindow::onSave)
            ->setShortcut(QKeySequence::Save);
    fileMenu->addAction(QStringLiteral("Save &As…"), this, &MainWindow::onSaveAs)
            ->setShortcut(QKeySequence::SaveAs);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("&Import vectors…"), this, [this] {
        importVectorFile(this, m_canvas, &m_doc, {}, {}); })
            ->setToolTip(QStringLiteral("SVG, DXF, HPGL (.plt), PDF, Illustrator (.ai), EPS, PostScript"));
    installImportDropHandler(this, m_canvas, &m_doc, [this](const QString &p) { openFile(p); });
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("Sen&d to phobiCCCP…"), this, &MainWindow::onSendTo)
            ->setToolTip(QStringLiteral("Send this design to phobiCCCP on the computer by the machine; "
                                        "it opens there and can start cutting"));
    fileMenu->addAction(QStringLiteral("Send to &pad…"), this, &MainWindow::onSendToPad)
            ->setToolTip(QStringLiteral("Open this design on a PhobiCCCP pad to edit it there; "
                                        "the edits come back to this file"));
    fileMenu->addAction(QStringLiteral("Export &G-code…"), this,
                        &MainWindow::onExportGcode)
            ->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
    fileMenu->addAction(QStringLiteral("Export G-code (&tiled)…"), this,
                        &MainWindow::onExportGcodeTiled);
    fileMenu->addAction(QStringLiteral("Setup S&heet…"), this, [this] {
        // Fusion's setup sheet: a printable page of the job for the machine.
        if (m_doc.toolpaths().isEmpty()) {
            QMessageBox::information(this, QStringLiteral("Setup sheet"),
                                     QStringLiteral("There are no toolpaths to describe yet."));
            return;
        }
        commitEdits();
        ++m_holdDoc;
        const auto hold = qScopeGuard([this] { --m_holdDoc; });
        const QFileInfo fi(m_untitled ? m_untitledFrom : m_doc.filePath());
        const QString title = fi.fileName().isEmpty() ? QStringLiteral("Untitled") : fi.completeBaseName();
        const QString dest = QFileDialog::getSaveFileName(
            this, QStringLiteral("Save setup sheet"),
            (fi.fileName().isEmpty() ? QDir::homePath() : fi.absolutePath())
                + QLatin1Char('/') + title + QStringLiteral(" setup sheet.html"),
            QStringLiteral("HTML (*.html)"));
        if (dest.isEmpty())
            return;
        QFile f(dest);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QMessageBox::warning(this, QStringLiteral("Setup sheet"),
                                 QStringLiteral("Could not write %1").arg(dest));
            return;
        }
        f.write(setupSheetHtml(m_doc, title).toUtf8());
        f.close();
        statusBar()->showMessage(QStringLiteral("Setup sheet saved to %1").arg(dest), 6000);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dest));
    });
    fileMenu->addSeparator();
    m_receiveAct = fileMenu->addAction(QStringLiteral("&Receive from Syntexophobic and pads"));
    m_receiveAct->setCheckable(true);
    m_receiveAct->setToolTip(QStringLiteral(
        "Let Syntexophobic and the PhobiCCCP pad app on this network or over Tailscale "
        "find this computer and send designs here (port %1).").arg(ProjectReceiver::kPort));
    m_receiveAct->setChecked(QSettings().value(QStringLiteral("receive/enabled"), true).toBool());
    connect(m_receiveAct, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("receive/enabled"), on);
        setReceiving(on);
    });
    fileMenu->addSeparator();
    installImageMenus(fileMenu, this, m_canvas, &m_doc, &m_bg);   // backgrounddialog.cpp
    fileMenu->addSeparator();
    // close(), not quit(): quit() skips closeEvent and with it the
    // unsaved-changes prompt.
    fileMenu->addAction(QStringLiteral("Se&ttings…"), this, &MainWindow::onSettings)
            ->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("E&xit"), this, &QWidget::close)
            ->setShortcut(QKeySequence::Quit);

    // Edit menu: undo/redo backed by the canvas undo stack.
    // About goes last on the bar, as a direct entry rather than a Help menu;
    // other menus are still being added below and by the install* helpers,
    // so append it once construction has finished.
    QTimer::singleShot(0, this, [this] {
        buildMachineMenu();
        m_menuBar->addAction(QStringLiteral("&About"), this, [this] {
            AboutDialog dlg(this);
            dlg.exec();
        });
    });
    auto *editMenu = m_menuBar->addMenu(QStringLiteral("&Edit"));
    QAction *undoAct = m_canvas->undoStack()->createUndoAction(this, QStringLiteral("&Undo"));
    undoAct->setShortcut(QKeySequence::Undo);
    QAction *redoAct = m_canvas->undoStack()->createRedoAction(this, QStringLiteral("&Redo"));
    redoAct->setShortcut(QKeySequence::Redo);
    editMenu->addAction(undoAct);
    editMenu->addAction(redoAct);
    editMenu->addSeparator();
    QAction *constrAct = editMenu->addAction(QStringLiteral("&Construction"), this, [this] {
        m_canvas->toggleConstruction(m_canvas->selectedElementIds());
    });
    constrAct->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_X));
    constrAct->setToolTip(QStringLiteral(
        "Make the selected vectors construction geometry (dashed, never machined), "
        "or normal again  (Shift+X)"));
    // Delete in the menu as well as on the key: on the touch screen there is
    // no Delete key and no right-click.
    QAction *delAct = editMenu->addAction(QStringLiteral("&Delete"), this, [this] {
        m_canvas->deleteSelected();
    });
    delAct->setToolTip(QStringLiteral("Delete the selected vectors  (Delete)"));

    // View > Full Screen: the window covers the whole display, panel and all.
    // Remembered, so the shop PC comes back up the way it was left.
    auto *viewMenu = m_menuBar->addMenu(QStringLiteral("&View"));
    {
        QAction *showCon = viewMenu->addAction(QStringLiteral("&Constraints and Dimensions"));
        showCon->setCheckable(true);
        showCon->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K));
        showCon->setToolTip(QStringLiteral("Show the constraint symbols and dimensions on the drawing"));
        const bool on = QSettings().value(QStringLiteral("ui/showConstraints"), true).toBool();
        showCon->setChecked(on);
        m_canvas->setShowConstraints(on);
        connect(showCon, &QAction::toggled, this, [this](bool on) {
            m_canvas->setShowConstraints(on);
            QSettings().setValue(QStringLiteral("ui/showConstraints"), on);
        });
        QAction *paramsAct = viewMenu->addAction(QStringLiteral("&Parameters"));
        paramsAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));
        connect(paramsAct, &QAction::triggered, this, &MainWindow::showParameters);
        viewMenu->addSeparator();
    }
    m_fullScreenAct = viewMenu->addAction(QStringLiteral("&Full Screen"));
    m_fullScreenAct->setCheckable(true);
    m_fullScreenAct->setShortcut(QKeySequence(Qt::Key_F11));
    m_fullScreenAct->setShortcutContext(Qt::ApplicationShortcut);
    addAction(m_fullScreenAct);           // still reachable if the menu bar is hidden
    connect(m_fullScreenAct, &QAction::toggled, this, [this](bool on) {
        if (on)
            showFullScreen();
        else
            showNormal();
        QSettings().setValue(QStringLiteral("ui/fullScreen"), on);
    });

    // Vector tools: horizontal icon row above the canvas (deliberately not
    // Carbide Create's left-hand column).
    auto *palette = new QToolBar(QStringLiteral("Tools"), this);
    palette->setMovable(false);
    palette->setToolButtonStyle(Qt::ToolButtonIconOnly);
    palette->setIconSize(QSize(24, 24));
    addToolBar(Qt::TopToolBarArea, palette);
    auto *grp = new QActionGroup(this);
    auto addTool = [&](const QString &name, const QString &icon, Canvas::Tool t,
                       Qt::Key key, const QString &tip) {
        QAction *a = palette->addAction(toolIcon(icon), name);
        a->setObjectName(icon);           // ToolbarOrder saves the order by name
        a->setCheckable(true);
        a->setShortcut(key);
        a->setToolTip(tip);
        grp->addAction(a);
        connect(a, &QAction::triggered, this, [this, t] { m_canvas->setTool(t); });
        return a;
    };
    QAction *selectAct = addTool(QStringLiteral("Select"), QStringLiteral("select"), Canvas::Select, Qt::Key_V,
            QStringLiteral("Select / move / delete  (V)"));
    selectAct->setChecked(true);
    QAction *handAct = addTool(QStringLiteral("Hand"), QStringLiteral("hand"), Canvas::Pan, Qt::Key_H,
            QStringLiteral("Hand: drag to move the view  (H)"));
    QAction *areaAct = addTool(QStringLiteral("Area"), QStringLiteral("area"), Canvas::SelectArea, Qt::Key_W,
            QStringLiteral("Area: click inside an area to select just it, like a fill, for a pocket  (W)"));
    QAction *circleAct = addTool(QStringLiteral("Circle"), QStringLiteral("circle"),
            Canvas::DrawCircle, Qt::Key_C,
            QStringLiteral("Circle (C) — hold for Center / 2-Point / 3-Point / 2-Tangent / 3-Tangent"));
    QAction *rectAct = addTool(QStringLiteral("Rect"), QStringLiteral("rect"),
            Canvas::DrawRect, Qt::Key_R,
            QStringLiteral("Rectangle (R) — hold for 2-Point / 3-Point / Center"));
    // Fusion's sketch variants: a click uses the last mode picked; press and
    // hold (500 ms, see PhobicStyle) opens the menu that picks the mode and
    // switches to the tool. Every mode has its own icon, and the button
    // takes the icon of the mode in use.
    struct Mode { QString name; QString icon; int value; };
    auto addModes = [&](QAction *toolAct, const QList<Mode> &modes,
                        const std::function<void(int)> &apply) {
        auto *menu = new QMenu(this);
        auto *modeGrp = new QActionGroup(menu);
        for (const auto &m : modes) {
            const QIcon icon = toolIcon(m.icon);
            QAction *ma = menu->addAction(icon, m.name);
            ma->setCheckable(true);
            modeGrp->addAction(ma);
            const int mode = m.value;
            connect(ma, &QAction::triggered, this, [toolAct, apply, mode, icon] {
                apply(mode);
                toolAct->setIcon(icon);
                if (!toolAct->isChecked())
                    toolAct->trigger();
            });
        }
        menu->actions().first()->setChecked(true);
        toolAct->setIcon(menu->actions().first()->icon());
        // On the action, not the button: rearranging the bar makes new
        // buttons, and ToolbarOrder gives each one the press-and-hold popup.
        toolAct->setMenu(menu);
    };
    addModes(circleAct,
             {{QStringLiteral("Center Diameter Circle"), QStringLiteral("circle-center"), Canvas::CircleCenterDiameter},
              {QStringLiteral("2-Point Circle"), QStringLiteral("circle-2pt"), Canvas::Circle2Point},
              {QStringLiteral("3-Point Circle"), QStringLiteral("circle-3pt"), Canvas::Circle3Point},
              {QStringLiteral("2-Tangent Circle"), QStringLiteral("circle-2tan"), Canvas::Circle2Tangent},
              {QStringLiteral("3-Tangent Circle"), QStringLiteral("circle-3tan"), Canvas::Circle3Tangent}},
             [this](int m) { m_canvas->setCircleMode(Canvas::CircleMode(m)); });
    addModes(rectAct,
             {{QStringLiteral("2-Point Rectangle"), QStringLiteral("rect-2pt"), Canvas::Rect2Point},
              {QStringLiteral("3-Point Rectangle"), QStringLiteral("rect-3pt"), Canvas::Rect3Point},
              {QStringLiteral("Center Rectangle"), QStringLiteral("rect-center"), Canvas::RectCenter}},
             [this](int m) { m_canvas->setRectMode(Canvas::RectMode(m)); });
    QAction *polyAct = addTool(QStringLiteral("Polygon"), QStringLiteral("polygon"),
            Canvas::DrawPolygon, Qt::Key_P,
            QStringLiteral("Polygon (P) — hold for Inscribed / Circumscribed / Edge"));
    addModes(polyAct,
             {{QStringLiteral("Inscribed Polygon"), QStringLiteral("poly-inscribed"), Canvas::PolyInscribed},
              {QStringLiteral("Circumscribed Polygon"), QStringLiteral("poly-circumscribed"), Canvas::PolyCircumscribed},
              {QStringLiteral("Edge Polygon"), QStringLiteral("poly-edge"), Canvas::PolyEdge}},
             [this](int m) { m_canvas->setPolygonMode(Canvas::PolygonMode(m)); });
    addTool(QStringLiteral("Ellipse"), QStringLiteral("ellipse"), Canvas::DrawEllipse, Qt::Key_E,
            QStringLiteral("Ellipse: drag across its bounding box, Ctrl from the center  (E)"));
    QAction *slotAct = addTool(QStringLiteral("Slot"), QStringLiteral("slot"), Canvas::DrawSlot,
            Qt::Key_S,
            QStringLiteral("Slot (S) — hold for Center to Center / Overall / Center Point / 3-Point Arc"));
    addModes(slotAct,
             {{QStringLiteral("Center to Center Slot"), QStringLiteral("slot-c2c"), Canvas::SlotCenterToCenter},
              {QStringLiteral("Overall Slot"), QStringLiteral("slot-overall"), Canvas::SlotOverall},
              {QStringLiteral("Center Point Slot"), QStringLiteral("slot-center"), Canvas::SlotCenterPoint},
              {QStringLiteral("3-Point Arc Slot"), QStringLiteral("slot-arc"), Canvas::SlotArc3Point}},
             [this](int m) { m_canvas->setSlotMode(Canvas::SlotMode(m)); });
    QAction *arcAct = addTool(QStringLiteral("Arc"), QStringLiteral("arc"), Canvas::DrawArc,
            Qt::Key_A, QStringLiteral("Arc (A) — hold for 3-Point / Center Point / Tangent"));
    addModes(arcAct,
             {{QStringLiteral("3-Point Arc"), QStringLiteral("arc-3pt"), Canvas::Arc3Point},
              {QStringLiteral("Center Point Arc"), QStringLiteral("arc-center"), Canvas::ArcCenter},
              {QStringLiteral("Tangent Arc"), QStringLiteral("arc-tangent"), Canvas::ArcTangent}},
             [this](int m) { m_canvas->setArcMode(Canvas::ArcMode(m)); });
    addTool(QStringLiteral("Spline"), QStringLiteral("spline"), Canvas::DrawSpline, Qt::Key_K,
            QStringLiteral("Fit-point spline: click the points it passes through; Enter finishes, click near start closes  (K)"));
    addTool(QStringLiteral("Conic"), QStringLiteral("conic"), Canvas::DrawConic, Qt::Key_Q,
            QStringLiteral("Conic curve: click the start, the end, then the apex its tangents meet at; rho in the options bar  (Q)"));
    addTool(QStringLiteral("Point"), QStringLiteral("point"), Canvas::DrawPoint, Qt::Key_O,
            QStringLiteral("Point: click to place a sketch point that drilling toolpaths drill at  (O)"));
    addTool(QStringLiteral("Path"), QStringLiteral("path"), Canvas::DrawPath, Qt::Key_L,
            QStringLiteral("Path: click = corner, click-drag = curve; Enter finishes, click near start closes  (L)"));
    addTool(QStringLiteral("Trim"), QStringLiteral("trim"), Canvas::Trim, Qt::Key_X,
            QStringLiteral("Trim: click the piece of a curve to cut away, up to where other curves cross it  (X)"));
    addTool(QStringLiteral("Extend"), QStringLiteral("extend"), Canvas::Extend, Qt::Key_J,
            QStringLiteral("Extend: click near an open curve's end to run it on to the next curve  (J)"));
    addTool(QStringLiteral("Break"), QStringLiteral("break"), Canvas::Break, Qt::Key_B,
            QStringLiteral("Break: click a curve to split it where other curves cross it  (B)"));
    addTool(QStringLiteral("Measure"), QStringLiteral("measure"), Canvas::Measure, Qt::Key_M,
            QStringLiteral("Measure: drag between two points for the distance, ΔX, ΔY and angle  (M)"));
    QAction *conAct = addTool(QStringLiteral("Constrain"), QStringLiteral("con-0"), Canvas::Constrain,
            Qt::Key_G,
            QStringLiteral("Constrain (G) — hold for Coincident / Horizontal / Vertical / Parallel / "
                           "Perpendicular / Tangent / Equal / Concentric / Midpoint / Collinear / Fix / Symmetric"));
    {
        QList<Mode> modes;
        for (int t = Constraint::Coincident; t <= Constraint::Symmetric; ++t)
            modes.append({Constraint::typeName(Constraint::Type(t)),
                          QStringLiteral("con-%1").arg(t), t});
        addModes(conAct, modes,
                 [this](int m) { m_canvas->setConstraintType(Constraint::Type(m)); });
    }
    addTool(QStringLiteral("Dimension"), QStringLiteral("dimension"), Canvas::Dimension, Qt::Key_D,
            QStringLiteral("Dimension (D): click a line, two points, a point and a line, two lines or a circle, "
                           "place it, then type a value or an expression"));
    addTool(QStringLiteral("Nodes"), QStringLiteral("nodes"), Canvas::NodeEdit, Qt::Key_N,
            QStringLiteral("Edit nodes: drag anchors/handles, double-click to insert, Del to remove, right-click for Corner/Smooth/Symmetric  (N)"));
    addTool(QStringLiteral("Text"), QStringLiteral("text"), Canvas::DrawText, Qt::Key_T,
            QStringLiteral("Text: click to place  (T)"));
    // Drag the icons sideways to rearrange them; kept across restarts.
    (new ToolbarOrder(palette, QStringLiteral("ui/toolOrder")))->restore();

    m_palette = palette;

    // Toolpaths mode swaps the drawing tools for one button per toolpath
    // kind: icon, name, and a one-line explanation as its tooltip. Pressing
    // one creates that toolpath for the vectors selected on the canvas.
    m_tpBar = new QToolBar(QStringLiteral("Toolpath kinds"), this);
    m_tpBar->setMovable(false);
    m_tpBar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    m_tpBar->setIconSize(QSize(26, 26));
    // A little narrower than the other toolbars, so the Options bar beside
    // it (Delete, Snap, Fit) still fits at 1280 px wide.
    m_tpBar->setStyleSheet(QStringLiteral("QToolButton { padding: 4px 5px; }"));
    addToolBar(Qt::TopToolBarArea, m_tpBar);
    // Arrow, hand and closed-area select on the Toolpaths page too (the same
    // actions as the drawing palette), icon only so the bar stays narrow.
    for (QAction *a : {selectAct, handAct, areaAct}) {
        auto *b = new QToolButton(m_tpBar);
        b->setDefaultAction(a);
        b->setToolButtonStyle(Qt::ToolButtonIconOnly);
        b->setIconSize(QSize(26, 26));
        m_tpBar->addWidget(b);
    }
    m_tpBar->addSeparator();
    {
        const QHash<QString, QString> why = {
            {QStringLiteral("contour"), QStringLiteral("Follow the outline: outside, inside or on the line")},
            {QStringLiteral("pocket_toolpath"), QStringLiteral("Clear everything inside the shape to a depth")},
            {QStringLiteral("drilling_toolpath"), QStringLiteral("Peck a hole at each selected circle")},
            {QStringLiteral("texture_toolpath"), QStringLiteral("Random shallow strokes for a textured surface")},
            {QStringLiteral("advanced_vcarve_toolpath"), QStringLiteral("V-bit carving of text and shapes, depth follows width")},
            {QStringLiteral("keyhole_toolpath"), QStringLiteral("Keyhole hanger slot: plunge, slide, come back")},
            {QStringLiteral("cutout"), QStringLiteral("Cut the shape free, with tabs to hold it")},
            {QStringLiteral("engrave_toolpath"), QStringLiteral("Single-line engraving, outline or hatch fill")},
            {QStringLiteral("face_toolpath"), QStringLiteral("Skim the stock flat")},
            {QStringLiteral("bore_toolpath"), QStringLiteral("Helix down the wall of each circle")},
            {QStringLiteral("chamfer_toolpath"), QStringLiteral("V-bit chamfer along the edges")},
            {QStringLiteral("adaptive_toolpath"), QStringLiteral("Clear with a light, steady bite at a deep stepdown")},
            {QStringLiteral("thread_toolpath"), QStringLiteral("Mill a thread around each circle, inside or outside")},
            {QStringLiteral("slot_toolpath"), QStringLiteral("Ramp down the centreline of a slot")},
            {QStringLiteral("circular_toolpath"), QStringLiteral("Round pocket or boss on each circle, in full laps")}};
        for (const ToolpathKind &k : toolpathKinds()) {
            QAction *a = m_tpBar->addAction(toolpathIcon(k.type), k.label);
            a->setToolTip(QStringLiteral("%1 — %2  (for the selected vectors)")
                              .arg(k.label, why.value(k.type)));
            const QString type = k.type;
            connect(a, &QAction::triggered, this, [this, type] { m_tp->newToolpath(type); });
        }
    }
    // Cut order: the list top to bottom (default), or same-tool toolpaths
    // pooled so each site is finished before the spindle moves on.
    m_tpBar->addSeparator();
    {
        QAction *site = m_tpBar->addAction(QStringLiteral("Site order"));
        site->setCheckable(true);
        site->setChecked(QSettings().value(QStringLiteral("export/siteOrder"), false).toBool());
        site->setToolTip(QStringLiteral("Off: cut in list order, top to bottom.\n"
                                        "On: pool toolpaths that share a tool and finish each site "
                                        "(counterbore, then its through-hole) before moving on."));
        connect(site, &QAction::toggled, this, [this](bool on) {
            QSettings().setValue(QStringLiteral("export/siteOrder"), on);
            m_doc.setParam(QStringLiteral("site_order"), on ? QStringLiteral("1") : QStringLiteral("0"));
            markIsoStale();
            if (m_previewAct->isChecked())
                refreshPreview();
        });
    }

    // Top bar: options and view/edit actions.
    auto *tb = addToolBar(QStringLiteral("Options"));
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    tb->setIconSize(QSize(20, 20));

    auto *sides = new QSpinBox(tb);
    sides->setRange(3, 64);
    sides->setValue(6);
    sides->setPrefix(QStringLiteral("sides "));
    sides->setToolTip(QStringLiteral("Polygon sides"));
    QAction *sidesAct = tb->addWidget(sides);
    connect(sides, &QSpinBox::valueChanged, this,
            [this](int n) { m_canvas->setPolygonSides(n); });
    auto *rho = new QDoubleSpinBox(tb);
    rho->setRange(0.05, 0.95);
    rho->setSingleStep(0.05);
    rho->setDecimals(2);
    rho->setValue(0.5);
    rho->setPrefix(QStringLiteral("rho "));
    rho->setToolTip(QStringLiteral("Conic fullness: 0.5 parabola, lower an ellipse arc, higher a hyperbola"));
    QAction *rhoAct = tb->addWidget(rho);
    connect(rho, &QDoubleSpinBox::valueChanged, this,
            [this](double v) { m_canvas->setConicRho(v); });
    QAction *sidesSep = tb->addSeparator();

    // The side count only means something while drawing polygons and rho
    // while drawing conics, so each shows (with the separator) only then.
    auto showSides = [sidesAct, rhoAct, sidesSep](Canvas::Tool t) {
        sidesAct->setVisible(t == Canvas::DrawPolygon);
        rhoAct->setVisible(t == Canvas::DrawConic);
        sidesSep->setVisible(t == Canvas::DrawPolygon || t == Canvas::DrawConic);
    };
    showSides(m_canvas->tool());
    connect(m_canvas, &Canvas::toolChanged, this, showSides);
    // Delete the selected vectors: on the touch screen there is no Delete
    // key. First on the Options bar, so both Design and Toolpaths show it
    // even when the bar runs out of room on the right.
    QAction *delBtn = tb->addAction(toolIcon(QStringLiteral("delete")), QStringLiteral("Delete"));
    delBtn->setToolTip(QStringLiteral("Delete the selected vectors  (Delete)"));
    delBtn->setEnabled(false);
    connect(delBtn, &QAction::triggered, this, [this] { m_canvas->deleteSelected(); });
    connect(m_canvas, &Canvas::selectionChangedIds, delBtn,
            [delBtn](const QStringList &ids) { delBtn->setEnabled(!ids.isEmpty()); });
    QAction *snapAct = tb->addAction(toolIcon(QStringLiteral("snap")), QStringLiteral("Snap"));
    snapAct->setCheckable(true);
    // Not plain G: that is the Constrain tool's, and a key bound twice in
    // one window fires neither.
    snapAct->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_G));
    snapAct->setToolTip(QStringLiteral("Snap to grid  (Shift+G)"));
    connect(snapAct, &QAction::toggled, this,
            [this](bool on) { m_canvas->setSnapEnabled(on); });

    QAction *fitAct = tb->addAction(toolIcon(QStringLiteral("fit")), QStringLiteral("Fit"));
    fitAct->setShortcut(Qt::Key_F);
    fitAct->setToolTip(QStringLiteral("Zoom to the selection, or the whole board when nothing is selected  (F)"));
    connect(fitAct, &QAction::triggered, this, [this] { m_canvas->zoomFit(); });
    // Keyboard zoom; the - / + buttons are on the canvas, bottom right.
    auto *zoomInAct = new QAction(QStringLiteral("Zoom in"), this);
    zoomInAct->setShortcuts({QKeySequence(Qt::Key_Plus), QKeySequence(Qt::Key_Equal), QKeySequence::ZoomIn});
    connect(zoomInAct, &QAction::triggered, this, [this] { m_canvas->zoomBy(1.25); });
    auto *zoomOutAct = new QAction(QStringLiteral("Zoom out"), this);
    zoomOutAct->setShortcuts({QKeySequence(Qt::Key_Minus), QKeySequence::ZoomOut});
    connect(zoomOutAct, &QAction::triggered, this, [this] { m_canvas->zoomBy(1.0 / 1.25); });
    // Only while the canvas has focus: "-" must still type in number boxes.
    zoomInAct->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    zoomOutAct->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    m_canvas->addActions({zoomInAct, zoomOutAct});

    m_previewAct = tb->addAction(toolIcon(QStringLiteral("gcode")),
                                 QStringLiteral("Preview"));
    m_previewAct->setCheckable(true);
    m_previewAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_P));
    m_previewAct->setToolTip(QStringLiteral(
        "Show the generated toolpath on the canvas — rapids dashed, cuts "
        "colored shallow→deep  (Ctrl+P)"));
    connect(m_previewAct, &QAction::toggled, this, [this](bool on) {
        if (!on) {
            m_canvas->clearToolpathPreview();
            return;
        }
        refreshPreview();
    });

    tb->addSeparator();
    tb->addAction(undoAct);
    tb->addAction(redoAct);
    new VectorActions(m_canvas, editMenu, this);   // Edit → Vectors + icon bar

    // Status bar: transient hints. Zoom and the live mm position are on the
    // canvas itself (bottom right, with the zoom buttons).

    connect(m_canvas, &Canvas::documentChanged, this, [this] {
        m_dirty = true;
        updateTitle();
        refreshInfo();
        m_props->refresh();
        m_tp->refresh();
        // When the 3D model has to be recomposited, wait for the worker:
        // refreshing now would build it synchronously here and again there.
        if (!m_model->documentChanged()) {
            if (m_previewAct->isChecked())
                refreshPreview();
            else
                markIsoStale();
        }
        statusBar()->showMessage(QStringLiteral("Edited — Ctrl+S to save"));
    });
    connect(m_canvas, &Canvas::statusHint, this, [this](const QString &m) {
        statusBar()->showMessage(m);
    });
    connect(m_parameters, &ParametersPanel::message, this, [this](const QString &m) {
        statusBar()->showMessage(m, 8000);
    });

    statusBar()->showMessage(QStringLiteral("Open a .c2d file to begin"));

    // ---- Mode switch: Design → Toolpaths → Simulate → Machine --------------
    // A segmented control at the top right of the menu bar, so it is in reach
    // from every page; Ctrl+1…4 and View menu entries do the same.
    auto *modeBox = new QWidget(this);
    modeBox->setObjectName(QStringLiteral("modeSwitch"));
    modeBox->setAttribute(Qt::WA_StyledBackground);
    modeBox->setStyleSheet(QStringLiteral(
        "#modeSwitch { background: #16181d; border: 1px solid #2a2e36; border-radius: 8px; }"
        "#modeSwitch QToolButton { border: 0; border-radius: 6px; padding: 6px 14px; font-size: 14px; "
        "  color: #a3abb8; background: transparent; }"
        "#modeSwitch QToolButton:hover { background: #333945; }"
        "#modeSwitch QToolButton:checked { background: #2f343d; color: #d8dce4; font-weight: bold; }"));
    auto *modeLay = new QHBoxLayout(modeBox);
    modeLay->setContentsMargins(3, 3, 3, 3);
    modeLay->setSpacing(3);
    auto *modeGrp = new QActionGroup(this);
    modeGrp->setExclusive(true);
    viewMenu->addSeparator();
    const char *modeNames[] = {"1 · Design", "2 · Toolpaths", "3 · Simulate", "4 · Machine"};
    const char *modeTips[] = {
        "Draw and edit vectors  (Ctrl+1)",
        "Toolpaths, cut order and the 3D preview  (Ctrl+2)",
        "Full-page material-removal simulation of the job  (Ctrl+3)",
        "Connect, jog, zero and run the machine  (Ctrl+4)"};
    for (int i = 0; i < 4; ++i) {
        auto *a = new QAction(QString::fromUtf8(modeNames[i]), this);
        a->setCheckable(true);
        a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key(Qt::Key_1 + i)));
        a->setShortcutContext(Qt::ApplicationShortcut);
        a->setToolTip(QString::fromUtf8(modeTips[i]));
        a->setStatusTip(a->toolTip());
        modeGrp->addAction(a);
        addAction(a);
        viewMenu->addAction(a);
        auto *b = new QToolButton(modeBox);
        b->setDefaultAction(a);
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        b->setFocusPolicy(Qt::NoFocus);
        modeLay->addWidget(b);
        m_modeActs[i] = a;
        connect(a, &QAction::triggered, this, [this, i] { setMode(MainWindow::Mode(i)); });
    }
    // Full screen as an icon too: [ + ] at the far right.
    m_fullScreenAct->setIcon(fullScreenIcon());
    auto *fsBtn = new QToolButton(this);
    fsBtn->setDefaultAction(m_fullScreenAct);
    fsBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    fsBtn->setIconSize(QSize(22, 22));
    fsBtn->setAutoRaise(true);
    fsBtn->setFocusPolicy(Qt::NoFocus);
    fsBtn->setToolTip(QStringLiteral("Full screen  (F11)"));

    // Brand in the middle: logo, name, version, and an update note when
    // GitHub has a newer one (checkForUpdate).
    auto *brand = new QWidget(this);
    auto *brandLay = new QHBoxLayout(brand);
    brandLay->setContentsMargins(0, 0, 0, 0);
    brandLay->setSpacing(8);
    auto *logo = new QLabel(brand);
    logo->setPixmap(brandLogo(24, devicePixelRatioF()));
    auto *name = new QLabel(QStringLiteral(
        "<span style='font-size:15px; font-weight:600'>phobi<span style='color:#f2a536'>CCCP</span></span>"
        "&nbsp;&nbsp;<span style='font-size:11px; color:#8b93a1'>v%1</span>")
        .arg(QLatin1String(PHOBICCCP_VERSION)), brand);
    name->setTextFormat(Qt::RichText);
    m_updateLabel = new QLabel(brand);
    m_updateLabel->setTextFormat(Qt::RichText);
    m_updateLabel->setOpenExternalLinks(false);
    connect(m_updateLabel, &QLabel::linkActivated, this,
            [this](const QString &link) { installUpdate(link.section(QLatin1Char(':'), 1)); });
    m_updateLabel->hide();
    // The open file, right of the version (it used to sit in the status
    // bar at the bottom, where nobody looked).
    m_fileLabel = new QLabel(brand);
    m_fileLabel->setTextFormat(Qt::PlainText);
    m_fileLabel->setStyleSheet(QStringLiteral("color: #c5cad3; font-size: 13px;"));
    brandLay->addWidget(logo);
    brandLay->addWidget(name);
    brandLay->addWidget(m_updateLabel);
    brandLay->addWidget(m_fileLabel);

    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("header"));
    header->setAttribute(Qt::WA_StyledBackground);
    header->setStyleSheet(QStringLiteral(
        "#header { background: #1e2127; border-bottom: 1px solid #2a2e36; }"));
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(4, 2, 10, 2);
    hl->setSpacing(10);
    hl->addWidget(m_menuBar);
    hl->addStretch(1);
    hl->addWidget(brand);
    hl->addStretch(1);
    hl->addWidget(modeBox);
    hl->addWidget(fsBtn);
    setMenuWidget(header);
    // Come back up in the mode the shop PC was left in.
    const int saved = QSettings().value(QStringLiteral("ui/mode"), 0).toInt();
    setMode(MainWindow::Mode(qBound(0, saved, 3)), false);
}

// [ + ]: square brackets with a plus inside, the full-screen toggle; while
// full screen it shows [ − ] instead (the icon's "On" state).
QIcon MainWindow::fullScreenIcon()
{
    auto draw = [](bool minus) {
        QPixmap pm(QSize(22, 22) * 3);
        pm.setDevicePixelRatio(3);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        QPen pen(QColor(0xd8, 0xdc, 0xe4), 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.drawPolyline(QPolygonF({QPointF(7, 3), QPointF(3, 3), QPointF(3, 19), QPointF(7, 19)}));
        p.drawPolyline(QPolygonF({QPointF(15, 3), QPointF(19, 3), QPointF(19, 19), QPointF(15, 19)}));
        p.drawLine(QPointF(7, 11), QPointF(15, 11));
        if (!minus)
            p.drawLine(QPointF(11, 7), QPointF(11, 15));
        return pm;
    };
    QIcon icon;
    icon.addPixmap(draw(false), QIcon::Normal, QIcon::Off);
    icon.addPixmap(draw(true), QIcon::Normal, QIcon::On);
    return icon;
}

// Once a session, a few seconds after the window is up: fetch the VERSION
// file on GitHub's main branch and, if it is newer than this build, say so
// next to the version in the header. Offline or blocked: silently nothing.
void MainWindow::checkForUpdate()
{
    auto *nam = new QNetworkAccessManager(this);
    QNetworkRequest req(QUrl(QStringLiteral(
        "https://raw.githubusercontent.com/phobicdotno/phobiCCCP/main/VERSION")));
    req.setTransferTimeout(8000);
    QNetworkReply *reply = nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, nam] {
        reply->deleteLater();
        nam->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return;
        const QString remote = QString::fromUtf8(reply->readAll()).trimmed();
        const QVersionNumber theirs = QVersionNumber::fromString(remote);
        const QVersionNumber ours = QVersionNumber::fromString(QLatin1String(PHOBICCCP_VERSION));
        if (theirs.isNull() || theirs <= ours)
            return;
        m_updateLabel->setText(QStringLiteral(
            "<a href='update:%1' "
            "style='color:#f2a536; text-decoration:none'>↑ Install v%1</a>").arg(remote));
        m_updateLabel->setToolTip(QStringLiteral("Click to install v%1 (this is v%2)")
                                      .arg(remote, QLatin1String(PHOBICCCP_VERSION)));
        m_updateLabel->show();
        statusBar()->showMessage(QStringLiteral("phobiCCCP v%1 is available: click it at the top to install").arg(remote), 15000);
        // What is new: the one-line summary of every release newer than this one.
        auto *nam2 = new QNetworkAccessManager(this);
        QNetworkRequest req2(QUrl(QStringLiteral(
            "https://raw.githubusercontent.com/phobicdotno/phobiCCCP/main/CHANGELOG.md")));
        req2.setTransferTimeout(8000);
        QNetworkReply *r2 = nam2->get(req2);
        connect(r2, &QNetworkReply::finished, this, [this, r2, nam2, ours] {
            r2->deleteLater();
            nam2->deleteLater();
            if (r2->error() != QNetworkReply::NoError)
                return;
            m_updateNotes.clear();
            for (const ReleaseSummary &rel : summarizeChangelog(QString::fromUtf8(r2->readAll()))) {
                if (QVersionNumber::fromString(rel.version) <= ours)
                    break;
                m_updateNotes << QStringLiteral("v%1 \u2014 %2").arg(rel.version,
                    rel.summary.isEmpty() ? rel.areas.join(QStringLiteral(", ")) : rel.summary);
            }
            if (!m_updateNotes.isEmpty())
                m_updateLabel->setToolTip(m_updateLabel->toolTip() + QStringLiteral("\n\n")
                                          + m_updateNotes.join(QLatin1Char('\n')));
        });
    });
}

// The update link at the top: get the new version from GitHub into the
// source folder this build came from, build it, install it and start it.
// Every step's output scrolls by in the dialog; a failed step stops there
// and leaves the running version untouched.
// Swallows Escape and close requests on the update dialog while one of its
// steps runs: the step's process is a child of the dialog.
class StepGuard : public QObject
{
public:
    StepGuard(std::shared_ptr<bool> running, QObject *parent) : QObject(parent), m_running(std::move(running)) {}
protected:
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (*m_running) {
            if (e->type() == QEvent::Close) {
                e->ignore();
                return true;
            }
            if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(e)->key() == Qt::Key_Escape)
                return true;
        }
        return QObject::eventFilter(o, e);
    }
private:
    std::shared_ptr<bool> m_running;
};

void MainWindow::installUpdate(const QString &version)
{
    const QString src = QStringLiteral(PHOBICCCP_SOURCE_DIR), build = QStringLiteral(PHOBICCCP_BUILD_DIR);
    if (!QFileInfo::exists(src + QStringLiteral("/.git")) || !QFileInfo::exists(build)) {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/phobicdotno/phobiCCCP")));
        return;
    }
    if (m_machine && m_machine->isBusy()) {
        shopTell(this, QStringLiteral("Update"), QStringLiteral("The machine is working"),
                 QStringLiteral("Install the update when the job is done: it restarts phobiCCCP."));
        return;
    }
    ShopAsk ask;
    ask.title = QStringLiteral("Update");
    ask.headline = QStringLiteral("Install phobiCCCP v%1?").arg(version);
    ask.body = (m_updateNotes.isEmpty() ? QString() : m_updateNotes.join(QLatin1Char('\n')) + QStringLiteral("\n\n"))
             + QStringLiteral("Downloads v%1, builds it (a few minutes) and restarts phobiCCCP. "
                              "You are asked to save unsaved changes before the restart.").arg(version);
    ask.okText = QStringLiteral("Install");
    ask.defaultOk = true;
    if (!shopAsk(this, ask))
        return;

    // One bar per stage; the build's "[ 42%]" lines drive its bar, the
    // others pulse while they run. The log is behind "Details".
    auto *dlg = new QDialog(this, Qt::Dialog | Qt::FramelessWindowHint);   // no title bar
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(QStringLiteral("Installing v%1").arg(version));
    dlg->setObjectName(QStringLiteral("updateDialog"));
    dlg->setStyleSheet(QStringLiteral("#updateDialog { border: 2px solid #f2a536; border-radius: 8px; }"));
    dlg->setFixedWidth(640);
    auto *v = new QVBoxLayout(dlg);
    v->setContentsMargins(24, 20, 24, 16);
    v->setSpacing(10);
    auto *head = new QLabel(dlg);
    head->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 600;"));
    v->addWidget(head);
    if (!m_updateNotes.isEmpty()) {
        auto *notes = new QLabel(m_updateNotes.join(QLatin1Char('\n')), dlg);
        notes->setStyleSheet(QStringLiteral("color: #aab1bd;"));
        notes->setWordWrap(true);
        v->addWidget(notes);
    }
    QHash<QProgressBar *, QLabel *> pctOf;   // the "42%" behind each bar
    const QString barStyle = QStringLiteral(
        "QProgressBar { background: #16181d; border: 1px solid #333945; border-radius: 6px;"
        " min-height: 24px; text-align: center; color: #d8dce4; font-size: 12px; }"
        "QProgressBar::chunk { background: #f2a536; border-radius: 5px; }");
    auto bar = [&](const QString &label) {
        auto *l = new QLabel(label, dlg);
        l->setStyleSheet(QStringLiteral("color: #aab1bd; margin-top: 6px;"));
        auto *b = new QProgressBar(dlg);
        b->setStyleSheet(barStyle);
        b->setRange(0, 100);
        b->setValue(0);
        b->setTextVisible(false);
        auto *pct = new QLabel(dlg);
        pct->setFixedWidth(44);
        pct->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        pct->setStyleSheet(QStringLiteral("color: #aab1bd;"));
        auto *h = new QHBoxLayout;
        h->addWidget(b, 1);
        h->addWidget(pct);
        v->addWidget(l);
        v->addLayout(h);
        pctOf.insert(b, pct);
        return b;
    };
    QProgressBar *dlBar = bar(QStringLiteral("Downloading"));
    QProgressBar *buildBar = bar(QStringLiteral("Compiling"));
    QProgressBar *instBar = bar(QStringLiteral("Installing"));
    auto *log = new QPlainTextEdit(dlg);
    log->setReadOnly(true);
    log->setMinimumHeight(220);
    log->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 11px;"));
    log->hide();
    auto *details = new QToolButton(dlg);
    details->setText(QStringLiteral("Details \u25BE"));
    details->setCheckable(true);
    details->setAutoRaise(true);
    connect(details, &QToolButton::toggled, dlg, [details, log, dlg](bool on) {
        details->setText(on ? QStringLiteral("Details \u25B4") : QStringLiteral("Details \u25BE"));
        log->setVisible(on);
        dlg->adjustSize();
    });
    auto *closeBtn = new QPushButton(QStringLiteral("Close"), dlg);
    closeBtn->setEnabled(false);
    // Close and Escape both take the dialog down for good, but only between
    // steps: the step's QProcess is a child of the dialog, so deleting it
    // would kill `git reset --hard` or the install half-way. While a step
    // runs Close is disabled and Escape / close requests are swallowed.
    auto stepRunning = std::make_shared<bool>(false);
    dlg->installEventFilter(new StepGuard(stepRunning, dlg));
    m_updateStep = stepRunning;   // and the window's own close
    auto takeDown = [dlg] { dlg->hide(); dlg->deleteLater(); };
    connect(closeBtn, &QPushButton::clicked, dlg, takeDown);
    connect(dlg, &QDialog::rejected, dlg, takeDown);
    auto *row = new QHBoxLayout;
    row->addWidget(details);
    row->addStretch(1);
    row->addWidget(closeBtn);
    v->addSpacing(6);
    v->addLayout(row);
    v->addWidget(log);
    dlg->show();
    const QList<QProgressBar *> bars = {dlBar, dlBar, buildBar, instBar};
    auto pulse = [pctOf](QProgressBar *b) { b->setRange(0, 0); pctOf[b]->setText(QStringLiteral("\u2026")); };
    auto done = [pctOf](QProgressBar *b) { b->setRange(0, 100); b->setValue(100); pctOf[b]->setText(QStringLiteral("100%")); };

    struct Step { QString what, prog; QStringList args; QString fallbackProg; QStringList fallbackArgs; };
    const QList<Step> steps = {
        {QStringLiteral("Downloading"), QStringLiteral("git"), {QStringLiteral("-C"), src, QStringLiteral("fetch"), QStringLiteral("origin"), QStringLiteral("main")}, {}, {}},
        {QStringLiteral("Downloading"), QStringLiteral("git"), {QStringLiteral("-C"), src, QStringLiteral("reset"), QStringLiteral("--hard"), QStringLiteral("origin/main")}, {}, {}},
        {QStringLiteral("Building"), QStringLiteral("cmake"), {QStringLiteral("--build"), build, QStringLiteral("-j4")}, {}, {}},
        {QStringLiteral("Installing"), QStringLiteral("sudo"), {QStringLiteral("-n"), QStringLiteral("cmake"), QStringLiteral("--install"), build},
         QStringLiteral("pkexec"), {QStringLiteral("cmake"), QStringLiteral("--install"), build}},
    };
    auto run = std::make_shared<std::function<void(int, bool)>>();
    *run = [=](int i, bool fallback) {
        if (i == steps.size()) {
            head->setText(QStringLiteral("v%1 installed \u2014 restarting").arg(version));
            closeBtn->setEnabled(true);
            *stepRunning = false;
            const QString file = m_untitled ? QString() : m_doc.filePath();
            // Close is live during the pause: the dialog may be gone by now.
            QTimer::singleShot(800, this, [this, file, guard = QPointer<QDialog>(dlg)] {
                if (guard)
                    guard->close();
                if (!close())            // save prompt cancelled: stay, new version next start
                    return;
                QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                        file.isEmpty() ? QStringList() : QStringList{file});
                qApp->quit();
            });
            return;
        }
        const Step &s = steps[i];
        head->setText(QStringLiteral("%1 v%2\u2026").arg(s.what, version));
        QProgressBar *b = bars[i];
        if (b == buildBar) {
            b->setRange(0, 100);
            b->setValue(0);
        } else {
            pulse(b);
        }
        *stepRunning = true;
        auto *p = new QProcess(dlg);
        p->setProcessChannelMode(QProcess::MergedChannels);
        connect(p, &QProcess::readyRead, log, [p, log, b, buildBar, pctOf] {
            const QString text = QString::fromLocal8Bit(p->readAll());
            log->appendPlainText(text.trimmed());
            if (b != buildBar)
                return;
            static const QRegularExpression pct(QStringLiteral("\\[\\s*(\\d+)%\\]"));
            auto it = pct.globalMatch(text);
            while (it.hasNext()) {
                b->setValue(it.next().captured(1).toInt());
                pctOf[b]->setText(QStringLiteral("%1%").arg(b->value()));
            }
        });
        connect(p, &QProcess::finished, dlg, [=](int code, QProcess::ExitStatus st) {
            p->deleteLater();
            if (st == QProcess::NormalExit && code == 0) {
                done(b);
                (*run)(i + 1, false);
                return;
            }
            if (!fallback && !s.fallbackProg.isEmpty()) {
                (*run)(i, true);
                return;
            }
            *stepRunning = false;
            b->setRange(0, 100);
            head->setText(QStringLiteral("%1 failed \u2014 nothing was changed").arg(s.what));
            details->setChecked(true);
            closeBtn->setEnabled(true);
            closeBtn->setDefault(true);
            closeBtn->setFocus();
        });
        connect(p, &QProcess::errorOccurred, dlg, [=](QProcess::ProcessError e) {
            if (e != QProcess::FailedToStart)
                return;
            p->deleteLater();
            if (!fallback && !s.fallbackProg.isEmpty()) {
                (*run)(i, true);
                return;
            }
            *stepRunning = false;
            b->setRange(0, 100);
            head->setText(QStringLiteral("%1 failed: %2 not found").arg(s.what, s.prog));
            details->setChecked(true);
            closeBtn->setEnabled(true);
        });
        log->appendPlainText(QStringLiteral("$ %1 %2").arg(fallback ? s.fallbackProg : s.prog,
                                                          (fallback ? s.fallbackArgs : s.args).join(QLatin1Char(' '))));
        p->start(fallback ? s.fallbackProg : s.prog, fallback ? s.fallbackArgs : s.args);
    };
    (*run)(0, false);
}

// Three tool bars on one row need about 1 500 px; the shop screen has
// 1 280. When they do not fit, the Vectors bar gets a row of its own.
void MainWindow::resizeEvent(QResizeEvent *e)
{
    QMainWindow::resizeEvent(e);
    QToolBar *vectors = nullptr;
    int need = 0;
    for (QToolBar *b : findChildren<QToolBar *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (!b->isHidden())
            need += b->sizeHint().width();
        if (b->windowTitle() == QLatin1String("Vectors"))
            vectors = b;
    }
    if (m_mode == Mode::Design || m_mode == Mode::Toolpaths)
        applySplits();
    if (!vectors)
        return;
    const bool wrap = need > width() - 20;
    if (wrap != m_vectorsWrapped) {
        m_vectorsWrapped = wrap;
        if (wrap)
            insertToolBarBreak(vectors);
        else
            removeToolBarBreak(vectors);
    }
}

void MainWindow::applySplits()
{
    // Before the first layout the splitter is still 100 px wide: size from
    // the window instead, which is what it will get.
    const int w = m_canvasPage->width() > 200 ? m_canvasPage->width() : width();
    QList<int> cs = m_canvasSizes;
    if (cs.size() != 2 || cs[0] + cs[1] < 200) {
        const int right = qBound(320, int(w * 0.30), 520);
        cs = {qMax(200, w - right), right};
    }
    m_canvasPage->setSizes(cs);
    const int h = m_designPage->height() > 200 ? m_designPage->height() : height() - 160;
    QList<int> ds = m_designSizes;
    if (ds.size() != 2 || ds[0] + ds[1] < 100)
        ds = {int(h * 0.45), h - int(h * 0.45)};
    m_designPage->setSizes(ds);
}

void MainWindow::showParameters()
{
    setMode(Mode::Design, false);
    // scrolled(): the panel sits in a scroll area's viewport.
    m_docTabs->setCurrentWidget(m_parameters->parentWidget()->parentWidget());
}

void MainWindow::setMode(Mode m, bool autoRun)
{
    m_mode = m;
    if (m_modeActs[int(m)] && !m_modeActs[int(m)]->isChecked())
        m_modeActs[int(m)]->setChecked(true);
    const bool design = m == Mode::Design, tp = m == Mode::Toolpaths;
    QWidget *page = m_canvasPage;
    if (m == Mode::Simulate)
        page = m_simPage;
    else if (m == Mode::Machine)
        page = m_machine;
    m_stack->setCurrentWidget(page);
    // Design: drawing tools and vector actions. Toolpaths: the toolpath
    // kinds instead. Options (snap, fit, preview, undo) on both.
    for (QToolBar *b : findChildren<QToolBar *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (b == m_tpBar)
            b->setVisible(tp);
        else if (b == m_palette || b->windowTitle() == QLatin1String("Vectors"))
            b->setVisible(design);
        else
            b->setVisible(design || tp);
    }
    m_rightStack->setCurrentWidget(design ? m_designPage : m_tpPage);
    m_canvas->setParametricVisible(design);
    if (design || tp)
        applySplits();
    if (tp) {
        // Toolpaths are what this mode is about: show them on the canvas.
        if (autoRun && !m_previewAct->isChecked())
            m_previewAct->setChecked(true);
    } else if (m == Mode::Simulate && autoRun) {
        if (m_isoStale)
            refreshIso();
        m_sim->simulate();
    } else if (m == Mode::Machine) {
        m_machine->setFocus();            // arrow keys jog straight away
    }
    QSettings().setValue(QStringLiteral("ui/mode"), int(m));
}

void MainWindow::onExportGcode()
{
    commitEdits();
    ++m_holdDoc;
    const auto hold = qScopeGuard([this] { --m_holdDoc; });
    if (m_doc.filePath().isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Export G-code"),
                                 QStringLiteral("Open a .c2d file first."));
        return;
    }
    const GcodeResult r = exportWithProgress(this, m_doc, QStringLiteral("Export G-code"));
    if (r.cancelled) {
        statusBar()->showMessage(QStringLiteral("Export cancelled"), 5000);
        return;                       // a partial program must never be written
    }
    if (r.done.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Export G-code"),
            QStringLiteral("No exportable toolpaths.\n\nSupported: contour, pocket, "
                           "cutout, drill, keyhole, texture, v-carve, engrave.\n"
                           "Skipped:\n  %1")
                .arg(r.skipped.join(QStringLiteral("\n  "))));
        return;
    }
    // A document with tiling switched on whose job is taller than one tile is
    // meant to be cut in pieces: offer the tiled export.
    const double tileH = m_doc.params().value("tile_height", "0").toDouble();
    if (m_doc.params().value("tiling_enabled") == QLatin1String("1") && tileH > 0
        && tileCount(r.ops, tileH) > 1) {
        const auto ans = QMessageBox::question(
            this, QStringLiteral("Export G-code"),
            QStringLiteral("Tiling is enabled in this document and the job needs %1 "
                           "tiles of %2 mm.\n\nExport one program per tile?")
                .arg(tileCount(r.ops, tileH)).arg(tileH),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Yes);
        if (ans == QMessageBox::Cancel)
            return;
        if (ans == QMessageBox::Yes) {
            onExportGcodeTiled();
            return;
        }
    }
    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export G-code"),
        QFileInfo(m_doc.filePath()).completeBaseName() + QStringLiteral(".nc"),
        QStringLiteral("G-code (*.nc *.gcode);;All files (*)"));
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("Export failed"), f.errorString());
        return;
    }
    // Unchecked and unflushed, a full disk or a stick pulled mid-write left a
    // program ending in the middle of a block - no retract, no spindle stop -
    // and the status bar said it had been exported.
    const QByteArray bytes = r.gcode.toUtf8();
    if (f.write(bytes) != bytes.size() || !f.flush()) {
        QMessageBox::warning(this, QStringLiteral("Export failed"), f.errorString());
        f.close();
        return;
    }
    f.close();
    QString msg = QStringLiteral("Exported %1 toolpath(s) to %2   [%3]")
                      .arg(r.done.size()).arg(path)
                      .arg(statsSummary(computeStats(r.ops))
                               .replace(QChar('\n'), QStringLiteral("  ·  ")));
    if (!r.skipped.isEmpty())
        msg += QStringLiteral("  (skipped: %1)").arg(r.skipped.join(QStringLiteral(", ")));
    if (!r.notes.isEmpty())
        msg += QStringLiteral("  (%1)").arg(r.notes.join(QStringLiteral("; ")));
    statusBar()->showMessage(msg, 8000);
}

void MainWindow::onExportGcodeTiled()
{
    commitEdits();
    ++m_holdDoc;
    const auto hold = qScopeGuard([this] { --m_holdDoc; });
    if (m_doc.filePath().isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Export G-code (tiled)"),
                                 QStringLiteral("Open a .c2d file first."));
        return;
    }
    const double docTile = m_doc.params().value("tile_height", "508.0").toDouble();
    bool ok = false;
    const double tileH = QInputDialog::getDouble(
        this, QStringLiteral("Export G-code (tiled)"),
        QStringLiteral("Tile height (mm): the Y extent one program may use.\n"
                       "Tile k holds the cuts with Y in [k·h, (k+1)·h), shifted down\n"
                       "by k·h — slide the stock forward by h between tiles."),
        docTile > 0 ? docTile : 508.0, 10.0, 10000.0, 1, &ok);
    if (!ok)
        return;
    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export tiled G-code — base name (_tile1.nc, _tile2.nc, … are added)"),
        QFileInfo(m_doc.filePath()).completeBaseName() + QStringLiteral(".nc"),
        QStringLiteral("G-code (*.nc *.gcode);;All files (*)"));
    if (path.isEmpty())
        return;
    for (const char *suffix : {".nc", ".gcode"})
        if (path.endsWith(QLatin1String(suffix), Qt::CaseInsensitive))
            path.chop(int(qstrlen(suffix)));
    const TiledExport r = exportTiledWithProgress(this, m_doc, path, tileH,
                                                  QStringLiteral("Export G-code (tiled)"));
    if (r.error == QLatin1String("cancelled")) {
        statusBar()->showMessage(QStringLiteral("Export cancelled"), 5000);
        return;
    }
    if (!r.error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Export failed"),
            QStringLiteral("%1\nSkipped: %2").arg(r.error, r.skipped.join(QStringLiteral(", "))));
        return;
    }
    QString msg = QStringLiteral("Exported %1 toolpath(s) as %2 tile(s) of %3 mm: %4")
                      .arg(r.done.size()).arg(r.files.size()).arg(tileH)
                      .arg(r.files.join(QStringLiteral(", ")));
    if (!r.skipped.isEmpty())
        msg += QStringLiteral("  (skipped: %1)").arg(r.skipped.join(QStringLiteral(", ")));
    statusBar()->showMessage(msg, 10000);
    // A previous export under this base name may have needed more tiles. Those
    // files are still there, still look like part of this job, and still say
    // "(tile 4/5)" inside. The save dialog only asked about <base>.nc, which is
    // never written, so the user was never warned about any of them.
    if (!r.stale.isEmpty())
        QMessageBox::warning(
            this, QStringLiteral("Older tiles are still in that folder"),
            QStringLiteral("This export made %1 tile(s). These files are left over "
                           "from an earlier, longer export and are NOT part of it:\n\n%2\n\n"
                           "Delete them before running the job.")
                .arg(r.files.size())
                .arg(r.stale.join(QStringLiteral("\n"))));
}

// One background preview export: what the worker reads and writes. The
// document and the relief are copies taken on the interface thread, as
// exportWithProgress takes them; the worker touches nothing else.
struct MainWindow::PreviewRun {
    quint64 gen = 0;
    Document doc;
    HeightModel relief;
    std::atomic<bool> cancel{false};
    GcodeResult result;
    QThread *thread = nullptr;
};

MainWindow::~MainWindow()
{
    // A preview export still running reads its own snapshot only, but the
    // process must not tear down under it.
    if (m_previewRun) {
        m_previewRun->cancel.store(true);
        m_previewRun->thread->wait();
        delete m_previewRun->thread;
        m_previewRun.reset();
    }
}

void MainWindow::refreshPreview(bool interactive)
{
    if (m_doc.filePath().isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Open a .c2d file first"));
        return;
    }
    if (interactive) {
        // Every edit with Preview on lands here. Exporting on this thread
        // (or behind a modal dialog) froze the window for as long as the
        // job takes to plan - tens of seconds with an adaptive pocket - on
        // each keystroke. Schedule it instead; the run in flight, if any, is
        // for an older document now, so it is asked to stop.
        ++m_previewGen;
        m_isoStale = true;            // Simulate rebuilds now if entered before it lands
        if (m_previewRun)
            m_previewRun->cancel.store(true);
        m_previewTimer->start();
        return;
    }
    dropPreviewRequests();
    const GcodeResult r = exportGcode(m_doc);
    applyPreview(r, true);
}

void MainWindow::dropPreviewRequests()
{
    ++m_previewGen;
    m_previewServed = m_previewGen;
    m_previewTimer->stop();
    if (m_previewRun)
        m_previewRun->cancel.store(true);
}

void MainWindow::startPreviewRun()
{
    if (m_previewServed == m_previewGen || m_doc.filePath().isEmpty())
        return;
    auto run = std::make_shared<PreviewRun>();
    run->gen = m_previewGen;
    // heightModelFor() composites into the model store, which the model
    // panel's own rebuild writes too: only this thread may call it.
    if (const HeightModel *live = heightModelFor(m_doc))
        run->relief = *live;
    run->doc = m_doc;
    run->thread = QThread::create([run] {
        ExportWatch watch;
        watch.step = [&run](int, int, const QString &) { return !run->cancel.load(); };
        run->result = exportGcode(run->doc, &watch, &run->relief);
    });
    m_previewRun = run;
    connect(run->thread, &QThread::finished, this, [this, run] {
        run->thread->wait();
        run->thread->deleteLater();
        run->thread = nullptr;
        if (m_previewRun == run)
            m_previewRun.reset();
        if (run->gen == m_previewGen && !run->cancel.load() && !run->result.cancelled) {
            m_previewServed = run->gen;
            applyPreview(run->result, m_previewAct->isChecked());
        } else if (m_previewServed != m_previewGen && !m_previewTimer->isActive()) {
            startPreviewRun();        // a newer edit came in while this one ran
        }
    });
    statusBar()->showMessage(QStringLiteral("Updating preview…"));
    run->thread->start();
}

void MainWindow::applyPreview(const GcodeResult &r, bool overlay)
{
    if (overlay)
        m_canvas->setToolpathPreview(r.ops);
    m_iso->setJob(r.ops, m_doc.boardWidth(), m_doc.boardHeight(),
                  m_doc.params().value("thickness").toDouble());
    m_sim->setJob(r.ops, toolGeometry(m_doc), m_doc.boardWidth(), m_doc.boardHeight(),
                  m_doc.params().value("thickness").toDouble(), heightModelFor(m_doc));
    m_toolStrip->setJob(m_doc, r.ops);
    m_isoStale = false;
    if (!overlay)
        return;
    QString msg = QStringLiteral("Preview: %1 toolpath(s)").arg(r.done.size());
    if (!r.skipped.isEmpty())
        msg += QStringLiteral(" — skipped: %1").arg(r.skipped.join(QStringLiteral(", ")));
    statusBar()->showMessage(msg, 6000);
}

void MainWindow::showMachinePanel(const QString &page)
{
    setMode(Mode::Machine, false);
    if (!page.isEmpty())
        m_machine->showPage(page);
}

void MainWindow::showToolpathPreview()
{
    m_previewAct->setChecked(true);
    refreshPreview(false);         // --shot screenshots straight after
    setMode(Mode::Toolpaths, false);
}

void MainWindow::refreshIso(bool interactive)
{
    if (m_isoBusy) {
        // Called from inside our own export's event loop: the program being
        // built is already out of date. Let the running call build again.
        m_isoStale = true;
        m_isoRerun = true;
        return;
    }
    m_isoBusy = true;
    for (;;) {
        m_isoRerun = false;
        m_isoStale = false;
        if (m_doc.filePath().isEmpty())
            break;
        // This builds the current document, so a background preview export
        // still pending or running is for it (or older): drop it, and put
        // this result on the canvas too when Preview is on.
        const quint64 gen = m_previewGen, served = m_previewServed;
        dropPreviewRequests();
        const GcodeResult r = interactive
            ? exportWithProgress(this, m_doc, QStringLiteral("Updating 3D preview"))
            : exportGcode(m_doc);
        if (r.cancelled) {
            if (served != gen)            // the edit still needs its preview
                refreshPreview();
            else
                m_isoStale = true;
            statusBar()->showMessage(QStringLiteral("Preview update cancelled"), 5000);
            break;
        }
        if (m_isoRerun) {
            // The document changed while it was built: this program is for
            // neither state. Build again while Simulate still shows it;
            // otherwise setMode catches up when Simulate is entered.
            if (m_mode == Mode::Simulate)
                continue;
            m_isoStale = true;
            break;
        }
        applyPreview(r, m_previewAct->isChecked());
        break;
    }
    m_isoBusy = false;
}

void MainWindow::showSimulation()
{
    setMode(Mode::Simulate, false);
    if (m_isoStale)
        refreshIso(false);         // --shot: no dialog, the grab follows
    m_sim->simulateBlocking();   // --shot … simulation: result is in the grab
    // --shot is the only headless way into the simulator: say on the console
    // what the status line says, for scripts that render a check image.
    const SimResult &sr = m_sim->result();
    if (sr.gougeCells > 0)
        qInfo().noquote() << QStringLiteral("simulation: gouge, %1 mm² into the 3D model, up to %2 mm deep")
                                 .arg(sr.gougeArea, 0, 'f', 2).arg(sr.gougeDepth, 0, 'f', 3);
    if (sr.rapidHitCount > 0)
        qInfo().noquote() << QStringLiteral("simulation: %1 rapid(s) into stock, worst %2 mm at X%3 Y%4 Z%5 (%6)")
                                 .arg(sr.rapidHitCount).arg(sr.worstRapid.depth, 0, 'f', 3)
                                 .arg(sr.worstRapid.x, 0, 'f', 2).arg(sr.worstRapid.y, 0, 'f', 2)
                                 .arg(sr.worstRapid.z, 0, 'f', 2).arg(sr.worstRapid.op);
}

void MainWindow::showModel()
{
    setMode(Mode::Design, false);
    m_docTabs->setCurrentIndex(1);
    m_model->rebuildBlocking();   // --shot … model: the relief is in the grab
}

void MainWindow::markIsoStale()
{
    // A preview export from before this change must not land as the
    // current job (Preview was turned off, or another file opened).
    dropPreviewRequests();
    m_isoStale = true;
    if (m_isoBusy)
        m_isoRerun = true;            // the program being built is for the old state
    if (m_mode != Mode::Simulate || m_isoQueued)
        return;
    // Simulate shows the program, so rebuild it - but not here. This runs
    // inside change notifications, among them QUndoCommand::undo(), and
    // refreshIso's progress dialog runs the event loop: a second Ctrl+Z
    // taken there undid the same command again (QUndoStack moves its index
    // only after undo() returns). Back in the main loop, edits that came in
    // together are built once.
    m_isoQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_isoQueued = false;
        if (m_mode == Mode::Simulate && m_isoStale)
            refreshIso();
    });
}

void MainWindow::updateTitle()
{
    QString t = QStringLiteral("phobiCCCP");
    if (m_untitled)
        t = (m_untitledFrom.isEmpty() ? QStringLiteral("Untitled") : QFileInfo(m_untitledFrom).fileName())
            + (m_dirty ? QStringLiteral(" *") : QString()) + QStringLiteral(" — phobiCCCP");
    else if (!m_doc.filePath().isEmpty())
        t = QFileInfo(m_doc.filePath()).fileName() + (m_dirty ? QStringLiteral(" *") : QString())
            + QStringLiteral(" — phobiCCCP");
    setWindowTitle(t);
    if (m_fileLabel) {
        const QString name = t.section(QStringLiteral(" — "), 0, 0);
        m_fileLabel->setText(t.contains(QStringLiteral(" — "))
                                 ? QStringLiteral("·   %1").arg(name)
                                 : QString());
        // A file is open: the start-up hint no longer applies.
        if (t.contains(QStringLiteral(" — "))
            && statusBar()->currentMessage() == QLatin1String("Open a .c2d file to begin"))
            statusBar()->clearMessage();
        m_fileLabel->setToolTip(m_doc.filePath());
    }
}

// A .c2d to write. The dialog asks before replacing the name as typed, but
// not when ".c2d" is added to it afterwards, so that is asked here.
static QString askC2dSavePath(QWidget *parent, const QString &title, const QString &start)
{
    QString path = QFileDialog::getSaveFileName(parent, title, start, QStringLiteral("Carbide Create (*.c2d)"));
    if (path.isEmpty() || path.endsWith(QStringLiteral(".c2d"), Qt::CaseInsensitive))
        return path;
    path += QStringLiteral(".c2d");
    if (QFileInfo::exists(path)
        && QMessageBox::question(parent, title,
                                 QStringLiteral("%1 already exists.\nDo you want to replace it?")
                                     .arg(QFileInfo(path).fileName()),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
               != QMessageBox::Yes)
        return {};
    return path;
}

void MainWindow::onNew()
{
    if (!confirmDiscard(QStringLiteral("Save before starting a new file?")))
        return;
    bool ok = false;
    QSettings st;
    const auto machine = appsettings::machine(st);
    const double w = QInputDialog::getDouble(this, QStringLiteral("New design"),
        QStringLiteral("Stock width (mm):"), machine.width, 1, 5000, 2, &ok);
    if (!ok) return;
    const double h = QInputDialog::getDouble(this, QStringLiteral("New design"),
        QStringLiteral("Stock height (mm):"), machine.height, 1, 5000, 2, &ok);
    if (!ok) return;
    const double t = QInputDialog::getDouble(this, QStringLiteral("New design"),
        QStringLiteral("Stock thickness (mm):"), appsettings::kBlankThickness, 0.1, 500, 2, &ok);
    if (!ok) return;
    const QString path = askC2dSavePath(this, QStringLiteral("Save new design as"), QStringLiteral("untitled.c2d"));
    if (path.isEmpty())
        return;
    QString err;
    if (!c2d::Document::createBlank(path, w, h, t, &err)) {
        QMessageBox::warning(this, QStringLiteral("New failed"), err);
        return;
    }
    m_dirty = false;   // nothing to discard: the question was already asked
    openFile(path);
}

void MainWindow::onOpen()
{
    // Start where the last design came from, not in the home folder.
    QString dir;
    const QStringList recent = QSettings().value(QStringLiteral("recentFiles")).toStringList();
    for (const QString &r : recent)
        if (QFileInfo::exists(r)) {
            dir = QFileInfo(r).absolutePath();
            break;
        }
#ifdef Q_OS_ANDROID
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/phobiCCCP");
#endif
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open"), dir,
        QStringLiteral("Designs and vectors (*.c2d *.cccp *.svg *.dxf *.plt *.hpgl *.hpg *.hgl *.pdf *.ai *.eps *.ps);;"
                       "Syntexophobic part (*.cccp);;"
                       "Carbide Create (*.c2d);;") + importFileFilter()
            + QStringLiteral(";;All files (*)"));
    if (!path.isEmpty())
        openFile(path);
}

void MainWindow::onSave()
{
    commitEdits();
    if (m_untitled || m_doc.filePath().isEmpty()) {
        onSaveAs();
        return;
    }
    QString err;
    if (!m_doc.save(m_doc.filePath(), &err)) {
        QMessageBox::warning(this, QStringLiteral("Save failed"), err);
    } else {
        saveExtras(m_doc.filePath());
        padShareSaved(m_doc.filePath());
    }
}

// The relief and the background live in the same container but are written
// after Document::save has already renamed the finished file into place, so
// they are a second, unprotected pass. Both return bool and both were being
// discarded: a failure there meant the user was told "Saved", the dirty flag
// was cleared, and the 3D model they had just sculpted was silently not in
// the file. Report it, and leave the document dirty so the next Ctrl+S retries.
void MainWindow::saveExtras(const QString &path)
{
    QString bgErr, mdErr;
    const bool bgOk = m_bg.saveTo(path, &bgErr);
    const bool mdOk = m_model->saveTo(path, &mdErr);
    if (!bgOk || !mdOk) {
        QMessageBox::warning(
            this, QStringLiteral("Saved incompletely"),
            QStringLiteral("%1 was written, but %2 could not be:\n%3")
                .arg(path,
                     !mdOk ? QStringLiteral("the 3D model")
                           : QStringLiteral("the background image"),
                     !mdOk ? mdErr : bgErr));
        updateTitle();
        return;                       // still dirty: the next save retries
    }
    m_dirty = false;
    m_recovery.discard();
    updateTitle();
    statusBar()->showMessage(QStringLiteral("Saved %1").arg(path), 5000);
}

void MainWindow::onSaveAs()
{
    commitEdits();
    if (m_doc.filePath().isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Nothing to save"),
                                 QStringLiteral("Open a .c2d file first."));
        return;
    }
    const QFileInfo from(m_untitled ? m_untitledFrom : m_doc.filePath());
    QString startAt = from.fileName().isEmpty() ? QString()
                      : from.absolutePath() + QLatin1Char('/') + from.completeBaseName() + QStringLiteral(".c2d");
#ifdef Q_OS_ANDROID
    // Never into the app's cache, where an untitled design lives.
    if (m_untitled || startAt.isEmpty())
        startAt = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/phobiCCCP/")
                  + (from.fileName().isEmpty() ? QStringLiteral("Design") : from.completeBaseName())
                  + QStringLiteral(".c2d");
#endif
    const QString path = askC2dSavePath(this, QStringLiteral("Save Carbide Create file"), startAt);
    if (path.isEmpty())
        return;

    QString err;
    if (!m_doc.save(path, &err)) {
        QMessageBox::warning(this, QStringLiteral("Save failed"), err);
    } else {
        // save() has moved the document onto `path`; the scratch file of an
        // untitled design is no longer needed.
        if (m_untitled) {
            m_untitled = false;
            removeUntitledFile();
        }
        rememberRecent(path);          // a titled design's new name belongs in Open Recent too
        saveExtras(path);
        padShareSaved(path);
    }
}

// m_dirty was tracked and shown in the title bar but never acted on: opening
// another document, picking one from Open Recent, dropping a file on the
// window or closing it all discarded an afternoon of edits without a word.
// A number typed into a field takes effect on Enter or when the field loses
// focus, and reaches the design through a queued call: a shortcut (Ctrl+S,
// Ctrl+G) or closing the window would otherwise work on the design without
// it, and it would land afterwards. Commit it first.
void MainWindow::commitEdits()
{
    const QPointer<QWidget> f = QApplication::focusWidget();
    if (f && isAncestorOf(f)) {
        f->clearFocus();
        // Other queued calls run here too: a pad's request is refused as busy
        // rather than opening a design under the save or export that called this.
        ++m_holdDoc;
        const auto hold = qScopeGuard([this] { --m_holdDoc; });
        for (int i = 0; i < 3; ++i)
            QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        if (f)
            f->setFocus();
    }
}

bool MainWindow::typingPending() const
{
    QWidget *f = QApplication::focusWidget();
    if (!f || !isAncestorOf(f))
        return false;
    if (f->property("typed").toBool())
        return true;
    auto *le = qobject_cast<QLineEdit *>(f);
    if (!le)
        if (auto *sb = qobject_cast<QAbstractSpinBox *>(f))
            le = sb->findChild<QLineEdit *>();
    return le && le->isModified();
}

bool MainWindow::confirmDiscard(const QString &what)
{
    commitEdits();
    if (!m_dirty)
        return true;
    const QString name = m_untitled ? QStringLiteral("The untitled design")
                       : m_doc.filePath().isEmpty()
                             ? QStringLiteral("This document")
                             : QFileInfo(m_doc.filePath()).fileName();
    const auto r = QMessageBox::warning(
        this, QStringLiteral("Unsaved changes"),
        QStringLiteral("%1 has unsaved changes.\n\n%2").arg(name, what),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);
    if (r == QMessageBox::Cancel)
        return false;
    if (r == QMessageBox::Save) {
        onSave();
        return !m_dirty;              // a failed save must not lose the edits
    }
    return true;
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    commitEdits();
    // Never close in the middle of a cut, a probe or a homing cycle: the
    // serial link goes with the window, and the machine is left mid-move.
    // Quietly: a popup over a running job is exactly what the operator must
    // not get (the close request may well have come from a script).
    if (m_machine && m_machine->isBusy()) {
        e->ignore();
        statusBar()->showMessage(QStringLiteral("Not closing: the machine is busy. Let the job "
                                                "finish, or stop it, before closing phobiCCCP."), 10000);
        return;
    }
    // The update dialog is a child of the window and its step's process a
    // child of the dialog: closing now would kill `git reset --hard`, the
    // build or the install half-way. The dialog's own Close waits too.
    if (m_updateStep && *m_updateStep) {
        e->ignore();
        statusBar()->showMessage(QStringLiteral("Not closing: an update is being installed. Wait "
                                                "for the step to finish."), 10000);
        return;
    }
    if (confirmDiscard(QStringLiteral("Save before closing?"))) {
        removeUntitledFile();
        m_recovery.discard();         // saved, or the edits were let go
        e->accept();
    }
    else
        e->ignore();
}

void MainWindow::openFile(const QString &path)
{
    // A Syntexophobic project opens as its final product only: the sketches
    // as vectors and the finished part as an STL model, on a new design.
    if (isCccpFile(path)) {
        const CccpProduct p = readCccp(path);
        if (!p.ok) {
            QMessageBox::warning(this, QStringLiteral("Open"), p.error);
            return;
        }
        if (!confirmDiscard(QStringLiteral("Save before opening another file?")))
            return;
        const bool wasDirty = m_dirty;
        m_dirty = false;
        // The stock: the part plus 10 mm all round, as thick as the part.
        const QJsonArray size = p.info.value(QStringLiteral("size")).toArray();
        constexpr double kMargin = 10.0;
        const bool started = size.size() == 3 && size[0].toDouble() > 0 && size[1].toDouble() > 0
            ? newUntitled(size[0].toDouble() + 2 * kMargin, size[1].toDouble() + 2 * kMargin, size[2].toDouble())
            : newUntitled();
        // Never import into the design the user was leaving.
        if (!started) {
            m_dirty = wasDirty;
            return;
        }
        if (!p.svg.isEmpty()) {
            QTemporaryDir tmp;
            // The name comes from the file: "Bracket A/B" or "../../x" must
            // not become a path outside the temporary folder.
            const QString svg = tmp.filePath(safeFileName(p.name) + QStringLiteral(".svg"));
            QFile f(svg);
            if (tmp.isValid() && f.open(QIODevice::WriteOnly) && f.write(p.svg) == p.svg.size()) {
                f.close();
                // Not auto-placed: the drawing lines up with the part, which
                // is put at (kMargin, kMargin) below, even when it is bigger
                // than the stock.
                importVectorFile(this, m_canvas, &m_doc, svg, {}, false);
            } else {
                QMessageBox::warning(this, QStringLiteral("Open"),
                                     QStringLiteral("The drawing in %1 could not be imported.")
                                         .arg(QFileInfo(path).fileName()));
            }
        }
        QString err;
        if (!p.stl.isEmpty() && !m_model->addStlMesh(p.stl, p.name + QStringLiteral(".stl"), kMargin, kMargin, &err))
            QMessageBox::warning(this, QStringLiteral("Open"),
                                 QStringLiteral("The part in %1 could not be read: %2")
                                     .arg(QFileInfo(path).fileName(), err));
        m_untitledFrom = path;
        rememberRecent(path);
        updateTitle();
        statusBar()->showMessage(QStringLiteral("%1: the finished part from Syntexophobic").arg(p.name), 8000);
        return;
    }
    // A vector file opens as a new design (the machine's bed) with the
    // drawing imported into it; Save asks for a name.
    if (isImportableFile(path)) {
        if (!confirmDiscard(QStringLiteral("Save before opening another file?")))
            return;
        const bool wasDirty = m_dirty;
        m_dirty = false;
        if (!newUntitled()) {   // never import into the design the user was leaving
            m_dirty = wasDirty;
            return;
        }
        importVectorFile(this, m_canvas, &m_doc, path);
        m_untitledFrom = path;         // shown in the header; Save As offers name.c2d
        rememberRecent(path);          // a DXF you keep coming back to belongs in Open Recent too
        updateTitle();
        return;
    }
    openPath(path, false);
}

void MainWindow::openPath(const QString &path, bool untitled)
{
    if (!confirmDiscard(QStringLiteral("Save before opening another file?")))
        return;
    qDebug() << "[diag] SQL drivers:" << QSqlDatabase::drivers();
    qDebug() << "[diag] opening:" << path;
    QString err;
    // load() clears the document before it validates anything, so loading
    // straight into m_doc and returning on failure left the panels, the canvas
    // and the undo stack pointing at a document that no longer had any
    // contents - and m_doc.filePath() already pointing at the file that failed.
    // The next drag rebuilt an empty drawing; the next Ctrl+S wrote it out.
    // Load into a temporary and commit only once it worked.
    Document next;
    if (!next.load(path, &err)) {
        qDebug() << "[diag] LOAD FAILED:" << err;
        QMessageBox::warning(this, QStringLiteral("Open failed"), err);
        return;
    }
    m_doc = std::move(next);
    m_doc.setParam(QStringLiteral("site_order"),
                   QSettings().value(QStringLiteral("export/siteOrder"), false).toBool()
                       ? QStringLiteral("1") : QStringLiteral("0"));
    qDebug() << "[diag] loaded elements:" << m_doc.elements().size()
             << "toolpaths:" << m_doc.toolpaths().size()
             << "board:" << m_doc.boardWidth() << "x" << m_doc.boardHeight()
             << "params:" << m_doc.params().size();
    m_bg.loadFrom(path);
    m_canvas->setDocument(&m_doc);
    m_props->setDocument(&m_doc);
    m_parameters->setDocument(&m_doc);
    m_tp->setDocument(&m_doc);
    m_machine->setDocument(&m_doc);
    m_model->setDocument(&m_doc);
    m_dirty = false;
    m_recovery.discard();             // the design it replaces was saved or let go
    if (m_untitledPath != path)
        removeUntitledFile();         // the untitled design it replaces
    m_untitled = untitled;
    m_untitledFrom.clear();
    if (untitled)
        m_untitledPath = path;
    else
        rememberRecent(path);
    updateTitle();
    refreshInfo();
    if (m_previewAct->isChecked())
        refreshPreview();
    else
        markIsoStale();
}

// ---- untitled design and settings ----------------------------------------
//
// With no file on the command line the window starts on a blank design the
// size of the machine's bed, like Carbide Create's New. Document::save works
// by cloning the file it was loaded from, so the blank lives in a scratch
// file in the cache directory; Save goes to Save As until it has a real name.
void MainWindow::showStartupWindow()
{
    if (QSettings().value(QStringLiteral("ui/fullScreen"), false).toBool())
        m_fullScreenAct->setChecked(true);    // shows it full screen
    else
        show();
    // Only the real window listens: --shot and the tests build a MainWindow too.
    setReceiving(m_receiveAct->isChecked());
    QTimer::singleShot(3000, this, [this] { checkForUpdate(); });
    // ... and once an hour after that, so a long-running copy sees a new
    // version without being restarted first.
    auto *recheck = new QTimer(this);
    recheck->setInterval(60 * 60 * 1000);
    connect(recheck, &QTimer::timeout, this, &MainWindow::checkForUpdate);
    recheck->start();
    // Crash recovery, also only for the real window.
    auto *autosaveTimer = new QTimer(this);
    autosaveTimer->setInterval(2 * 60 * 1000);
    connect(autosaveTimer, &QTimer::timeout, this, &MainWindow::autosave);
    autosaveTimer->start();
    QTimer::singleShot(0, this, &MainWindow::offerRecovery);
}

void MainWindow::autosave()
{
    if (!m_dirty || m_doc.filePath().isEmpty())
        return;
    // Not while the machine works: the save runs on the thread that feeds
    // GRBL, and a big model starves its ~100 ms buffer. The next tick retries.
    if (m_machine && m_machine->isBusy())
        return;
    QString err;
    const QString copy = m_recovery.write(m_doc, m_untitled ? m_untitledPath : m_doc.filePath(),
                                          m_untitled, &err);
    if (copy.isEmpty()) {
        if (!err.isEmpty())
            statusBar()->showMessage(QStringLiteral("Could not write a recovery copy: %1").arg(err), 8000);
        return;
    }
    m_bg.saveTo(copy);
    m_model->saveTo(copy);
}

// A copy left by a session that did not close: open it as an untitled
// design (Save As offers the original name), never over the user's file.
void MainWindow::offerRecovery()
{
    for (const Recovery::Entry &e : m_recovery.pending()) {
        const QString what = e.untitled ? QStringLiteral("an untitled design")
                                        : QFileInfo(e.source).fileName();
        const auto r = QMessageBox::question(
            this, QStringLiteral("Recover unsaved work"),
            QStringLiteral("phobiCCCP did not close normally. A recovery copy of %1 from %2 "
                           "has changes that were never saved.\n\nOpen it? (No deletes it.)")
                .arg(what, QLocale().toString(e.saved, QLocale::ShortFormat)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (r != QMessageBox::Yes) {
            Recovery::remove(e.copy);
            continue;
        }
        if (!confirmDiscard(QStringLiteral("Save before opening the recovered design?")))
            return;
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QDir().mkpath(dir);
        const QString scratch = dir + QStringLiteral("/untitled-%1.c2d")
                                          .arg(QCoreApplication::applicationPid());
        QFile::remove(scratch);
        if (!QFile::copy(e.copy, scratch)) {
            QMessageBox::warning(this, QStringLiteral("Recover unsaved work"),
                                 QStringLiteral("Could not copy %1").arg(e.copy));
            return;
        }
        m_dirty = false;
        openPath(scratch, true);
        if (QFileInfo(m_doc.filePath()) != QFileInfo(scratch))
            return;
        m_recovery.adopt(e.copy);     // kept until the next copy or a save
        m_untitledFrom = e.untitled ? QString() : e.source;
        m_dirty = true;
        updateTitle();
        statusBar()->showMessage(QStringLiteral("Recovered %1; save it to keep it").arg(what), 10000);
        return;
    }
}

bool MainWindow::newUntitled(double width, double height, double thickness)
{
    QSettings st;
    const auto machine = appsettings::machine(st);
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/untitled-%1.c2d")
                                   .arg(QCoreApplication::applicationPid());
    QString err;
    if (!Document::createBlank(path, width > 0 ? width : machine.width, height > 0 ? height : machine.height,
                               thickness > 0 ? thickness : appsettings::kBlankThickness, &err)) {
        statusBar()->showMessage(QStringLiteral("Could not start a blank design: %1").arg(err));
        return false;
    }
    openPath(path, true);
    if (QFileInfo(m_doc.filePath()) != QFileInfo(path))
        return false;
    statusBar()->showMessage(QStringLiteral("New design — %1").arg(machine.label), 6000);
    return true;
}

// ---- projects from Syntexophobic -----------------------------------------
//
// Syntexophobic's File > Send to phobiCCCP… sends the part (STL), its XY
// sketches (DXF), the stock size and the project with its history (.cccp);
// see projectreceiver.h. Nothing is written or opened before the user says
// yes here, and the sender is told what happened.
void MainWindow::setReceiving(bool on)
{
    if (!on) {
        delete m_receiver;
        m_receiver = nullptr;
        return;
    }
    if (m_receiver)
        return;
    m_receiver = new ProjectReceiver(this);
    QString err;
    if (!m_receiver->start(ProjectReceiver::kPort, &err)) {
        statusBar()->showMessage(QStringLiteral("Cannot receive from Syntexophobic (port %1): %2")
                                     .arg(ProjectReceiver::kPort).arg(err), 10000);
        delete m_receiver;
        m_receiver = nullptr;
        return;
    }
    // Queued: the handlers ask questions (a nested event loop), and the socket
    // that emitted must not be deleted under its own readyRead meanwhile.
    connect(m_receiver, &ProjectReceiver::received, this,
            [this](int id, const ReceivedProject &p) {
                QTimer::singleShot(0, this, [this, id, p] { onProjectReceived(id, p); });
            });
    connect(m_receiver, &ProjectReceiver::request, this,
            [this](int id, const QJsonObject &req, const QString &peer) {
                QTimer::singleShot(0, this, [this, id, req, peer] { onPadRequest(id, req, peer); });
            });
    refreshDiscovery();
}

void MainWindow::onProjectReceived(int id, const ReceivedProject &p)
{
    const QString host = QSysInfo::machineHostName();
    const ReceivedFile *model = p.first(QStringLiteral("model"));
    const int sketches = p.count(QStringLiteral("vectors"));
    QStringList what;
    if (model)
        what << QStringLiteral("the part as a 3D model");
    if (sketches)
        what << (sketches == 1 ? QStringLiteral("1 sketch as vectors")
                               : QStringLiteral("%1 sketches as vectors").arg(sketches));
    if (p.count(QStringLiteral("project")))
        what << QStringLiteral("the project with its history");
    const QString sender = p.from.isEmpty() ? p.peer
                                            : QStringLiteral("%1 (%2)").arg(p.from, p.peer);
    // Never swap the document under a running job, nor in the middle of a
    // pad's send.
    if (m_linkBusy || m_machine->isBusy()) {
        if (m_receiver)
            m_receiver->answer(id, false, QStringLiteral("%1 is busy; try again when the machine is done").arg(host));
        return;
    }
    if (docHeld()) {
        if (m_receiver)
            m_receiver->answer(id, false, QStringLiteral("%1 is busy with the open design; try again shortly").arg(host));
        return;
    }
    m_linkBusy = true;
    const auto release = qScopeGuard([this] { m_linkBusy = false; });
    if (isMinimized())
        showNormal();
    raise();
    activateWindow();
    // The sender's own words are shown as they are, never as rich text.
    QMessageBox ask(QMessageBox::Question, QStringLiteral("Project from Syntexophobic"),
                    QStringLiteral("%1 on %2 sends \u201c%3\u201d: %4.\n\nOpen it as a new design?")
                        .arg(p.app.isEmpty() ? QStringLiteral("Syntexophobic") : p.app, sender, p.name,
                             what.join(QStringLiteral(", "))),
                    QMessageBox::Yes | QMessageBox::No, this);
    ask.setTextFormat(Qt::PlainText);
    ask.setDefaultButton(QMessageBox::Yes);
    const int r = ask.exec();
    if (r != QMessageBox::Yes
        || !confirmDiscard(QStringLiteral("Save before opening the received project?"))
        || m_machine->isBusy()) {
        if (m_receiver)
            m_receiver->answer(id, false, QStringLiteral("declined on %1").arg(host));
        return;
    }
    auto refuse = [&](const QString &why) {
        QMessageBox::warning(this, QStringLiteral("Project from Syntexophobic"), why);
        if (m_receiver)
            m_receiver->answer(id, false, QStringLiteral("could not open it on %1: %2").arg(host, why));
    };

    const QString base = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                         + QStringLiteral("/phobiCCCP/Received");
    QStringList paths;
    QString err;
    const QString dir = saveReceived(p, base, &paths, &err);
    if (dir.isEmpty())
        return refuse(err);
    QSettings st;
    const auto machine = appsettings::machine(st);
    const QString c2dPath = QDir(dir).filePath(safeFileName(p.name) + QStringLiteral(".c2d"));
    if (!Document::createBlank(c2dPath, p.stockWidth > 0 ? p.stockWidth : machine.width,
                               p.stockHeight > 0 ? p.stockHeight : machine.height,
                               p.stockThickness > 0 ? p.stockThickness : appsettings::kBlankThickness,
                               &err))
        return refuse(err);
    m_dirty = false;   // the question was already asked
    openPath(c2dPath, false);
    if (QFileInfo(m_doc.filePath()) != QFileInfo(c2dPath))
        return refuse(QStringLiteral("%1 did not open").arg(c2dPath));
    setMode(Mode::Design, false);
    QStringList notes;
    for (int i = 0; i < p.files.size(); ++i) {
        const ReceivedFile &f = p.files.at(i);
        if (f.role == QLatin1String("vectors"))   // in stock coordinates, like the model: not moved
            importVectorFile(this, m_canvas, &m_doc, paths.at(i), {}, false);
        else if (f.role == QLatin1String("model")
                 && !m_model->addStlMesh(f.data, f.name, f.x, f.y, &err))
            notes << QStringLiteral("the model could not be read (%1)").arg(err);
    }
    onSave();
    const QString done = QStringLiteral("opened %1 on %2").arg(QFileInfo(c2dPath).fileName(), host)
                         + (notes.isEmpty() ? QString() : QStringLiteral(", but ") + notes.join(QStringLiteral("; ")));
    statusBar()->showMessage(QStringLiteral("Received \u201c%1\u201d from %2 into %3")
                                 .arg(p.name, sender, dir), 10000);
    if (m_receiver)
        m_receiver->answer(id, true, done);
}

// ---- the PhobiCCCP pad app ------------------------------------------------
//
// Same port as Syntexophobic (projectreceiver.h), one JSON request per line.
// Designs a pad syncs are kept in Documents/phobiCCCP/Pad so every pad,
// and through the pads every other phobiCCCP, ends up with the same set.
namespace {

QString padDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
           + QStringLiteral("/phobiCCCP/Pad");
}

// The designs, with this computer's own id in their versions kept apart from
// them: Documents may be synced to, or copied onto, another computer
// (padlink::Store::replicaId).
padlink::Store padStore()
{
    return padlink::Store(padDir(), QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                                        + QStringLiteral("/Pad"));
}

// This install, so a pad tells two computers with the same name apart.
QString instanceId()
{
    QSettings st;
    QString id = st.value(QStringLiteral("pad/instanceId")).toString();
    if (id.isEmpty()) {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        st.setValue(QStringLiteral("pad/instanceId"), id);
    }
    return id;
}

bool padsMayStart()
{
    return QSettings().value(QStringLiteral("pad/allowStart"), false).toBool();
}

// A pad from before 1.3.1 (its puts and sends do not say `versions`) keeps
// one base for every computer: syncing with more than one, it can still
// write over an edit made elsewhere (Android/README.md, "Versions"). Said
// once a session for each pad, so it gets updated.
QString olderPadWarning(const QJsonObject &req, const QString &peer)
{
    static QSet<QString> told;
    if (req.contains(QStringLiteral("versions")) || told.contains(peer))
        return {};
    told.insert(peer);
    return QStringLiteral("The pad at %1 runs PhobiCCCP from before 1.3.1; update it. Until then, "
                          "if it syncs with more than one computer, an edit made on one can be lost")
        .arg(peer);
}

QJsonObject padError(const QString &why)
{
    return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), why}};
}

// Switching machines drops the connection and the homing here, so any pad on
// the network may only do it while nothing is connected, unless pads are
// trusted to start cutting anyway. Empty when the switch may go ahead.
QString padSwitchRefusal(const QString &machineId, const QJsonObject &status)
{
    QSettings st;
    if (machineId.isEmpty() || machineId == machineprofiles::activeId(st) || padsMayStart()
        || !status.value(QStringLiteral("connected")).toBool())
        return {};
    return QStringLiteral("the machine on %1 is connected; switching it from a pad is off there "
                          "(Machine > Pads may start cutting), or disconnect it first")
        .arg(QSysInfo::machineHostName());
}

// A new file beside `path` for a pad's version that must not go over it.
QString padCopyPath(const QString &path)
{
    const QFileInfo fi(path);
    const QDir dir = fi.absoluteDir();
    for (int n = 1; n < 100; ++n) {
        const QString name = n == 1 ? QStringLiteral("%1 (pad).c2d").arg(fi.completeBaseName())
                                    : QStringLiteral("%1 (pad %2).c2d").arg(fi.completeBaseName()).arg(n);
        if (!QFileInfo::exists(dir.filePath(name)))
            return dir.filePath(name);
    }
    return {};
}

// Write the pad design `d` to `path`, built beside it and swapped in, so a
// failure leaves what is there alone.
bool buildPadFile(const QJsonObject &d, const QString &path, QString *err)
{
    return padlink::rebuildDocument(d, path, err);
}

} // namespace

bool MainWindow::sameDesign(const QString &opened, QString *err) const
{
    // The same file by another path counts (a pad's edit reopens it by its
    // canonical path, a symlinked folder's target).
    if (m_doc.filePath() == opened
        || (!opened.isEmpty() && !m_doc.filePath().isEmpty() && QFileInfo(m_doc.filePath()) == QFileInfo(opened)))
        return true;
    if (err)
        *err = QStringLiteral("a different design was opened here; close this and send again");
    return false;
}

QString MainWindow::sendCopy(const QString &opened, QString *err)
{
    if (!sameDesign(opened, err))
        return {};
    // A copy as the design is now, saved or not; the user's file is untouched.
    const QString name = m_untitled || m_doc.filePath().isEmpty()
                             ? QStringLiteral("Untitled")
                             : QFileInfo(m_doc.filePath()).completeBaseName();
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                        + QStringLiteral("/send");
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(safeFileName(name) + QStringLiteral(".c2d"));
    QFile::remove(path);
    Document d = m_doc;
    if (!d.save(path, err))
        return {};
    m_bg.saveTo(path);
    m_model->saveTo(path);
    return path;
}

void MainWindow::onSendTo()
{
    commitEdits();
    const QString name = m_untitled || m_doc.filePath().isEmpty()
                             ? QStringLiteral("Untitled")
                             : QFileInfo(m_doc.filePath()).completeBaseName();
    // The dialog stays open while another computer or a pad may open a design
    // here; its name is fixed, so a different design is refused, not sent as this one.
    const QString opened = m_doc.filePath();
    SendToDialog dlg(name, instanceId(), [this, opened](QString *err) { return sendCopy(opened, err); }, this);
    dlg.exec();
}

void MainWindow::refreshDiscovery()
{
    if (!m_receiver)
        return;
    QSettings st;
    QJsonObject extra{
        {QStringLiteral("id"), instanceId()},
        {QStringLiteral("api"), 1},
        {QStringLiteral("machine"), machineprofiles::nameOf(st, machineprofiles::activeId(st))}};
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    // This program on a pad: File > Send to pad… on a computer lists it.
    extra.insert(QStringLiteral("pad"), true);
#endif
    m_receiver->setDiscoveryExtra(extra);
}

QJsonObject MainWindow::padInfo()
{
    QSettings st;
    const QString active = machineprofiles::activeId(st);
    QJsonArray machines;
    for (const machineprofiles::Profile &p : machineprofiles::list(st)) {
        QJsonObject m{{QStringLiteral("id"), p.id}, {QStringLiteral("name"), p.name},
                      {QStringLiteral("active"), p.id == active}};
        if (p.id == active) {
            const auto size = appsettings::machine(st);
            m.insert(QStringLiteral("size"), size.label);
            m.insert(QStringLiteral("width"), size.width);
            m.insert(QStringLiteral("height"), size.height);
        }
        machines.append(m);
    }
    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("app"), QStringLiteral("phobiCCCP")},
        {QStringLiteral("version"), QStringLiteral(PHOBICCCP_VERSION)},
        {QStringLiteral("host"), QSysInfo::machineHostName()},
        {QStringLiteral("id"), instanceId()},
        {QStringLiteral("machines"), machines},
        {QStringLiteral("activeMachine"), active},
        {QStringLiteral("status"), m_machine->statusJson()},
        {QStringLiteral("allowStart"), padsMayStart()},
        {QStringLiteral("document"), m_doc.filePath().isEmpty() || m_untitled
                                         ? QString() : QFileInfo(m_doc.filePath()).fileName()},
        {QStringLiteral("unsaved"), m_dirty}};
}

bool MainWindow::docHeld() const
{
    // Any modal step (a dialog, the export's progress) may be working on the
    // open design; so may an export not yet showing its dialog.
    // Send to pad / Send to phobiCCCP stay open after a send, waiting for the
    // design to come back; they read the open file only when Send is pressed.
    QWidget *modal = QApplication::activeModalWidget();
    if (qobject_cast<SendToPadDialog *>(modal) || qobject_cast<SendToDialog *>(modal))
        modal = nullptr;
    return m_holdDoc > 0 || modal != nullptr || exportRunning();
}

void MainWindow::onPadRequest(int id, const QJsonObject &req, const QString &peer)
{
    const QString op = req.value(QStringLiteral("op")).toString();
    QJsonObject reply;
    padlink::Store store = padStore();
    const bool changes = op == QLatin1String("put") || op == QLatin1String("machine")
                         || op == QLatin1String("send") || op == QLatin1String("open");
    if (changes && m_linkBusy) {
        // Another send (or a received project) is waiting on a question here.
        reply = padError(QStringLiteral("%1 is busy with another design; try again shortly")
                             .arg(QSysInfo::machineHostName()));
    } else if ((op == QLatin1String("send") || op == QLatin1String("open")) && docHeld()) {
        // Opening it would swap the design under a dialog or an export.
        reply = padError(QStringLiteral("%1 is busy with the open design; try again shortly")
                             .arg(QSysInfo::machineHostName()));
    } else if (op == QLatin1String("info")) {
        reply = padInfo();
        // A pad's sync round says who it is, so Send to pad… can list it,
        // and gets the design sent it, if one waits.
        const QJsonObject pad = req.value(QStringLiteral("pad")).toObject();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_pads.seen(pad, peer, now)) {
            const QJsonObject open = m_pads.pendingFor(pad.value(QStringLiteral("id")).toString(), now);
            if (!open.isEmpty())
                reply.insert(QStringLiteral("open"), open);
        }
    } else if (op == QLatin1String("opened")) {
        const QString padId = req.value(QStringLiteral("pad")).toString();
        QString name;
        if (m_pads.opened(padId, qint64(req.value(QStringLiteral("seq")).toDouble()), &name))
            statusBar()->showMessage(QStringLiteral("“%1” is open on %2").arg(name, m_pads.nameOf(padId)), 10000);
        reply = QJsonObject{{QStringLiteral("ok"), true}};
    } else if (op == QLatin1String("tools")) {
        reply = padlink::toolsJson();
        reply.insert(QStringLiteral("ok"), !reply.isEmpty());
    } else if (op == QLatin1String("designs")) {
        reply = QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("designs"), store.list()}};
    } else if (op == QLatin1String("get")) {
        const QString designId = req.value(QStringLiteral("id")).toString();
        QJsonObject d = store.get(designId);
        // A linked file a pad edit will not be rebuilt into (it holds what
        // the pad cannot, or was changed outside phobiCCCP): the pad says so
        // when the design is edited there, as the edit then lands in a copy.
        const padlink::Links links(padDir());
        for (const QString &path : links.pathsFor(designId))
            if (!d.isEmpty() && (!links.padOnly(path) || links.changedOutside(path)))
                d.insert(QStringLiteral("desktopOnly"), QSysInfo::machineHostName());
        reply = d.isEmpty() ? padError(QStringLiteral("no such design"))
                            : QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("design"), d}};
    } else if (op == QLatin1String("put")) {
        QString err;
        padlink::Store::PutOutcome out;
        const QJsonObject design = req.value(QStringLiteral("design")).toObject();
        const QString designId = design.value(QStringLiteral("id")).toString();
        const bool stored = store.put(design, &err, &out);
        reply = err.isEmpty() ? QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("stored"), stored}}
                              : padError(err);
        if (err.isEmpty()) {
            // The copy kept here now, so the pad knows whether it has it.
            reply.insert(QStringLiteral("modified"), store.get(designId).value(QStringLiteral("modified")));
        }
        const QString name = store.get(designId).value(QStringLiteral("name")).toString();
        if (out.moved) {
            // The pad's is the later edit: the copy kept here, and the files
            // that hold it, go to the fork; the rest follow the pad's.
            reply.insert(QStringLiteral("forked"), out.forked);
            reply.insert(QStringLiteral("moved"), true);
            padMoveLinks(designId, out.forked);
            padDesignChanged(designId);
            statusBar()->showMessage(QStringLiteral("“%1” changed both here and on a pad; the version "
                                                    "from here is kept as “%2”")
                                         .arg(name, store.get(out.forked).value(QStringLiteral("name")).toString()),
                                     10000);
        } else if (!out.forked.isEmpty()) {
            // Not rebuilt into the linked file: that holds the change made
            // here, which the pad's edit never saw.
            reply.insert(QStringLiteral("forked"), out.forked);
            statusBar()->showMessage(QStringLiteral("“%1” changed both here and on a pad; the pad's "
                                                    "version is kept as “%2”")
                                         .arg(name, store.get(out.forked).value(QStringLiteral("name")).toString()),
                                     10000);
        } else if (out.conflict) {
            reply.insert(QStringLiteral("conflict"), true);
            statusBar()->showMessage(QStringLiteral("A pad deleted “%1”, which has changed here since; "
                                                    "it is kept").arg(name), 10000);
        } else if (stored) {
            padDesignChanged(designId);
        }
        if (const QString older = olderPadWarning(req, peer); !older.isEmpty())
            statusBar()->showMessage(older, 15000);
    } else if (op == QLatin1String("machine")) {
        QString why;
        const QString machineId = req.value(QStringLiteral("id")).toString();
        why = padSwitchRefusal(machineId, m_machine->statusJson());
        reply = why.isEmpty() && switchMachine(machineId, &why) ? padInfo() : padError(why);
    } else if (op == QLatin1String("send") || op == QLatin1String("open")) {
        m_linkBusy = true;
        reply = padSend(id, req, peer);
        m_linkBusy = false;
    } else {
        reply = padError(QStringLiteral("unknown request %1").arg(op.left(20)));
    }
    if (m_receiver)    // the user may have turned receiving off meanwhile
        m_receiver->respond(id, reply);
}

QJsonObject MainWindow::padSend(int id, const QJsonObject &req, const QString &peer)
{
    const QString host = QSysInfo::machineHostName();
    // "send" carries a pad design (JSON), "open" a whole .c2d file, as
    // phobiCCCP itself sends it from File > Send to phobiCCCP.
    const bool whole = req.value(QStringLiteral("op")).toString() == QLatin1String("open");
    QJsonObject design = req.value(QStringLiteral("design")).toObject();
    QByteArray c2dBytes;
    if (whole) {
        const auto decoded = QByteArray::fromBase64Encoding(req.value(QStringLiteral("c2d")).toString().toLatin1(),
                                                            QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded)
            return padError(QStringLiteral("the design did not arrive whole"));
        c2dBytes = *decoded;
        if (!c2dBytes.startsWith(QByteArrayLiteral("SQLite format 3")))
            return padError(QStringLiteral("that is not a phobiCCCP design"));
    } else {
        if (const QString e = padlink::check(design); !e.isEmpty())
            return padError(e);
        if (design.value(QStringLiteral("deleted")).toBool())
            return padError(QStringLiteral("that design was deleted"));
    }
    // Never swap the document under a running job.
    if (m_machine->isBusy())
        return padError(QStringLiteral("%1 is busy with the machine; try again when it is done").arg(host));
    const QString machineId = req.value(QStringLiteral("machine")).toString();
    QString why;
    bool knownMachine = machineId.isEmpty();
    QSettings st;
    for (const auto &p : machineprofiles::list(st))
        knownMachine = knownMachine || p.id == machineId;
    if (!knownMachine)
        return padError(QStringLiteral("no such machine on %1").arg(host));
    if (const QString no = padSwitchRefusal(machineId, m_machine->statusJson()); !no.isEmpty())
        return padError(no);
    // Sent is also synced. Changed here since the pad's copy, it is kept as a
    // new design: that is what opens (and is linked), and the pad is told so it
    // takes the copy here in place of its own. A whole file is not synced.
    padlink::Store store = padStore();
    QString putError;
    padlink::Store::PutOutcome put;
    bool stored = false;
    // This very copy was kept as a design of its own before, and that was
    // deleted since: what the pad sent opens on its own, not brought back
    // for every pad.
    bool forkDeleted = false;
    if (!whole) {
        stored = store.put(design, &putError, &put);
        if (!put.forked.isEmpty() && !put.moved) {
            const QJsonObject fork = store.get(put.forked);
            if (fork.value(QStringLiteral("deleted")).toBool())
                forkDeleted = true;
            else if (!fork.isEmpty())
                design = fork;
        } else if (stored) {
            // The pad's is the later edit: the copy here went to the fork.
            if (put.moved)
                padMoveLinks(design.value(QStringLiteral("id")).toString(), put.forked);
            padDesignChanged(design.value(QStringLiteral("id")).toString());
        }
    }
    // Kept here already as a later copy (made from the pad's, or a deletion):
    // that is what every pad gets, so that is what opens, and a deletion
    // leaves what opens unlinked. Linked as the pad's older copy, the file
    // would neither follow the design nor be shared without forking it.
    bool deletedHere = false;
    if (!whole && !stored && putError.isEmpty() && put.forked.isEmpty()) {
        const QJsonObject kept = store.get(design.value(QStringLiteral("id")).toString());
        if (kept.value(QStringLiteral("deleted")).toBool())
            deletedHere = true;
        else if (!kept.isEmpty())
            design = kept;
    }

    const bool start = req.value(QStringLiteral("start")).toBool();
    const QString from = req.value(QStringLiteral("from")).toString().left(60);
    QString name = (whole ? req : design).value(QStringLiteral("name")).toString().left(120);
    if (name.trimmed().isEmpty())
        name = QStringLiteral("Design");
    const QString sender = from.isEmpty() ? peer : QStringLiteral("%1 (%2)").arg(from, peer);
    if (isMinimized())
        showNormal();
    raise();
    activateWindow();
    if (!confirmDiscard(QStringLiteral("%1 sends “%2”. Save before opening it?")
                            .arg(sender, name)))
        return padError(QStringLiteral("declined on %1").arg(host));
    // The question may have taken a while: the pad may have given up, and
    // then nothing is opened or started for it.
    if (m_receiver && !m_receiver->isWaiting(id))
        return padError(QStringLiteral("the pad stopped waiting"));
    if (m_machine->isBusy())
        return padError(QStringLiteral("%1 is busy with the machine; try again when it is done").arg(host));
    // Switched only now it is certain to be opened (switching resets the
    // machine's connection and homing).
    why = padSwitchRefusal(machineId, m_machine->statusJson());
    if (!why.isEmpty() || (!machineId.isEmpty() && !switchMachine(machineId, &why)))
        return padError(why);

    ReceivedProject folder;   // no files: only the folder, named like a received project
    folder.name = name;
    const QString base = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                         + QStringLiteral("/phobiCCCP/Received");
    QString err;
    const QString dir = saveReceived(folder, base, nullptr, &err);
    if (dir.isEmpty())
        return padError(err);
    const QString c2dPath = QDir(dir).filePath(safeFileName(name) + QStringLiteral(".c2d"));
    QStringList notes;
    if (whole) {
        QSaveFile f(c2dPath);
        if (!f.open(QIODevice::WriteOnly) || f.write(c2dBytes) != c2dBytes.size() || !f.commit())
            return padError(QStringLiteral("could not save the design on %1: %2").arg(host, f.errorString()));
    } else if (!padlink::buildDocument(design, c2dPath, &notes, &err)) {
        return padError(QStringLiteral("could not make the design on %1: %2").arg(host, err));
    }
    m_dirty = false;   // the question was already asked
    openPath(c2dPath, false);
    if (QFileInfo(m_doc.filePath()) != QFileInfo(c2dPath))
        return padError(QStringLiteral("%1 did not open on %2").arg(QFileInfo(c2dPath).fileName(), host));
    // A whole file sent by a computer carries no versions to warn about.
    if (const QString older = whole ? QString() : olderPadWarning(req, peer); !older.isEmpty())
        statusBar()->showMessage(QStringLiteral("“%1” from %2. %3").arg(name, sender, older), 15000);
    else
        statusBar()->showMessage(QStringLiteral("“%1” from %2").arg(name, sender), 10000);
    // A shape that could not be made here would be gone after a save here,
    // so then a pad edit must not replace the file either.
    if (!whole) {
        bool padOnly = true;
        for (const QString &n : notes)
            padOnly = padOnly && !n.startsWith(QLatin1String("shape "));
        padlink::Links links(padDir());
        if (!putError.isEmpty())
            notes << QStringLiteral("not kept for the pads, so edits do not sync: %1").arg(putError);
        else if (deletedHere)
            notes << QStringLiteral("deleted for the pads meanwhile, so edits here do not sync");
        else if (forkDeleted)
            notes << QStringLiteral("“%1” had changed on %2 since, and this version, kept beside it, was "
                                    "deleted for the pads meanwhile; it opened on its own, so edits here do not sync")
                         .arg(store.get(design.value(QStringLiteral("id")).toString()).value(QStringLiteral("name")).toString(),
                              host);
        else if (!links.set(c2dPath, design.value(QStringLiteral("id")).toString(), padOnly,
                            design.value(QStringLiteral("modified")).toDouble()))
            notes << QStringLiteral("not linked to the pad design, so edits do not sync: %1").arg(links.error());
    }

    QJsonObject reply{{QStringLiteral("ok"), true}, {QStringLiteral("started"), false},
                      {QStringLiteral("notes"), QJsonArray::fromStringList(notes)}};
    // As a put answers: the copy kept here now, and the fork the pad's
    // version became, so the pad moves its base or takes the copy here.
    const QString sentId = req.value(QStringLiteral("design")).toObject().value(QStringLiteral("id")).toString();
    if (!whole && putError.isEmpty())
        reply.insert(QStringLiteral("modified"), store.get(sentId).value(QStringLiteral("modified")));
    if (put.moved) {
        reply.insert(QStringLiteral("forked"), put.forked);
        reply.insert(QStringLiteral("moved"), true);
        notes << QStringLiteral("“%1” had changed on %2 too; that version is kept there as “%3”")
                     .arg(name, host, store.get(put.forked).value(QStringLiteral("name")).toString());
        reply.insert(QStringLiteral("notes"), QJsonArray::fromStringList(notes));
    } else if (!put.forked.isEmpty()) {
        reply.insert(QStringLiteral("forked"), put.forked);
        if (!forkDeleted)
            notes << QStringLiteral("“%1” had changed on %2 since, so yours opened as “%3”")
                     .arg(store.get(sentId).value(QStringLiteral("name")).toString(), host, name);
        reply.insert(QStringLiteral("notes"), QJsonArray::fromStringList(notes));
    }
    QString message = QStringLiteral("opened %1 on %2").arg(QFileInfo(c2dPath).fileName(), host);
    if (start) {
        setMode(Mode::Machine, false);
        QString notStarted;
        if (!padsMayStart())
            notStarted = QStringLiteral("starting from a pad is off on %1 (Machine > Pads may start cutting)")
                             .arg(host);
        else if (m_receiver && !m_receiver->isWaiting(id))
            notStarted = QStringLiteral("the pad stopped waiting");
        else if (!m_machine->startFromRemote(&notStarted) && notStarted.isEmpty())
            notStarted = QStringLiteral("the machine did not start");
        if (notStarted.isEmpty()) {
            reply.insert(QStringLiteral("started"), true);
            message += QStringLiteral(", cutting started");
        } else {
            message += QStringLiteral(", not started: ") + notStarted;
        }
    } else {
        setMode(Mode::Design, false);
    }
    reply.insert(QStringLiteral("message"), message);
    return reply;
}

QString MainWindow::padShareSaved(const QString &c2dPath, bool startLink, QStringList *lostOut,
                                  QString *error)
{
    padlink::Links links(padDir());
    if (!links.ok()) {
        statusBar()->showMessage(QStringLiteral("Saved %1, but not to the pads: %2").arg(c2dPath, links.error()),
                                 10000);
        if (error)
            *error = links.error();
        return {};
    }
    QString id = links.idFor(c2dPath);
    padlink::Store store = padStore();
    QJsonObject base = id.isEmpty() ? QJsonObject() : store.get(id);
    if (startLink && (base.isEmpty() || base.value(QStringLiteral("deleted")).toBool())) {
        // Sent to a pad for the first time (File > Send to pad…), or its pad
        // design was deleted since: it becomes a pad design of its own,
        // linked like one a pad sent here.
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        base = QJsonObject{{QStringLiteral("id"), id},
                           {QStringLiteral("name"), QFileInfo(c2dPath).completeBaseName().left(60)},
                           {QStringLiteral("modified"), 0}};
    }
    if (id.isEmpty() || base.isEmpty() || base.value(QStringLiteral("deleted")).toBool()) {
        if (error)
            *error = QStringLiteral("it is not a pad design");
        return {};
    }
    // Saved here but not shared: the file now holds what the pads do not, so
    // a pad edit must not be rebuilt over it.
    auto notShared = [&](const QString &why) {
        if (error)
            *error = why;
        if (!links.idFor(c2dPath).isEmpty())
            links.set(c2dPath, id, false, links.modified(c2dPath));
        statusBar()->showMessage(QStringLiteral("Not sent to the pads: %1").arg(why), 10000);
        return QString();
    };
    // A pad changed the design after this file was made from it (and it was
    // not rebuilt, as it was open with changes or holds what a pad cannot).
    // Sharing this file over it would silently undo that edit everywhere, so
    // it goes to the pads as a design of its own.
    const double fileFrom = links.modified(c2dPath);
    QString forkedFrom;
    if (fileFrom > 0 && base.value(QStringLiteral("modified")).toDouble() > fileFrom) {
        forkedFrom = base.value(QStringLiteral("name")).toString();
        base.insert(QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
        base.insert(QStringLiteral("name"), QStringLiteral("%1 (%2)").arg(forkedFrom.left(40),
                                                                         QSysInfo::machineHostName().left(19)));
        base.insert(QStringLiteral("modified"), 0);
        base.remove(QStringLiteral("vv"));   // a design of its own: its versions start here
    }
    QStringList lost;
    QString err;
    QJsonObject d = padlink::designFromDocument(c2dPath, base, &lost, &err);
    if (d.isEmpty())
        return notShared(err);
    if (!m_model->model().components.isEmpty())
        lost << QStringLiteral("the 3D model");
    if (!m_bg.isNull())
        lost << QStringLiteral("the background picture");
    // A pad edit rebuilds the file from the pad's shapes, which would drop
    // the constraints, dimensions and parameters (and the toolpath fields
    // bound to them), so such a design is not rebuilt from a pad.
    if (!m_doc.parametric().isEmpty())
        lost << QStringLiteral("the constraints and parameters");
    if (lostOut)
        *lostOut = lost;
    // Newer than what the pads have, even with this computer's clock behind.
    const double modified = padlink::nextStamp(base.value(QStringLiteral("modified")).toDouble(),
                                               double(QDateTime::currentMSecsSinceEpoch()));
    // With this computer's edit in its versions, so it replaces the copy it
    // was made from everywhere but is kept beside one edited meanwhile.
    d = store.edited(d, modified);
    if (!store.put(d, &err))
        return notShared(err.isEmpty() ? QStringLiteral("a newer copy is kept here") : err);
    if (!links.set(c2dPath, d.value(QStringLiteral("id")).toString(), lost.isEmpty(), modified)) {
        statusBar()->showMessage(QStringLiteral("Saved %1 and sent to the pads, but its link could not be "
                                                "kept: %2").arg(c2dPath, links.error()), 10000);
        return d.value(QStringLiteral("id")).toString();
    }
    if (forkedFrom.isEmpty()) {
        // The design's other linked files follow it, as after a pad edit
        // (padDesignChanged): left as they were, they would hold the old
        // design and fork it when saved. One open with changes is left alone.
        auto isOpen = [&](const QString &path) {
            return !m_untitled && QFileInfo(m_doc.filePath()) == QFileInfo(path);
        };
        for (const QString &path : padlink::followDesign(links, d, c2dPath, [&](const QString &p) {
                 return isOpen(p) && (m_dirty || m_machine->isBusy());
             })) {
            if (isOpen(path))
                openPath(path, false);
        }
    }
    if (!forkedFrom.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Saved %1. “%2” had changed on a pad since, so the "
                                                "pads get this as “%3” and keep their own")
                                     .arg(c2dPath, forkedFrom, d.value(QStringLiteral("name")).toString()),
                                 10000);
        return d.value(QStringLiteral("id")).toString();
    }
    statusBar()->showMessage(
        lost.isEmpty() ? QStringLiteral("Saved %1, and the pads get the change").arg(c2dPath)
                       : QStringLiteral("Saved %1. The pads get the change without %2, so a pad "
                                        "edit no longer replaces this file")
                             .arg(c2dPath, lost.join(QStringLiteral(", "))),
        10000);
    return d.value(QStringLiteral("id")).toString();
}

void MainWindow::onSendToPad()
{
    commitEdits();
    if (m_doc.filePath().isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Nothing to send"),
                                 QStringLiteral("Open or make a design first."));
        return;
    }
    // The pad gets the file as saved, and its edits come back into it.
    if (m_untitled || m_dirty) {
        const auto b = QMessageBox::question(
            this, QStringLiteral("Send to pad"),
            m_untitled ? QStringLiteral("Save this design first? Edits made on the pad come back to its file.")
                       : QStringLiteral("Save the changes first? The pad gets the design as saved."),
            QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Save);
        if (b != QMessageBox::Save)
            return;
        onSave();
        if (m_untitled || m_dirty)
            return;
    }
    SendToPadDialog::Hooks hooks;
    hooks.pads = [this] { return m_pads.recent(QDateTime::currentMSecsSinceEpoch()); };
    const QString opened = m_doc.filePath();
    hooks.send = [this, opened](const QString &padId, QString *note, QString *error) {
        if (!sameDesign(opened, error))
            return qint64(0);
        return sendToPad(padId, note, error);
    };
    hooks.waiting = [this](const QString &padId, qint64 seq) { return m_pads.waiting(padId, seq); };
    hooks.makeCopy = [this, opened](QString *err) { return sendCopy(opened, err); };
    hooks.ownId = instanceId();
    SendToPadDialog dlg(QFileInfo(m_doc.filePath()).completeBaseName(), m_receiver != nullptr, hooks, this);
    dlg.exec();
}

qint64 MainWindow::sendToPad(const QString &padId, QString *note, QString *error)
{
    const QString path = m_doc.filePath();
    if (m_untitled || m_dirty || path.isEmpty()) {
        *error = QStringLiteral("save the design first");
        return 0;
    }
    if (!QFileInfo::exists(path)) {
        *error = QStringLiteral("%1 is not there any more").arg(path);
        return 0;
    }
    // Already the pads' copy as it is in the file: sent as it is. Otherwise
    // (not a pad design yet, or the file is newer) it is shared first, as a
    // save shares it.
    const padlink::Links links(padDir());
    QString id = links.idFor(path);
    const QJsonObject kept = id.isEmpty() ? QJsonObject() : padStore().get(id);
    const bool current = !kept.isEmpty() && !kept.value(QStringLiteral("deleted")).toBool()
                         && links.modified(path) > 0
                         && kept.value(QStringLiteral("modified")).toDouble() == links.modified(path)
                         && !links.changedOutside(path);
    bool whole = current && links.padOnly(path);
    if (!current) {
        QStringList lost;
        id = padShareSaved(path, true, &lost, error);
        if (id.isEmpty())
            return 0;
        whole = lost.isEmpty();
    }
    if (!whole)
        *note = QStringLiteral("The pad gets it without what only this program holds (curves, tabs, "
                               "parameters…), so edits there come back as a copy beside this file.");
    const QJsonObject d = padStore().get(id);
    return m_pads.sendOpen(padId, id, d.value(QStringLiteral("name")).toString(), QSysInfo::machineHostName(),
                           QDateTime::currentMSecsSinceEpoch());
}

void MainWindow::padMoveLinks(const QString &id, const QString &forkId)
{
    // The copy kept here went to the fork (Store::put, `moved`): so do the
    // files that hold it, so later saves of them share to the fork and pad
    // edits of the design no longer rebuild them.
    const QJsonObject fork = padStore().get(forkId);
    padlink::Links links(padDir());
    if (!fork.isEmpty() && links.moveTo(id, fork.value(QStringLiteral("modified")).toDouble(), forkId) < 0)
        statusBar()->showMessage(QStringLiteral("Which files hold “%1” could not be noted: %2")
                                     .arg(fork.value(QStringLiteral("name")).toString(), links.error()),
                                 10000);
}

void MainWindow::padDesignChanged(const QString &id)
{
    padlink::Links links(padDir());
    const QJsonObject d = padStore().get(id);
    const QString name = d.value(QStringLiteral("name")).toString();
    if (!links.ok()) {
        statusBar()->showMessage(QStringLiteral("“%1” changed on a pad, but which files it is linked to "
                                                "is not known: %2").arg(name, links.error()), 10000);
        return;
    }
    const QStringList paths = links.pathsFor(id);
    const double modified = d.value(QStringLiteral("modified")).toDouble();
    // A linked file the pad's version may replace: it holds only what the
    // pad can, and nothing (Carbide Create, say) changed it since.
    auto rebuildable = [&](const QString &path) {
        return links.padOnly(path) && !links.changedOutside(path);
    };
    if (d.isEmpty())
        return;   // not readable here: nothing to rebuild from, and its links stay
    QStringList missing;
    for (const QString &path : paths) {
        if (d.value(QStringLiteral("deleted")).toBool()) {
            links.remove(path);   // the file stays; it is only no longer linked
            continue;
        }
        // Gone for now (on a drive not plugged in, say): still linked, so it is
        // rebuilt or shared again once it is back.
        if (!QFileInfo::exists(path)) {
            missing << path;
            continue;
        }
        const QString file = QFileInfo(path).fileName();
        QString err;
        if (!rebuildable(path)) {
            const bool outside = links.changedOutside(path);
            const QString why = outside ? QStringLiteral("was changed outside phobiCCCP")
                                        : QStringLiteral("has things the pad cannot hold");
            // Its own edits stay; it no longer counts as the pad's to replace.
            if (outside)
                links.set(path, id, false, links.modified(path));
            // The pad's version goes into a file of its own beside it, linked
            // too, so later pad edits rebuild that one instead of piling up copies.
            bool hasCopy = false;
            for (const QString &other : paths)
                hasCopy = hasCopy || (other != path && QFileInfo::exists(other) && rebuildable(other));
            if (hasCopy) {
                statusBar()->showMessage(QStringLiteral("“%1” changed on a pad; %2 %3, so it was left as it is")
                                             .arg(name, file, why), 10000);
                continue;
            }
            const QString copy = padCopyPath(path);
            if (copy.isEmpty() || !buildPadFile(d, copy, &err)) {
                statusBar()->showMessage(QStringLiteral("“%1” changed on a pad; %2 %3, and the pad's version "
                                                        "could not be made beside it: %4")
                                             .arg(name, file, why, err), 10000);
                continue;
            }
            links.set(copy, id, true, modified);
            statusBar()->showMessage(QStringLiteral("“%1” changed on a pad; %2 %3, so the pad's version is in %4")
                                         .arg(name, file, why, QFileInfo(copy).fileName()), 10000);
            continue;
        }
        const bool open = !m_untitled && QFileInfo(m_doc.filePath()) == QFileInfo(path);
        // A number being typed (not committed yet) is an unsaved change too.
        // Not committed here: taking the focus away mid-number would keep
        // half of it ("2" of "2.5").
        if (open && (m_dirty || typingPending() || m_machine->isBusy())) {
            statusBar()->showMessage(QStringLiteral("“%1” changed on a pad; save or finish %2 first, "
                                                    "then send it again from the pad")
                                         .arg(name, file), 10000);
            continue;
        }
        if (!buildPadFile(d, path, &err)) {
            statusBar()->showMessage(QStringLiteral("“%1” changed on a pad but could not be made: %2")
                                         .arg(name, err), 10000);
            continue;
        }
        links.set(path, id, true, modified);
        if (open)
            openPath(path, false);
        statusBar()->showMessage(QStringLiteral("“%1” updated from a pad").arg(name), 10000);
    }
    if (!missing.isEmpty())
        statusBar()->showMessage(QStringLiteral("“%1” changed on a pad, but %2 cannot be found (a drive not "
                                                "plugged in?); it stays linked, so the next change reaches it once it is back")
                                     .arg(name, missing.join(QStringLiteral(", "))),
                                 10000);
}

bool MainWindow::switchMachine(const QString &id, QString *why)
{
    QSettings st;
    if (id == machineprofiles::activeId(st))
        return true;
    bool known = false;
    for (const auto &p : machineprofiles::list(st))
        known = known || p.id == id;
    if (!known) {
        if (why) *why = QStringLiteral("no such machine on %1").arg(QSysInfo::machineHostName());
        return false;
    }
    if (m_machine->isBusy()) {
        if (why) *why = QStringLiteral("the machine on %1 is busy").arg(QSysInfo::machineHostName());
        return false;
    }
    machineprofiles::activate(st, id);
    m_machine->reloadMachine(why);
    rebuildMachineMenu();
    refreshDiscovery();
    statusBar()->showMessage(QStringLiteral("Machine: %1").arg(machineprofiles::nameOf(st, id)), 6000);
    return true;
}

void MainWindow::buildMachineMenu()
{
    m_machineMenu = m_menuBar->addMenu(QStringLiteral("Mac&hine"));
    m_machineMenu->setToolTipsVisible(true);
    rebuildMachineMenu();
}

void MainWindow::rebuildMachineMenu()
{
    if (!m_machineMenu)
        return;
    m_machineMenu->clear();
    QSettings st;
    const QString active = machineprofiles::activeId(st);
    auto *group = new QActionGroup(m_machineMenu);
    for (const machineprofiles::Profile &p : machineprofiles::list(st)) {
        QAction *a = m_machineMenu->addAction(p.name);
        a->setCheckable(true);
        a->setChecked(p.id == active);
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, id = p.id] {
            QString why;
            if (!switchMachine(id, &why))
                QMessageBox::information(this, QStringLiteral("Machine"), why);
            rebuildMachineMenu();
        });
    }
    m_machineMenu->addSeparator();
    m_machineMenu->addAction(QStringLiteral("&Add machine…"), this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Add machine"),
            QStringLiteral("Name of the machine (its size, port and BitSetter positions are "
                           "kept separately for each machine):"), QLineEdit::Normal, QString(), &ok);
        if (!ok)
            return;
        QSettings s;
        const QString id = machineprofiles::add(s, name);
        QString why;
        if (!switchMachine(id, &why))
            QMessageBox::information(this, QStringLiteral("Machine"), why);
        rebuildMachineMenu();
    });
    m_machineMenu->addAction(QStringLiteral("&Rename machine…"), this, [this] {
        QSettings s;
        const QString id = machineprofiles::activeId(s);
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Rename machine"),
            QStringLiteral("Name:"), QLineEdit::Normal, machineprofiles::nameOf(s, id), &ok);
        if (ok && machineprofiles::rename(s, id, name)) {
            rebuildMachineMenu();
            refreshDiscovery();
        }
    });
    QAction *rm = m_machineMenu->addAction(QStringLiteral("Re&move other machine…"), this, [this] {
        QSettings s;
        const QString active = machineprofiles::activeId(s);
        QStringList names;
        QStringList ids;
        for (const auto &p : machineprofiles::list(s))
            if (p.id != active) { names << p.name; ids << p.id; }
        bool ok = false;
        const QString pick = QInputDialog::getItem(this, QStringLiteral("Remove machine"),
            QStringLiteral("Forget this machine and its settings:"), names, 0, false, &ok);
        if (ok && names.contains(pick)) {
            machineprofiles::remove(s, ids.at(names.indexOf(pick)));
            rebuildMachineMenu();
        }
    });
    rm->setEnabled(machineprofiles::list(st).size() > 1);
    m_machineMenu->addSeparator();
    QAction *allow = m_machineMenu->addAction(QStringLiteral("&Pads may start cutting"));
    allow->setCheckable(true);
    allow->setChecked(padsMayStart());
    allow->setToolTip(QStringLiteral("Let the PhobiCCCP pad app send a design and start it "
                                     "right away. The machine still has to be connected, homed "
                                     "and idle, and the pad asks before it sends."));
    connect(allow, &QAction::toggled, this, [](bool on) {
        QSettings().setValue(QStringLiteral("pad/allowStart"), on);
    });
}

void MainWindow::removeUntitledFile()
{
    if (m_untitledPath.isEmpty())
        return;
    QFile::remove(m_untitledPath);
    m_untitledPath.clear();
}

void MainWindow::onSettings()
{
    SettingsDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const bool scaleChanged = dlg.save();
    m_tp->applyTextSize();
    if (!scaleChanged)
        return;
    // QT_SCALE_FACTOR is read once, when QApplication starts.
    const auto r = QMessageBox::question(
        this, QStringLiteral("GUI scale"),
        QStringLiteral("The new GUI scale takes effect after a restart. Restart phobiCCCP now?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (r != QMessageBox::Yes)
        return;
    const QString reopen = m_untitled ? QString() : m_doc.filePath();
    if (!close())
        return;                       // cancelled at the unsaved-changes prompt
    QStringList args;
    if (!reopen.isEmpty())
        args << reopen;
    QProcess::startDetached(QCoreApplication::applicationFilePath(), args);
}

// ---- recent files ---------------------------------------------------------

static const int kMaxRecent = 5;

void MainWindow::rememberRecent(const QString &path)
{
    const QString abs = QFileInfo(path).absoluteFilePath();
    QSettings st;
    QStringList recent = st.value(QStringLiteral("recentFiles")).toStringList();
    recent.removeAll(abs);
    recent.prepend(abs);
    while (recent.size() > kMaxRecent)
        recent.removeLast();
    st.setValue(QStringLiteral("recentFiles"), recent);
    rebuildRecentMenu();
}

void MainWindow::rebuildRecentMenu()
{
    if (!m_recentMenu)
        return;
    m_recentMenu->clear();

    QSettings st;
    QStringList recent = st.value(QStringLiteral("recentFiles")).toStringList();
    // Files that have been moved or deleted are dropped rather than offered.
    const qsizetype had = recent.size();
    recent.erase(std::remove_if(recent.begin(), recent.end(),
                                [](const QString &p) { return !QFile::exists(p); }),
                 recent.end());
    if (recent.size() != had)
        st.setValue(QStringLiteral("recentFiles"), recent);

    if (recent.isEmpty()) {
        QAction *none = m_recentMenu->addAction(QStringLiteral("(nothing yet)"));
        none->setEnabled(false);
        return;
    }
    int n = 0;
    for (const QString &p : std::as_const(recent)) {
        // &1..&5 so the list is reachable from the keyboard; the full path is
        // the tooltip, since several projects often share a file name.
        QAction *a = m_recentMenu->addAction(
            QStringLiteral("&%1  %2").arg(++n).arg(QFileInfo(p).fileName()));
        a->setToolTip(p);
        a->setStatusTip(p);
        connect(a, &QAction::triggered, this, [this, p] { openFile(p); });
    }
    m_recentMenu->addSeparator();
    m_recentMenu->addAction(QStringLiteral("&Clear list"), this, [this] {
        QSettings().remove(QStringLiteral("recentFiles"));
        rebuildRecentMenu();
    });
}

void MainWindow::refreshInfo()
{
    QStringList lines;
    lines << QStringLiteral("== params ==");
    const auto p = m_doc.params();
    QStringList keys = p.keys();
    keys.sort();
    for (const QString &k : keys)
        lines << QStringLiteral("%1 = %2").arg(k, p.value(k));

    // element type histogram
    QHash<QString, int> elemHist;
    for (const Element &e : m_doc.elements())
        elemHist[e.geometryType]++;
    lines << QString() << QStringLiteral("== elements (%1) ==").arg(m_doc.elements().size());
    for (auto it = elemHist.constBegin(); it != elemHist.constEnd(); ++it)
        lines << QStringLiteral("%1: %2").arg(it.key()).arg(it.value());

    // toolpath list
    lines << QString() << QStringLiteral("== toolpaths (%1) ==").arg(m_doc.toolpaths().size());
    for (const Toolpath &t : m_doc.toolpaths())
        lines << QStringLiteral("%1  [%2]").arg(t.json.value("name").toString(), t.type);

    m_info->setPlainText(lines.join(QChar('\n')));
}

} // namespace c2d
