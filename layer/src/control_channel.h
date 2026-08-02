// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

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
