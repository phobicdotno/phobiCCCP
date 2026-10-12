// The tool-change rehearsal program (GrblStreamer::toolChangeRehearsalLines).
//
// It is run on a real machine to check the tool-change flow before a job
// relies on it, so what matters is what it must never contain: anything that
// starts the spindle or moves the tool other than straight up. The flow itself
// (park, measure, G43.1, continue) is driven against the simulator by
// tools/flowtest.sh through `phobicccp --grbl-rehearse`.

#include "grblstreamer.h"

#include <QRegularExpression>

#include <cstdio>
#include <cstdlib>

static int g_checks = 0;

static void check(bool cond, const char *what)
{
    ++g_checks;
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

int main()
{
    c2d::BitSetterConfig cfg;
    cfg.enabled = true;
    cfg.x = -20;
    cfg.y = -30;
    cfg.safeZ = -7.5;
    const QStringList lines = c2d::GrblStreamer::toolChangeRehearsalLines(cfg, 3);

    check(lines.filter(QRegularExpression(QStringLiteral("^M0\\s*;\\s*T3$"))).size() == 1,
          "exactly one tool-change marker, for the tool asked for");
    check(lines.first() == QLatin1String("M5"), "the spindle is switched off first");

    const QRegularExpression spindle(QStringLiteral("\\bM0?[34]\\b|\\bS\\d"));
    const QRegularExpression cut(QStringLiteral("\\bG0?[123]\\b|\\bG38"));
    const QRegularExpression word(QStringLiteral("([A-Z])(-?[\\d.]+)"));
    const int marker = lines.indexOf(QRegularExpression(QStringLiteral("M0\\s*;.*")));
    check(marker > 0, "the marker is not the first line");
    bool raisedBeforeMarker = false;
    for (int i = 0; i < lines.size(); ++i) {
        const QString &l = lines.at(i);
        if (i == marker)
            continue;
        check(!spindle.match(l).hasMatch(), "nothing starts the spindle");
        check(!cut.match(l).hasMatch(), "nothing feeds, arcs or probes");
        if (!l.startsWith(QLatin1String("G0")) && !l.contains(QLatin1String(" G0 ")))
            continue;
        // The only motion is a machine-coordinate rapid to safe Z.
        check(l.startsWith(QLatin1String("G53 G0 ")), "rapids are in machine coordinates");
        auto it = word.globalMatch(l.mid(7));
        int axes = 0;
        while (it.hasNext()) {
            const auto m = it.next();
            ++axes;
            check(m.captured(1) == QLatin1String("Z"), "rapids move Z only");
            check(qAbs(m.captured(2).toDouble() - cfg.safeZ) < 1e-9, "to the safe Z");
        }
        check(axes == 1, "one axis word per rapid");
        if (i < marker)
            raisedBeforeMarker = true;
    }
    check(raisedBeforeMarker, "the tool goes up to safe Z before the tool change");
    check(lines.last() == QLatin1String("M5"), "and the program ends with the spindle off");
    check(!lines.contains(QStringLiteral("M2")) && !lines.contains(QStringLiteral("M02")),
          "no program end, so the measured offset is left in place");

    std::printf("test_rehearsal: %d checks OK\n", g_checks);
    return 0;
}
