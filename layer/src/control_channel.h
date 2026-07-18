// PlaySpectra control channel: localhost TCP NDJSON server embedded in the OpenXR API layer.
//
// The MCP server (TypeScript) connects here and sends one JSON object per line. Commands are
// dispatched to handlers that read/write layer state (layer_state.h). The channel runs on a
// background thread; state access from hooks happens on the app thread via the layer_state API.

#pragma once

namespace playspectra {

void ControlChannelStart();
void ControlChannelStop();

}  // namespace playspectra
