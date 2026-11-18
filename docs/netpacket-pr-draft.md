### Claude says

This draft was prepared with OpenAI Codex; the heading above follows this repository's agent instructions.

Follow-up to #22832 using the transport abstraction introduced in #22855, on `926f630c1cac780b1c6a6f8c7daf3178f6aeb1da`. It lets a libretro frontend carry PSP ad hoc discovery, PDP datagrams and PTP streams through Netpacket. Each participant runs a separate PSP.

The new `NetpacketTransport` implements `AdhocTransport`. Shared HLE retains validation, emulated blocking waits, PDP receive queues, GameMode and matching. A small overridable control provider supplies group discovery and MAC identity; its default still forwards to aemu. PTP connection setup is nonblocking, and only the frontend thread calls Netpacket send callbacks.

The option defaults off and requires a content restart. Network savestates remain disabled. No HLE table or serialized-state layout changes are included, and the old GameMode resend workaround is omitted in favor of the current upstream HLE.

Validation on Linux:
- Built the libretro core, PPSSPPHeadless and PPSSPPUnitTest.
- All 107 C++ unit tests passed.
- `python test.py -g --graphics=software` exited 0: 527 single-console executions and 21 pair scenarios (42 programs) passed, using the suite's existing exclusions.
- Standalone transport tests passed, including ASan/UBSan (leak detection disabled in this environment).
- Actual-core smoke tests passed for enabled, disabled, unsupported-frontend and unload/reload paths.
- Two actual core processes passed targeted discovery/PDP/PTP and GameMode payload/lifecycle assertions through Netpacket callbacks.
- Extra strict relay probes reproduce the same closed-port timing and GameMode scan-mode mismatches on the unmodified upstream base. The Netpacket pair harness does not claim a full hardware-output comparison.

This remains experimental. Windows/RetroBat, RetroArch itself, commercial games and Internet/relay latency have not been validated with this revision. The previous Bomberman report belongs to the old branch. Main remaining risks are lifecycle/teardown, PSP network timing, games caching MACs before the frontend starts, and frontend delivery behavior. The GameMode scan-mode flag is not yet represented by the discovery protocol.

Build/test commands and RetroBat installation notes are in `libretro/README_NETPLAY.md`. Coordinated savestate work stays downstream and is not part of this PR.

Downstream integration review: the current RetroBat-APIExpose launcher expects a separate `ppsspp_netplay` core and per-player `core_start` states. This upstream transport is not a drop-in replacement for that packaged core. The downstream restoration plan records the existing launch option, prepared-state path, archive hash handling and host/seat mapping.
