// Shared layer logger. Extracted from layer_entry.cpp (refactor phase 1) so the three
// translation units that use it stop re-declaring the same extern by hand (the old "implicit ABI").
#pragma once

namespace playspectra {

// Append a line to the layer log (path from PLAYSPECTRA_LOG, else %TEMP%\playspectra_layer.log). Implemented
// in layer_log.cpp. Thread-safe.
void LayerLog(const char* msg, const char* detail);

}  // namespace playspectra
