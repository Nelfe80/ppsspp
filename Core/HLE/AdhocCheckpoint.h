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

#include "Core/HLE/proAdhoc.h"

// Downstream checkpoint data, deliberately outside sceNetAdhoc's upstream state layout.
// Capture/restore run with the CPU paused, after both frontend packet streams have drained.
struct AdhocCheckpoint {
	struct Datagram {
		SceNetEtherAddr mac{};
		u16 port = 0;
		std::vector<u8> data;
	};
	struct Socket {
		u32 slot = 0, bufferSize = 0;
		SceNetEtherAddr mac{};
		u16 port = 0;
		s32 flags = 0, alertedFlags = 0, nonblocking = 0;
		bool isClient = false, reliable = false;
		std::vector<Datagram> queue;
	};
	struct Area {
		GameModeArea value{};  // data is never serialized or copied into the live state.
		std::vector<u8> bytes;
	};
	SceNetAdhocctlParameter parameter{};
	s32 mode = 0, state = 0, gameSocket = -1, gameBufferSize = 0;
	std::vector<SceNetAdhocctlPeerInfo> peers;  // next is never serialized.
	std::vector<Socket> sockets;
	Area master;
	std::vector<Area> replicas;
	std::vector<SceNetEtherAddr> macs, requiredMacs;
	std::vector<std::pair<SceNetEtherAddr, u16>> peerPorts;
	void DoState(PointerWrap &p);
	bool Validate(u16 self) const;
};

bool NetAdhocCaptureCheckpoint(AdhocCheckpoint &state, std::string &error);
// Call after the emulator state has loaded. The provider recreates the external group synchronously.
bool NetAdhocRestoreCheckpoint(const AdhocCheckpoint &state);
