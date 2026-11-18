# NelfePlay coordinated challenge starts

This downstream branch implements experimental **two-player PDP/GameMode
checkpoints** on top of the transport introduced by upstream #22855. The upstream
`libretro-netpacket-transport` branch remains transport-only. Ordinary RetroArch
`retro_serialize*` / `retro_unserialize` calls remain disabled in packet mode;
NelfePlay uses the core-owned capture and startup loader described here.

## Capture and start a challenge

Use the same core binary, game revision, PSP model, CPU backend, clock and cheat
settings for capture and playback. Turn off cheats for initial validation; the
format records the enable flag, not the contents of cheat files. The first
supported targets are stable two-player PDP/GameMode sessions. PTP sockets,
matching contexts, pending ad hoc calls, callbacks and incomplete groups cause
capture to fail. Do not use an old branch's savestates with this build.

1. Enable `ppsspp_retroarch_netplay` on both machines and restart content. Start
   or join RetroArch Netplay. PSP execution waits for the frontend to assign
   the local player identity, so a game cannot cache the other player's MAC.
2. Join the game's own ad hoc session. At a suitable checkpoint, enable
   `ppsspp_retroarch_netplay_capture` **on the RetroArch host**. Both PSPs pause,
   drain their reliable packet streams and save their own state. Existing PDP
   receive queues are included. They resume only after both files are saved.
3. Collect `PPSSPP/netplay/captures/<game ID>-player1.state` from the host and
   `<game ID>-player2.state` from the client, below each RetroArch system
   directory. The files share a checkpoint ID, but their contents differ.
   Disable the capture option before requesting another capture.
4. For a fresh launch, install the correct player's file as
   `PPSSPP/netplay/<game ID>.state` and enable
   `ppsspp_retroarch_netplay_start` on both machines before loading content.
   NelfePlay's API can prepare this file. The core currently uses this canonical
   path; arbitrary catalogue `core_start.path` values are not auto-discovered.
5. Host/join RetroArch Netplay. Each core validates and restores its own file,
   reconstructs the network endpoints, then waits at the readiness barrier.
   The host releases both PSPs only after both are ready.

Missing or incompatible files, a disconnect or the 60-second deadline leave
emulation stopped. Restart content to retry. There is no fallback to a solo
challenge and no ordinary runtime load/rollback support in packet mode.
A file saved before a failed two-player capture is not a valid pair; use only
pairs for which both participants reported completion.

## Current launcher contract

Source review on 2026-11-18 covered `Nelfe80/RetroBat-APIExpose` at
`ea98f9b63b7dc684b6d67856bec7b0f8899cdc27` and `Nelfe80/RetroArch` at
`c58d703a2e6982e1508952acebab2dcc888c4dca`.

| Existing contract | This core |
| --- | --- |
| Separate `ppsspp_netplay_libretro.dll`, `.info` and license alongside the solo core. | Preserve these names. Validate the experimental package before replacing the distributed NelfePlay build. |
| `ppsspp_retroarch_netplay_start` selects automatic startup. | The option now invokes a loader and readiness barrier, rather than being silently ignored. |
| The API prepares each seat's state under the system directory, usually RetroBat `bios`. | Use `PPSSPP/netplay/<game ID>.state`. Loading happens once per content launch, after PSP initialization and before PSP execution. |
| The API verifies `core_start.content` (ROM MD5), player count and per-seat SHA-256. | Keep these catalogue checks. The core also verifies the installed raw envelope, its player, producing Git version, content and recorded options. |
| The API checks delivered bytes, then decompresses `.gz`. | The catalogue hash belongs to the delivered archive. The envelope's internal SHA-256 belongs to the raw state; these hashes are different. |
| The API permutes state seats, scores and statistics when the host is platform seat 2. | Consume that mapping once: packet ID 0 loads player 1, packet ID 1 loads player 2. No second permutation occurs in the core. |
| The API removes previously prepared files and fails on missing downloads. | Keep that behavior; the core independently refuses missing files. |
| RetroArch skips its normal savestate synchronization in packet sessions. | The core performs coordinated loading itself. Simply enabling `retro_unserialize()` would not meet this contract. |

Public source references:

