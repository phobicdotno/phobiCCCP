// GRBL error and alarm codes in plain words: every GRBL 1.1 error (1..38)
// and alarm (1..10) has a meaning, a likely cause and a fix; an unknown
// code still gives a usable generic text; codes are found in log lines.
//
// Plain asserts, no framework; exits 0 on success.

#include "../src/grblcodes.h"

#include <cstdio>

using namespace c2d;

static int failures = 0;
static void check(bool ok, const char *what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static bool full(const grblcodes::Explained &e)
{
    return !e.meaning.isEmpty() && !e.brief.isEmpty() && !e.cause.isEmpty() && !e.fix.isEmpty();
}

int main()
{
    for (int n = 1; n <= 38; ++n) {
        const auto e = grblcodes::error(n);
        if (!e.known || !full(e))
            std::fprintf(stderr, "error:%d\n", n);
        check(e.known && full(e), "every error 1..38 has text");
        check(grblcodes::label(false, n).startsWith(QStringLiteral("error:%1 — ").arg(n)),
              "error label starts with its code");
    }
    for (int n = 1; n <= 10; ++n) {
        const auto e = grblcodes::alarm(n);
        if (!e.known || !full(e))
            std::fprintf(stderr, "ALARM:%d\n", n);
        check(e.known && full(e), "every alarm 1..10 has text");
        check(grblcodes::details(true, n).contains(e.fix), "details carry the fix");
    }

    const auto ue = grblcodes::error(99);
    check(!ue.known && full(ue) && ue.meaning.contains(QStringLiteral("99")), "unknown error: generic text");
    const auto ua = grblcodes::alarm(42);
    check(!ua.known && full(ua) && ua.meaning.contains(QStringLiteral("42")), "unknown alarm: generic text");
    check(!grblcodes::error(0).known && full(grblcodes::error(0)), "error 0: generic text");

    bool isAlarm = true;
    int code = 0;
    check(grblcodes::find(QStringLiteral("program stopped: error:22 on line 5"), &isAlarm, &code)
              && !isAlarm && code == 22, "finds error:22 in a stop message");
    check(grblcodes::find(QStringLiteral("ALARM:10"), &isAlarm, &code) && isAlarm && code == 10,
          "finds ALARM:10");
    check(!grblcodes::find(QStringLiteral("ok"), &isAlarm, &code), "no code in ok");

    if (failures)
        return 1;
    std::printf("grblcodes: all passed\n");
    return 0;
}
