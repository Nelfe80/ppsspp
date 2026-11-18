# NelfePlay coordinated challenge starts

This is downstream work for the NelfePlay fork. The upstream Netpacket branch
keeps `retro_serialize_size`, `retro_serialize` and `retro_unserialize` disabled
while Netplay is enabled. Nothing in this document enables loading a state.

## Preserved prototype

`Nelfe80/ppsspp:nelfeplay-netplay`, commit
`db2e59a4e095342ef59648a3bfa37657fb84047b`, contains the previous implementation
and its Bomberman test report. Keep it available as the reference implementation.
It restores PDP sockets and GameMode areas, rejoins the group and waits for the
other players. It does not restore PTP streams or matching sessions.

The new base is `926f630c1cac780b1c6a6f8c7daf3178f6aeb1da`. Port deliberately:

- The old branch added its own version 11 of `sceNetAdhoc`. Upstream now uses
  versions 11–13 for different fields. A version number alone cannot identify
  these two state layouts. Do not replay the old serializer patch or silently
  accept old network states. Identify the producing build in an outer manifest;
  initially create fresh challenge states with the new build.
- `friendFinderPoll()` now runs from the emulated ad hoc poll event. Freezing
  emulation also freezes that poll. The old hold loop cannot wait for this poll
  to rejoin a group: the reconnect handshake must progress independently, with
  the resulting PSP-visible state installed on the CPU thread before release.
- Frontend client IDs currently determine MAC addresses. A restore must bind
  each saved player slot to the correct participant, regardless of join order,
  or reject the roster. Copying the host's state to all players is invalid.
- The old five-minute timeout resumes with missing players. A challenge start
  must instead remain stopped and offer retry or cancellation.

## Ownership and integration points

| Component | State needed for a coordinated start |
| --- | --- |
| `libretro/libretro.cpp` | Load each player's own emulator state after boot; hold PSP execution while continuing frontend packet polling. |
| `LibretroNetpacket.cpp` | Session lifetime, frontend identity, launch manifest validation and the all-players-ready barrier. |
| `NetpacketAdhoc::State` | Game/group identity, saved-player-to-live-peer mapping, endpoint IDs, receive queues, outgoing packets and flow-control credit. Export values under its mutex, never pointers or synchronization primitives. |
| `NetpacketTransport::Socket` | PSP socket slot association, endpoint association, accepted-but-not-yet-returned connections, PTP blocks, offsets and readable times. These are owned by the adapter, independently of aemu/P2P. |
| `proAdhoc.cpp` | Restore the group's PSP-visible parameter, membership and BSSID without generating a second connection event. |
| `sceNetAdhoc.cpp` | PDP socket fields and queued datagrams, GameMode master/replica data and deadlines. Reconcile scheduled events with the restored emulated clock. |

The transport already starts PTP connections asynchronously and confines
frontend callbacks to the frontend thread. Preserve both properties: stopping
PSP execution must not stop the transport handshake. The control provider is
installed before boot and removed only after PSP networking threads have joined.

## First supported checkpoint

Start with a coordinated checkpoint between rounds in a PDP/GameMode game,
with no active PTP connection, matching session, pending ad hoc syscall, or
in-flight application datagram. Validate these restrictions at capture and
restore. If any is present, reject the checkpoint with the reason.

Use a separate, versioned NelfePlay session record next to each player's PSP
state rather than assigning downstream meanings to an upstream state-section
version. The manifest identifies the game hash/revision, core build, protocol
and session format, challenge/checkpoint ID, complete player roster, per-player
state hashes and saved MACs. Validate it before changing any live PSP state.

The host coordinates `PREPARE -> READY (every player) -> COMMIT`. A new launch
generation distinguishes packets from earlier runs. All participants validate
the same checkpoint and roster; each restores only its own state. Recreate the
endpoint map and group, then acknowledge readiness. Release the PSPs only after
the host's commit. A disconnect, wrong state or timeout aborts the launch;
there is no automatic solo continuation.

PTP and matching checkpoints require a later extension. PTP must preserve both
ends' queued data, segment boundaries, credits, accepted backlog and delivery
deadlines as one consistent cut. Reopening a TCP-like stream with empty queues
does not restore an established connection. Matching also has its own message
queues, callbacks and timing state.

## Validation before enabling

1. Capture and restore the public GameMode pair test using two real cores;
   verify the expected member roster, increasing peer counters and data.
2. Start the client late, reverse connection order, disconnect during PREPARE,
   and supply a state from another checkpoint. None may release one PSP alone.
3. Reject checkpoints with PTP/matching/pending calls until their state is
   implemented. Reject ambiguous legacy version-11 states before deserialization.
4. Repeat Bomberman setup, round intro and mid-round tests with the new build
   and both players' states. Only expand the allowed capture points after these
   tests demonstrate a consistent checkpoint.
5. Run ordinary savestate and Netplay-disabled regression tests, then repeat on
   Windows/RetroBat and two separate machines.

The existing `libretro/tests/core_pair.py` harness supplies the two-process
frontend for the first integration tests. Extend it with coordinated pause,
manifest exchange and delayed/mismatched-player scenarios when implementing
the downstream loader.