- [Core installation](https://github.com/Nelfe80/RetroBat-APIExpose/blob/ea98f9b63b7dc684b6d67856bec7b0f8899cdc27/src/RetroBat.Api/Multiplayer/MultiplayerCoeursService.cs)
- [Prepared states](https://github.com/Nelfe80/RetroBat-APIExpose/blob/ea98f9b63b7dc684b6d67856bec7b0f8899cdc27/src/RetroBat.Api/Multiplayer/MultiplayerDepartDuCoeur.cs)
- [Seat mapping](https://github.com/Nelfe80/RetroBat-APIExpose/blob/ea98f9b63b7dc684b6d67856bec7b0f8899cdc27/src/RetroBat.Api/Multiplayer/MultiplayerOrdreNetplay.cs)
- [RetroArch packet lifecycle](https://github.com/Nelfe80/RetroArch/blob/c58d703a2e6982e1508952acebab2dcc888c4dca/network/netplay/netplay_frontend.c)

The downstream protocol is `ppsspp-nelfeplay-checkpoint-1`. All participants
must use this build; it cannot join the older experimental-1 or experimental-2
protocols. The previous `nelfeplay-netplay` implementation and its Bomberman
report remain at `db2e59a4e095342ef59648a3bfa37657fb84047b` for reference.
That report does not validate this new implementation.

## State envelope and synchronization

The `NLFPSSP1` envelope contains a separate `NelfeAdhoc` version-1 record and
PPSSPP's normal `SaveState::SaveStart` payload. Upstream `sceNetAdhoc` remains
version 13, with its original layout and syscall indices unchanged. The old
fork assigned its own meaning to version 11, which conflicts with newer
upstream states. Legacy files are rejected before entering that deserializer.
Fresh captures are required.

| Offset | Bytes | Meaning |
| --- | ---: | --- |
| 0 | 8 | `NLFPSSP1` marker |
| 8 | 4 | Network record length, little endian |
| 12 | 4 | PSP state length, little endian |
| 16 | 4 | Packet player ID, 0 or 1 |
| 20 | 4 | Player count, exactly 2 |
| 24 | 16 | Shared capture identity |
| 40 | 64 | Zero-padded `PPSSPP_GIT_VERSION` |
| 104 | 16 | This player's content-file MD5 |
| 120 | 16 | PSP model, CPU backend, clock, cheat-enable flag (four LE integers) |
| 136 | 32 | SHA-256 of bytes 0–135 followed by the payload |
| 168 | variable | Network record, then PSP state |

File size is bounded at 512 MiB and the network record at 8 MiB. Counts,
packet lengths, player MACs, socket slots and GameMode memory ranges are
validated. The Git version check is not a binary signature: distribute the
same packaged binary, and do not produce challenge states from dirty or
independently configured builds sharing that version string.

Capture uses ordered reliable `PREPARE_CAPTURE / FROZEN / SAVE / SAVED /
COMMIT` markers. After pausing the CPU, the frontend drains outgoing packets;
the markers delimit application data from both players. Incoming PDP data is
moved into PSP-owned queues and serialized, including payloads already waiting
for the game. Fresh host-side pointers and endpoint IDs are created on load.
PSP socket slots, group membership, master/replica buffers, update flags and
timestamps are restored. Restored CoreTiming events are retained.

Resume uses `PREPARE_RESUME / READY / COMMIT` with a new launch generation and
the shared capture identity. It does not wait for an emulated poll event to
rejoin while emulation is frozen, or send a duplicate PSP CONNECT callback.
A mismatch aborts the launch. Packet callbacks remain on the frontend thread;
the emulation thread is paused before reading or changing state.

## Lessons checked against mGBA Splitscreen

The review used `Nelfe80/mgba-splitscreen:nelfeplay-build` at
`d03d6a130da240c389eb1caf7468cfa316223fc3` and the linked-savestate branches,
not just that repository's master.

| GBA finding | PSP consequence |
| --- | --- |
| `adc72de3`: missing SIOMLT_SEND/SIODATA8 state; ready flags derived from the previous session. | Restore network-visible buffers and readiness from the saved session. Validate after completely exiting and relaunching both processes. |
| `cef98136`: event/free-list tails needed termination; stale links caused later hangs. | Never serialize process pointers. Rebuild owned nodes and queues, retain queued payloads, and exercise traffic after loading. |
| Linked coordinator state includes queues, offsets and sleeping players. Resetting it after loading destroys the restored link. | Preserve CoreTiming events and queued data; recreate only external endpoints, without resetting the loaded PSP connection. |
| `1618bb74`: linked instances must exist before startup state synchronization. | Assign frontend player identity before PSP execution; hold both instances through preparation. |
| `acb301cd`: AV information updates belong in `retro_run`, not `retro_load_game`. | Checkpoint work stays in `retro_run`; this change adds no AV update during content loading. |

The GBA lockstep scheduler and its RAM lock table are specific to that core.
They are not transplanted into PSP's independent consoles and packet transport.

## Validation and remaining limits

Linux validation uses a real compiled core in each of two separate processes:

```sh
cmake -S libretro/tests -B build-netpacket-tests
cmake --build build-netpacket-tests
ctest --test-dir build-netpacket-tests --output-on-failure
python3 libretro/tests/core_pair.py build-libretro/lib/ppsspp_libretro.so
python3 libretro/tests/checkpoint_pair.py build-libretro/lib/ppsspp_libretro.so --timeout-case
```

The checkpoint test captures both public PSP GameMode programs with nonempty
receive queues, exits both processes, then relaunches and loads them. It checks
40+ replica updates, intact magic values, increasing counters/timestamps and
successful GameMode exit. It repeats with the client two seconds late. Wrong
player, legacy format, different checkpoint, Git build, content, options,
corruption, missing file, disabled client start, disconnect and the real
60-second deadline must leave both consoles stopped.

The transport test also covers restored endpoint maps without duplicate
CONNECT events and rejects capture while data or PTP endpoints remain.
The ordinary disabled-Netplay smoke path verifies a savestate round-trip.
General regression suites are `build/PPSSPPUnitTest all` and
`python test.py -g --graphics=software` (107 C++ and 507 main PSP tests passed,
plus the suite's relay/single-console and paired networking runs).

Still required before production use: a real two-machine RetroArch/RetroBat
run, graphics-driver/threaded-renderer coverage, actual-game capture and
restore (including Bomberman), API startup with the host in either platform
seat, archive delivery/cleanup and scores, and Internet/relay latency tests.
This implementation does not claim savestates for PTP or matching, more than
two players, infrastructure networking, hot loading or rollback. A failed
capture requires restarting content. Windows CI builds a test package; building
and smoke-testing a DLL does not establish commercial-game compatibility.
