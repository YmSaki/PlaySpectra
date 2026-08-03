// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

// Shared layer logger, declared once so the three translation units that use it stop re-declaring the
// same extern by hand (the old "implicit ABI").
#pragma once

namespace playspectra {

// Append a line to the layer log (path from PLAYSPECTRA_LOG, else %TEMP%\playspectra_layer.log). Implemented
// in layer_log.cpp. Thread-safe.
void LayerLog(const char* msg, const char* detail);

}  // namespace playspectra
