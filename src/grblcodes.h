#pragma once

#include <QString>

namespace c2d {

// GRBL 1.1 error and alarm codes in plain words, for the log and for the
// popup when a job stops: what the code means, the likely cause on a
// Shapeoko with a BitSetter and a BitZero, and what to do about it.
namespace grblcodes {

struct Explained {
    bool known = false;
    QString meaning;    // short, headline style: "Feed rate not set"
    QString brief;      // lower-case phrase that fits after "because": "an end stop fired"
    QString cause;      // likely cause on this machine
    QString fix;        // what to do
};

// error:N (1..38, and the grblHAL codes 18 and 19). Unknown codes give a
// generic text with known == false.
Explained error(int code);
// ALARM:N (1..10). Unknown codes give a generic text with known == false.
Explained alarm(int code);

// Finds "error:N" or "ALARM:N" anywhere in a line. False when there is none.
bool find(const QString &line, bool *isAlarm, int *code);

// "error:22 — Feed rate not set" / "ALARM:1 — Hard limit: an end stop fired".
QString label(bool isAlarm, int code);

// The popup body: meaning, likely cause, what to do, on separate paragraphs.
QString details(bool isAlarm, int code);

} // namespace grblcodes
} // namespace c2d
