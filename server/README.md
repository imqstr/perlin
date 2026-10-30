# Perlin server

Server software that lives inside the Perlin tree but builds on its own. One codebase, three ways to run it:

| Target | What it is |
| --- | --- |
| `perlin-server` | Dedicated server, like Bedrock Dedicated Server. Reads `server.json`, loads plugins, takes console commands on stdin. |
| `perlin_server` (`.dll` / `.so` / `.dylib`) | The same server behind a small C API (`include/perlin/server.h`) for singleplayer inside the client. |
| `perlin_server_core` | Static library both of the above are made from. |

```
cmake -S server -B build-server && cmake --build build-server -j
./build-server/perlin-server --data run        # writes run/server.json on first start
ctest --test-dir build-server
```

From the root tree: `-DPERLIN_BUILD_SERVER=ON`. Needs a C++23 compiler, Python 3 (the Bedrock packet codecs are generated from `protocol/` by the client's own `tools/protogen`, so the server speaks exactly what the client decodes), and network access for the first configure (libdatachannel, cpp-httplib, miniz, wasm3 through FetchContent). Options: `PERLIN_SERVER_NETHERNET` (default ON), `PERLIN_SERVER_GPU` (OFF, see below), `PERLIN_SERVER_BUILD_SHARED`, `PERLIN_SERVER_BUILD_EXE`.

## Transport: NetherNet only

There is no RakNet in the server. A player joins the way the client's direct join already does:

1. `GET /v1/join` returns the server info (name, protocol, version, players).
2. `POST /v1/join/<id>` carries the client's SDP offer; the answer comes back in the response.
3. Two WebRTC data channels, `ReliableDataChannel` and `UnreliableDataChannel`, carry game batches. Each message is `[fragments still to come][payload]`, 262143 bytes per fragment.

DTLS already secures the channel, so the Bedrock encryption handshake is not used (the client replies to `ServerToClientHandshake` without encryption on NetherNet). Compression is raw deflate with a 256 byte threshold.

Reachability: forward one port, TCP for the signalling and UDP for WebRTC, both `port` (default 19133). `ice_udp_mux` (default on) puts every player on that one UDP port. `ice_servers` takes `stun:host:port` / `turn:` URLs, or `["none"]` for LAN only. `ice_bind_address` pins the interface (needed on hosts without IPv6).

The in-process `LoopbackListener` skips the network entirely and is what singleplayer uses.

## What works

- Bedrock 1.26.60 (protocol 2225) login: network settings, login, resource pack negotiation (no packs offered), start game, chunk radius, chunk streaming, spawn.
- Flat world (`flat_layers` in `server.json`, default bedrock / 2 dirt / grass, surface at y -60), sections that are one block are stored and sent as a single value.
- Multiplayer: any number of sessions, player list, other players appear with their own skin, movement is relayed, join and leave messages.
- Chat (`Text`), commands (`/help /list /say /tp /kick /plugins /stop`), command list sent to the client. Operators are listed in `operators` (names, or Xbox ids); in embedded singleplayer the one player is an operator.
- Building: the creative menu (every block the client knows, from its own `block_states.json`), placing, breaking, pick block, the 36-slot inventory with cursor, hotbar selection, swap / take / place / drop / destroy through the server-authoritative item stack requests (with stack ids and atomic rollback). Reach is checked, plugins can cancel a place or break, and a refused action sends the real block back so the client undoes its guess. In survival, placing spends one item and breaking pays the block straight into the inventory; bedrock does not break.
- WASM plugins.
- Xbox authentication (see below), GPU terrain (see below).

Not modelled on purpose: entities other than players, so no item entities (a dropped stack is simply gone), no mobs, no crafting grid or recipes, no containers, no block states chosen by the placer (a placed block has its default state: logs face up, stairs do not rotate), no armour.

## Xbox authentication

`"xbox_auth": true` in `server.json` makes the server accept only Xbox Live identities. Every login's certificate chain is always checked (ES384 signatures, each link signed by the key the previous link named, expiry, the client data signed by the last key); `xbox_auth` decides what a valid chain is worth:

- **off (default):** the chain only proves the player holds the key it names. The name is a label anyone can pick; no Xbox id is trusted.
- **on:** the chain must pass through a trusted root key (Mojang's, or the keys in `auth_root_keys`), the name and Xbox id come from the links after the root's signature only, and over NetherNet the offer's `a=identity` assertion must be signed by the player's key and cover this connection's DTLS fingerprints, so a captured login cannot be replayed from another machine. Anything else is disconnected with "not authenticated" (the client words it as needing an Xbox login).

Operators can then be listed by Xbox id (`"operators": ["2535412345678"]`); with `xbox_auth` on, a name in the list no longer counts, since names are not unique to an account.

Checked with locally generated keys against a custom root, including a forged self-signed link, tampering, expiry and a replayed login. **Not checked against a real Microsoft-issued chain**: the built-in Mojang root keys are two constants (current and pre-2020); they parse as valid P-384 keys, but I could not confirm the current one against a live login. If real players are refused, set `auth_root_keys` to the key the client's chain is signed with. The newer OpenID `Token` is not verified; the client still sends the certificate chain, which is what is checked.

## Plugins

Same shape as client plugins: a `.wasm` in `plugins/` (optional `name.json` with `name`, `version`, `author`, `description`), the same engine (`WasmRuntime`), one import `perlin.call(op, in, in_len, out, out_cap)` and the exports `perlin_alloc`, `perlin_free`, `perlin_subscriptions`, `perlin_event`. The contract is `sdk/perlin_server.h`: events (enable, disable, tick, join, quit, chat, command, block change, block break, block place) and ops (log, broadcast, message, kick, register command, players, teleport, set/get block, give item, server info).

`examples/hello_server` is a complete plugin in freestanding C: welcome message, a chat filter that cancels a message, and a `/hello` command. `examples/hello_server/build.sh` needs only clang with the wasm32 target and `wasm-ld`.

A plugin that traps is disabled and logged; the server keeps running.

## Singleplayer in the client

The title screen has a **Singleplayer** button. Hovering it shows a note that singleplayer is in very early development and not playable yet. Clicking it starts the server inside the client (`client_bridge/EmbeddedServer` loads `perlin_server` from the folder of the executable, loopback only, you are the operator), then joins it through `BedrockSession` over an in-process transport. The client writes its own `block_states.json` and `block_destroy.tsv` into `<data dir>/singleplayer/` for the server to read.

Build it with `-DPERLIN_BUILD_SERVER=ON` on the root; without it the button explains that this build has no singleplayer. The bridge and the shared library are tested end to end (`server_embed_test`); the button itself and the full client join were **not run**, only compiled.

## Terrain backends and the GPU

World generation goes through `TerrainBackend` (`src/world/Generator.hpp`): a batch of chunk positions in, columns out, same result for the same seed and position. `ChunkManager` runs it on a worker thread and asks for `PreferredBatch()` positions at a time.

`terrain_backend` in `server.json`: `cpu`, `gpu`, or `auto`. The GPU backend (`src/gpu/GpuTerrainRhi.cpp` and `src/gpu/shaders/terrain_flat.slang`) runs a compute pass on the client's RHI: one thread per cell for up to 16 chunks per dispatch, one readback, the block a function of (x, y, z, seed). `auto` takes it only if it starts on a hardware device **and** matches the CPU reference on a probe batch; otherwise it falls back and logs why.

Build it with `-DPERLIN_SERVER_GPU=ON` (needs `slangc`, fetched unless `PERLIN_SLANGC` is set). Only the Vulkan shader is compiled; D3D12 and Metal need their shader targets added to the pack and their bindings checked.

**Verified on a software Vulkan device (SwiftShader), not on a real GPU:** the output equals the CPU's for 108 chunks across several batches and negative coordinates cell by cell, and a full two-player session runs on it. Its speed on real hardware is unmeasured.

**For a flat world the GPU does not help**: the CPU fills a whole uniform section with one store, so dispatch, readback and copy-out only add time (on SwiftShader, 127 ms against 0.5 ms for the same 108 chunks). That is why `auto` keeps flat terrain on the CPU. The backend exists for generators that do real work per block (noise, caves); `terrain_backend: "gpu"` forces it, and `BackendsAgree` is the check every future generator has to pass against its CPU twin.

## Not done / known limits

- No world persistence: chunks are regenerated each run, edits are lost.
- Resource packs are not served; `BiomeDefinitionList` and `AvailableEntityIdentifiers` are sent empty. Everything is tested against the client's own packet codecs and chunk decoder, **not against the running Perlin client or a vanilla Bedrock client in a game**.
- Player position is the eye position the client reports; it is relayed as is.
- Survival block breaking trusts the client's stop-break message (no dig-time check).

## Tests

`ctest --test-dir build-server` runs: units (JSON, block hashes checked against every state in the client's `block_states.json`, chunk encoding decoded with the client's own `ChunkCodec`, batches, fragments, config, the inventory rules, login verification, the plugin host with the real example plugin); a scripted two-player session over loopback (handshake, 49 chunks, chat, commands, plugin, movement, creative menu, place, break, pick, survival rules, leave); the C API; the client's embedding bridge against the real shared library; and a real WebRTC client against the NetherNet listener (probe, SDP offer, data channels, login, chunk delivery, and the `xbox_auth` cases). With `PERLIN_SERVER_GPU=ON` there is also `server_gpu_test`; it needs a Vulkan device (for software Vulkan, `VK_ICD_FILENAMES` can point at a SwiftShader or lavapipe ICD) and skips itself without one. `PS_TEST_BACKEND=gpu` runs the whole session test on the GPU backend.
