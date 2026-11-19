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

#include "libretro/NetpacketCheckpoint.h"
#include "libretro/LibretroNetpacket.h"
#include "Core/HLE/AdhocCheckpoint.h"
#include "Core/SaveState.h"
#include "Core/Config.h"
#include "Core/System.h"
#include "Core/ELF/ParamSFO.h"
#include "Common/Crypto/sha256.h"
#include "Common/Crypto/md5.h"
#include "Common/File/FileUtil.h"
#include "Common/TimeUtil.h"
#include <array>
#include <cstring>
#include <random>

extern const char *PPSSPP_GIT_VERSION;

namespace Libretro {
namespace {
using Token = std::array<u8, 16>;
enum class Phase { Idle, Load, WaitPrepare, FreezeHost, FreezeClient, WaitFrozen, WaitSave, Save, WaitSaved, WaitReady, Failed };
enum Message : u8 { PREPARE_CAPTURE = 1, FROZEN, SAVE, SAVED, PREPARE_RESUME, READY, COMMIT, ABORT };
constexpr size_t HEADER_SIZE = 168;
constexpr size_t MAX_STATE = 512 * 1024 * 1024;
constexpr size_t PACKET_SIZE = 4 + 1 + 16 + 16;
retro_environment_t environment = nullptr;
Phase phase = Phase::Idle;
u16 self = 65535;
bool started = false, peerConnected = false, captureLatched = false, loaded = false, saved = false;
bool identityAssigned = false;
bool peerReady = false, prepareReceived = false;
Token generation{}, checkpoint{};
std::string content, failure;
double deadline = 0, lastMessage = 0, lastRequestCheck = 0;

u32 Read32(const u8 *p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }
void Write32(u8 *p, u32 n) { for (int i = 0; i < 4; ++i) p[i] = n >> (8 * i); }
bool Option(const char *name) {
	retro_variable v{name, nullptr};
	return environment && environment(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value && !strcmp(v.value, "enabled");
}
Token NewToken() {
	std::random_device random;
	Token result;
	for (auto &byte : result) byte = (u8)random();
	return result;
}
void Send(Message message) {
	std::array<u8, PACKET_SIZE> packet{};
	memcpy(packet.data(), "NLC1", 4);
	packet[4] = message;
	memcpy(packet.data() + 5, generation.data(), 16);
	memcpy(packet.data() + 21, checkpoint.data(), 16);
	NetpacketSendCheckpoint(packet.data(), packet.size());
}
void Fail(const char *reason, bool notify = true) {
	if (phase == Phase::Failed) return;
	failure = reason;
	phase = Phase::Failed;
	ERROR_LOG(Log::sceNet, "NelfePlay checkpoint: %s. Emulation remains stopped.", reason);
	if (notify && started) Send(ABORT);
}
void Notice(const std::string &text) {
	retro_message message{text.c_str(), 180};
	if (environment) environment(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
}
std::string Stem() {
	auto id = g_paramSFO.GetDiscID();
	return id.empty() ? "homebrew" : id;
}
bool ContentHash(u8 *out) {
	FILE *file = File::OpenCFile(Path(content), "rb");
	if (!file) return false;
	md5_context hash;
	ppsspp_md5_starts(&hash);
	std::array<u8, 64 * 1024> buffer;
	size_t n;
	while ((n = fread(buffer.data(), 1, buffer.size(), file)) != 0)
		ppsspp_md5_update(&hash, buffer.data(), (int)n);
	bool ok = !ferror(file);
	fclose(file);
	ppsspp_md5_finish(&hash, out);
	return ok;
}
void ConfigIdentity(u8 *out) {
	Write32(out, (u32)g_Config.iPSPModel);
	Write32(out + 4, (u32)g_Config.iCpuCore);
	Write32(out + 8, (u32)g_Config.iLockedCPUSpeed);
	Write32(out + 12, (u32)g_Config.bEnableCheats);
}
void Digest(const std::vector<u8> &bytes, u8 out[32]) {
	sha256_context hash;
	sha256_starts(&hash);
	sha256_update(&hash, bytes.data(), 136);
	sha256_update(&hash, bytes.data() + HEADER_SIZE, (u32)(bytes.size() - HEADER_SIZE));
	sha256_finish(&hash, out);
}

bool SaveCheckpoint() {
	AdhocCheckpoint network;
	std::string error;
	if (!NetAdhocCaptureCheckpoint(network, error)) { Fail(error.c_str()); return false; }
	if (!NetpacketCheckpointReady()) { Fail("Transport still has pending data or discovery events"); return false; }
	size_t queued = 0, queuedBytes = 0;
	for (const auto &socket : network.sockets) {
		queued += socket.queue.size();
		for (const auto &packet : socket.queue) queuedBytes += packet.data.size();
	}
	INFO_LOG(Log::sceNet, "NelfePlay checkpoint: preserving %d PDP sockets, %d queued datagrams (%d bytes)",
		(int)network.sockets.size(), (int)queued, (int)queuedBytes);
	std::vector<u8> netBytes, pspBytes;
	SaveState::SaveStart psp;
	if (CChunkFileReader::MeasureAndSavePtr(network, &netBytes) != CChunkFileReader::ERROR_NONE ||
		CChunkFileReader::MeasureAndSavePtr(psp, &pspBytes) != CChunkFileReader::ERROR_NONE ||
		netBytes.size() > 8 * 1024 * 1024 || pspBytes.size() + netBytes.size() > MAX_STATE - HEADER_SIZE) {
		Fail("Could not capture emulator state"); return false;
	}
	std::vector<u8> bytes(HEADER_SIZE);
	memcpy(bytes.data(), "NLFPSSP1", 8);
	Write32(bytes.data() + 8, (u32)netBytes.size());
	Write32(bytes.data() + 12, (u32)pspBytes.size());
	Write32(bytes.data() + 16, self);
	Write32(bytes.data() + 20, 2);
	memcpy(bytes.data() + 24, checkpoint.data(), 16);
	if (strlen(PPSSPP_GIT_VERSION) >= 64) { Fail("Build identity is too long"); return false; }
	memcpy(bytes.data() + 40, PPSSPP_GIT_VERSION, strlen(PPSSPP_GIT_VERSION));
	if (!ContentHash(bytes.data() + 104)) { Fail("Cannot hash the game content"); return false; }
	ConfigIdentity(bytes.data() + 120);
	bytes.insert(bytes.end(), netBytes.begin(), netBytes.end());
	bytes.insert(bytes.end(), pspBytes.begin(), pspBytes.end());
	Digest(bytes, bytes.data() + 136);
	Path folder = g_Config.internalDataDirectory / "netplay" / "captures";
	std::string name = Stem() + "-player" + std::to_string(self + 1) + ".state";
	Path path = folder / name, temporary = folder / (name + ".tmp");
	if (!File::CreateFullPath(folder) || !File::WriteDataToFile(false, bytes.data(), bytes.size(), temporary) ||
		!File::Rename(temporary, path)) {
		Fail("Cannot write checkpoint file"); return false;
	}
	INFO_LOG(Log::sceNet, "NelfePlay checkpoint saved: %s", path.c_str());
	return true;
}

bool LoadCheckpoint() {
	Path path = g_Config.internalDataDirectory / "netplay" / (Stem() + ".state");
	u64 size = File::GetFileSize(path);
	if (size < HEADER_SIZE || size > MAX_STATE) { Fail("Prepared checkpoint missing or oversized"); return false; }
	std::string raw;
	if (!File::ReadBinaryFileToString(path, &raw) || raw.size() != size) { Fail("Cannot read prepared checkpoint"); return false; }
	std::vector<u8> bytes(raw.begin(), raw.end());
	// Only our identified envelope is accepted. In particular, the old downstream sceNetAdhoc
	// version 11 must never reach the upstream version 11-13 deserializer.
	if (memcmp(bytes.data(), "NLFPSSP1", 8) || Read32(bytes.data() + 20) != 2 ||
		Read32(bytes.data() + 16) != self) { Fail("Legacy checkpoint, unsupported roster or wrong player"); return false; }
	u32 netSize = Read32(bytes.data() + 8), pspSize = Read32(bytes.data() + 12);
	if (netSize > 8 * 1024 * 1024 || !netSize || !pspSize || u64(netSize) + pspSize + HEADER_SIZE != bytes.size()) {
		Fail("Invalid checkpoint lengths"); return false;
	}
	std::array<u8, 64> build{};
	if (strlen(PPSSPP_GIT_VERSION) >= build.size()) { Fail("Build identity is too long"); return false; }
	memcpy(build.data(), PPSSPP_GIT_VERSION, strlen(PPSSPP_GIT_VERSION));
	u8 contentHash[16], config[16], digest[32];
	ConfigIdentity(config);
	Digest(bytes, digest);
	if (memcmp(digest, bytes.data() + 136, 32) || memcmp(build.data(), bytes.data() + 40, 64) ||
		memcmp(config, bytes.data() + 120, 16) || !ContentHash(contentHash) || memcmp(contentHash, bytes.data() + 104, 16)) {
		Fail("Checkpoint hash, producing build, options or game content differs"); return false;
	}
	Token token;
	memcpy(token.data(), bytes.data() + 24, 16);
	if (prepareReceived && token != checkpoint) { Fail("Players selected different checkpoints"); return false; }
	AdhocCheckpoint network;
	std::string error;
	if (CChunkFileReader::LoadPtr(bytes.data() + HEADER_SIZE, netSize, network, &error) != CChunkFileReader::ERROR_NONE ||
		!network.Validate(self)) { Fail("Unsupported or corrupt networking state"); return false; }
	SaveState::SaveStart psp;
	if (CChunkFileReader::LoadPtr(bytes.data() + HEADER_SIZE + netSize, pspSize, psp, &error) != CChunkFileReader::ERROR_NONE ||
		!NetAdhocRestoreCheckpoint(network)) { Fail("Could not restore the complete checkpoint"); return false; }
	checkpoint = token;
	INFO_LOG(Log::sceNet, "NelfePlay checkpoint loaded for player %u", self + 1);
	return true;
}
}

void CheckpointInit(retro_environment_t env, const char *path) {
	environment = env; content = path ? path : "";
	self = 65535; started = peerConnected = loaded = saved = peerReady = prepareReceived = false;
	identityAssigned = false;
	generation = {}; checkpoint = {}; failure.clear(); deadline = lastMessage = lastRequestCheck = 0;
	captureLatched = false;
	phase = Option("ppsspp_retroarch_netplay_start") ? Phase::Load : Phase::Idle;
	if (phase != Phase::Idle && !NetpacketEnabled()) Fail("Challenge start requires RetroArch Netplay", false);
}
void CheckpointStart(uint16_t id) {
	if (started && phase != Phase::Idle) { Fail("Session restarted during checkpoint"); return; }
	self = id; started = identityAssigned = true;
	if (phase != Phase::Idle && self > 1) Fail("Checkpoints require player IDs 0 and 1");
}
void CheckpointConnected(uint16_t peer) { if (peer == 1) peerConnected = true; }
void CheckpointDisconnected(uint16_t peer) {
	if (peer == 1) peerConnected = false;
	if (phase != Phase::Idle) Fail("Player disconnected during checkpoint");
}
void CheckpointStop() {
	if (phase != Phase::Idle && started) Fail("Netplay stopped during checkpoint", false);
	started = false;
}

bool CheckpointReceive(const void *data, size_t size, uint16_t from) {
	if (!data || size < 4 || memcmp(data, "NLC1", 4)) return false;
	if (!started || self > 1 || from != (self ^ 1) || size != PACKET_SIZE) return true;
	auto bytes = (const u8 *)data;
	Token incomingGeneration, incomingCheckpoint;
	memcpy(incomingGeneration.data(), bytes + 5, 16);
	memcpy(incomingCheckpoint.data(), bytes + 21, 16);
	Message message = (Message)bytes[4];
	if (message == PREPARE_CAPTURE && self == 1 && phase == Phase::Idle) {
		generation = incomingGeneration; checkpoint = incomingCheckpoint;
		phase = Phase::FreezeClient;
		deadline = time_now_d() + 60;
		return true;
	}
	if (message == PREPARE_RESUME && self == 1 && (phase == Phase::Load || phase == Phase::WaitPrepare)) {
		if (loaded && checkpoint != incomingCheckpoint) { Fail("Players selected different checkpoints"); return true; }
		generation = incomingGeneration; checkpoint = incomingCheckpoint; prepareReceived = true;
		return true;
	}
	if (message == PREPARE_RESUME && self == 1 && phase == Phase::Idle) {
		generation = incomingGeneration; checkpoint = incomingCheckpoint;
		Fail("Host requested a challenge but this player did not enable challenge start");
		return true;
	}
	if (message == ABORT && phase != Phase::Idle) { Fail("Other player rejected the checkpoint", false); return true; }
	if (incomingGeneration != generation || incomingCheckpoint != checkpoint) return true;
	if (message == FROZEN && self == 0 && phase == Phase::WaitFrozen) {
		phase = Phase::Save;
		Send(SAVE);
	} else if (message == SAVE && self == 1 && phase == Phase::WaitSave) {
		phase = Phase::Save;
	} else if (message == SAVED && self == 0 && (phase == Phase::Save || phase == Phase::WaitSaved)) {
		peerReady = true;
	} else if (message == READY && self == 0 && phase == Phase::WaitReady) {
		peerReady = true;
	} else if (message == COMMIT && self == 1 && ((phase == Phase::WaitSaved && saved) || (phase == Phase::WaitReady && loaded))) {
		phase = Phase::Idle;
		INFO_LOG(Log::sceNet, "NelfePlay checkpoint committed; resuming player 2");
	}
	return true;
}

bool CheckpointNeeded() {
	bool capture = Option("ppsspp_retroarch_netplay_capture");
	// A tool (or a lab operator) can also ask the host for a capture without opening the frontend's menu, which would
	// pause the host's PSP and break GameMode on the other side: PPSSPP/netplay/capture.request, removed when seen.
	bool requested = false;
	if (phase == Phase::Idle && NetpacketEnabled() && started && self == 0 && time_now_d() - lastRequestCheck > 0.5) {
		lastRequestCheck = time_now_d();
		Path request = g_Config.internalDataDirectory / "netplay" / "capture.request";
		if (File::Exists(request)) {
			File::Delete(request);
			requested = true;
			INFO_LOG(Log::sceNet, "NelfePlay checkpoint: capture requested by %s", request.c_str());
		}
	}
	if (phase == Phase::Idle && NetpacketEnabled() && started && self == 0 && ((capture && !captureLatched) || requested)) {
		generation = checkpoint = NewToken();
		peerReady = saved = false;
		phase = Phase::FreezeHost;
		deadline = time_now_d() + 60;
	}
	captureLatched = capture;
	return phase != Phase::Idle || (NetpacketEnabled() && !identityAssigned);
}

bool CheckpointFrame() {
	if (phase == Phase::Idle) {
		if (NetpacketEnabled() && !identityAssigned) {
			// PSP software may cache its MAC at boot, before its wireless menu opens.
			// Do not let the client boot with the host's provisional ID/MAC.
			if (time_now_d() - lastMessage > 3) {
				Notice("Start or join RetroArch Netplay to assign this PSP's player identity.");
				lastMessage = time_now_d();
			}
			return true;
		}
		return false;
	}
	// A capture is bounded from its start. A challenge start waits without limit for the other player to join the
	// RetroArch session (a launcher can take longer than a minute to bring a slower machine in), then is bounded.
	if (!deadline) {
		bool resuming = phase == Phase::Load || phase == Phase::WaitPrepare || phase == Phase::WaitReady;
		bool together = started && (self != 0 || peerConnected);
		if (!resuming || together)
			deadline = time_now_d() + 60;
	}
	if (phase != Phase::Failed && deadline && time_now_d() > deadline) Fail("Checkpoint timed out; restart the session to retry");
	if (phase != Phase::Failed && started && !NetpacketActive()) Fail("Packet transport stopped");
	// Called only after pausing the CPU and draining its render work. Reliable packet ordering
	// makes PREPARE/FROZEN markers delimit all application data sent before each local pause.
	NetpacketPoll();
	if (phase == Phase::FreezeHost && peerConnected) {
		phase = Phase::WaitFrozen; Send(PREPARE_CAPTURE);
	} else if (phase == Phase::FreezeClient) {
		phase = Phase::WaitSave; Send(FROZEN);
	} else if (phase == Phase::Save) {
		if (SaveCheckpoint()) {
			saved = true; phase = Phase::WaitSaved;
			if (self == 1) Send(SAVED);
		}
	} else if (phase == Phase::Load && started) {
		if (LoadCheckpoint()) { loaded = true; phase = Phase::WaitPrepare; }
	}
	if (phase == Phase::WaitPrepare && self == 0 && peerConnected) {
		generation = NewToken(); phase = Phase::WaitReady; Send(PREPARE_RESUME);
	} else if (phase == Phase::WaitPrepare && self == 1 && prepareReceived) {
		phase = Phase::WaitReady; Send(READY);
	}
	if (self == 0 && peerReady && ((phase == Phase::WaitSaved && saved) || (phase == Phase::WaitReady && loaded))) {
		Send(COMMIT);
		phase = Phase::Idle;
		INFO_LOG(Log::sceNet, "NelfePlay checkpoint committed; resuming player 1");
		Notice(saved ? "Both checkpoint files saved in PPSSPP/netplay/captures" : "NelfePlay: both players ready");
	}
	if (phase != Phase::Idle && time_now_d() - lastMessage > 3) {
		Notice(phase == Phase::Failed ? "NelfePlay: " + failure + ". Restart content to retry." :
			!deadline ? "NelfePlay: waiting for the other player to join..." : "NelfePlay: waiting for both players' checkpoints...");
		lastMessage = time_now_d();
	}
	return phase != Phase::Idle;
}
}
