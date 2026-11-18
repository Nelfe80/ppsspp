// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once
#include "libretro.h"

namespace Libretro {
void CheckpointInit(retro_environment_t environment, const char *content);
void CheckpointStart(uint16_t self);
void CheckpointConnected(uint16_t peer);
void CheckpointDisconnected(uint16_t peer);
void CheckpointStop();
bool CheckpointReceive(const void *data, size_t size, uint16_t from);
// All functions run on the frontend thread. Frame must run with the CPU paused and context held.
bool CheckpointNeeded();
bool CheckpointFrame();
}
