// Dust collector / coolant with the job (post_grbl.cpp coolantTransform):
// M8 (or M7) right after each spindle start, M9 right before each spindle
// stop, so before a tool change's park too; a program that switches it
// itself is left as it is. And the gamepad can toggle it.
//
// Plain asserts, no framework; exits 0 on success.

#include "../src/gamepad.h"
#include "../src/post_grbl.h"

#include <cstdio>
#include <cstdlib>

using namespace c2d;

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
    // Two tools: spindle start, cut, tool change, start again, cut, end.
    QVector<Op> ops;
    ops << Op::tool(1) << Op::spindle(12000) << Op::rapid(0, 0, 5)
        << Op::feedTo(10, 0, -1, 800) << Op::rapid(10, 0, 5)
        << Op::tool(2) << Op::spindle(16000) << Op::feedTo(20, 0, -1, 800)
        << Op::rapid(20, 0, 5);
    const QStringList prog = GrblPost(true).generate(ops).split(QChar('\n'), Qt::SkipEmptyParts);
    const QStringList out = coolantTransform(prog);

    auto idx = [&](const QStringList &l, auto pred, int from = 0) {
        for (int i = from; i < l.size(); ++i)
            if (pred(l.at(i).trimmed()))
                return i;
        return -1;
    };
    auto starts = [](const QString &p) { return [p](const QString &l) { return l.startsWith(p); }; };
    auto is = [](const QString &p) { return [p](const QString &l) { return l == p; }; };

    const int m3 = idx(out, starts(QStringLiteral("M03")));
    check(m3 >= 0 && out.value(m3 + 1) == QLatin1String("M8"), "M8 follows the first M3");
    int lastM5 = -1;
    for (int i = 0; i < out.size(); ++i)
        if (out.at(i).trimmed() == QLatin1String("M05"))
            lastM5 = i;
    check(lastM5 > 0 && out.value(lastM5 - 1) == QLatin1String("M9"), "M9 precedes the final M5");
    const int tc = idx(out, starts(QStringLiteral("M0 ;T2")));
    const int m5tc = idx(out, is(QStringLiteral("M05")));
    check(tc > 0 && m5tc > 0 && m5tc < tc && out.value(m5tc - 1) == QLatin1String("M9"),
          "M9 before the spindle stops for the tool change");
    const int m3b = idx(out, starts(QStringLiteral("M03")), tc);
    check(m3b > tc && out.value(m3b + 1) == QLatin1String("M8"), "M8 again after the spindle restarts");
    check(out.count(QStringLiteral("M8")) == 2 && out.count(QStringLiteral("M9")) == 2,
          "one on and one off per spindle run");
    check(out.size() == prog.size() + 4, "nothing else changes");

    const QStringList mist = coolantTransform(prog, true);
    check(mist.count(QStringLiteral("M7")) == 2 && !mist.contains(QStringLiteral("M8")),
          "mist: M7 in place of M8");

    // A program that already switches coolant keeps its own.
    QStringList own = prog;
    own.insert(m3 + 1, QStringLiteral("M8 ; dust"));
    check(coolantTransform(own) == own, "own M8: left alone");

    // Not fooled by M30 or a comment.
    const QStringList other{QStringLiteral("(M3 in a name)"), QStringLiteral("G0 X1"), QStringLiteral("M30")};
    check(coolantTransform(other) == other, "no spindle: nothing added");
    const QStringList m3s{QStringLiteral("M3 S9000"), QStringLiteral("G1 X5 F500"), QStringLiteral("M5")};
    check(coolantTransform(m3s) == QStringList({QStringLiteral("M3 S9000"), QStringLiteral("M8"),
                                                QStringLiteral("G1 X5 F500"), QStringLiteral("M9"),
                                                QStringLiteral("M5")}),
          "short M3/M5 words count too");

    check(gamepadmap::actions().contains(QStringLiteral("coolant"))
              && gamepadmap::label(QStringLiteral("coolant")) != QLatin1String("coolant"),
          "gamepad: coolant toggle is an action with a label");

    std::printf("test_coolant: %d checks passed\n", g_checks);
    return 0;
}
