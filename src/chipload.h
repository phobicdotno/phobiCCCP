#pragma once
#include <QString>

// Chipload (feed per tooth): how thick a chip each flute takes,
// feed (mm/min) / (rpm x flutes). Too thin and the bit rubs and heats up
// instead of cutting; too thick and a small bit may snap. The range for a bit
// comes from the tool library's own feeds for the material where it has
// them, else from a small built-in table by bit diameter.
namespace c2d {

class ToolLibrary;

struct ChiploadRange {
    double low = 0, mid = 0, high = 0;      // mm per tooth
    bool fromLibrary = false;               // false: the built-in table
    bool valid() const { return mid > 0; }
};

enum class ChiploadVerdict { Unknown, TooLow, Ok, TooHigh };

// feed / (rpm x flutes); 0 when any of them is not positive.
double chiploadOf(double feed, double rpm, int flutes);
// The library entry's chipload ±40 %.
ChiploadRange chiploadRangeFrom(double libFeed, double libRpm, int flutes);
// Built-in table, for a bit with no library feeds for the material.
ChiploadRange chiploadTable(double diameter, const QString &materialId);
// The library tool with this number and diameter if it has feeds for the
// material, else the table. lib may be null.
ChiploadRange chiploadRangeFor(const ToolLibrary *lib, int toolNumber, double diameter,
                               int flutes, const QString &materialId);
ChiploadVerdict classifyChipload(double chipload, const ChiploadRange &r);
// "Chipload 0.050 mm/tooth: OK" / "...: too low (rubbing, heat)" / ...; empty when unknown.
QString chiploadText(double chipload, const ChiploadRange &r);
// Feed (mm/min) for the middle of the range at this rpm, rounded to 10.
double suggestedFeed(const ChiploadRange &r, double rpm, int flutes);
// A spindle speed to start from for the material: 10000 for aluminium, else 18000.
double defaultRpmFor(const QString &materialId);

} // namespace c2d
