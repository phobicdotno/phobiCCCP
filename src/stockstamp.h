#pragma once
#include <functional>

namespace c2d {

// Stamp a change to a bit's stock counts or to the Collections with the time,
// so copies of the app on the network can tell which side is newer (see
// StockSync). The hook is called after each stamp; StockSync sets it.
void stampStock(int toolNumber);
void stampCollections();
void setStockChangedHook(std::function<void()> hook);

} // namespace c2d
