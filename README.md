# Voice Bridge

Voice chat for **open.mp** and **SA-MP** servers, with its own client mod and **SampVoice compatibility**.

Created by **[devbluen](https://github.com/devbluen)** · [Repository](https://github.com/devbluen/omp-Voice-Bridge) · [Releases](https://github.com/devbluen/omp-Voice-Bridge/releases) · [Português](README.pt-BR.md)

| | |
|---|---|
| **Server** | One file works as an open.mp component and as a SA-MP plugin (0.3.7-R2 and 0.3.DL), on Windows and Linux. |
| **Client** | One `voice-bridge.asi` for SA-MP 0.3.7 R1, R2, R3, R4, R5 and 0.3.DL. No installer. |
| **SampVoice** | `sampvoice.asi` players work on Voice Bridge servers, `voice-bridge.asi` works on SampVoice servers (3.x and the open.mp port), and gamemodes using the `Sv*` natives run unchanged. |
| **Connection** | Fixed, configurable voice port. When UDP is blocked, voice goes through the game connection (tunnel). |
| **Audio** | 3D sound relative to the character (direction, distance and room echo), effects (radio, phone, megaphone...) and per-player volume. |
| **Security** | Per-player key bound to the game IP, automatic blocking of IPs that send invalid packets, and no IP/port shown on the client. |

## Contents

1. [Installation](#installation)
2. [Configuration](#configuration)
3. [Getting started in Pawn](#getting-started-in-pawn)
4. [Examples](#examples)
5. [Controlling the players' client](#controlling-the-players-client)
6. [API reference](#api-reference)
7. [Client menu and settings](#client-menu-and-settings)
8. [Security](#security)
9. [Troubleshooting](#troubleshooting)
10. [Migrating from SampVoice](#migrating-from-sampvoice)
11. [Building](#building)
12. [How it works](#how-it-works)

## Installation

### Players

1. Copy `voice-bridge.asi` into the GTA San Andreas folder (next to `gta_sa.exe`).
2. Delete `sampvoice.asi` if you have it. Voice Bridge also works on SampVoice servers.
3. In game, **F11** opens the voice menu.

An ASI loader is required (most SA-MP/CLEO installs have one). If the mod does not load, install *Silent's ASI Loader*.

### Server

| | open.mp | SA-MP 0.3.7-R2 / 0.3.DL |
|---|---|---|
| Plugin | `voice-bridge.dll`/`.so` in `components/` | `voice-bridge.dll`/`.so` in `plugins/`, and `voice-bridge` (or `voice-bridge.so`) on the `plugins` line of `server.cfg` |
| Includes | `include/*.inc` in `qawno/include/` | `include/*.inc` in `pawno/include/` |
| Firewall | Open the **voice UDP port** (game port + 1) | same |

The server prints the port at startup:

```
[VoiceBridge] voice server listening on UDP 0.0.0.0:7778. Open this UDP port in the firewall/hosting panel.
```

> If the file is in both `components/` **and** `plugins/` on open.mp, only the component is used.

## Configuration

Everything is optional. Each setting can come from an environment variable, `config.json` (open.mp, inside `"voice_bridge"`) or `server.cfg` (`voice_` prefix), in that order.

```json
"voice_bridge": { "port": 7778, "bitrate": 24000 }
```

```
voice_port 7778
voice_bitrate 24000
```

**Network**

| config.json | server.cfg | Default | Description |
|---|---|---|---|
| `port` | `voice_port` | 0 | Voice UDP port. `0` = game port + 1. |
| `bind` | `voice_bind` | - | Local IP of the voice port (default: the server `bind`). |
| `public_host` | `voice_public_host` | - | Host announced to clients (server behind a proxy/anti-DDoS). |
| `tunnel` | `voice_tunnel` | true | Use the game connection when UDP is blocked. |
| `force_tunnel` | `voice_force_tunnel` | false | Always use the tunnel (hosts that forbid UDP ports). |
| `keepalive_ms` | `voice_keepalive_ms` | 5000 | Keep-alive (keeps NAT open). |

**Audio**

| config.json | server.cfg | Default | Description |
|---|---|---|---|
| `bitrate` | `voice_bitrate` | 24000 | Opus bitrate (6000-128000). |
| `frame_ms` | `voice_frame_ms` | 100 | Frame size for Voice Bridge clients: 20, 40, 60 or 100. The SampVoice client only plays 100. |
| `stream_tick_ms` | `voice_stream_tick_ms` | 100 | Dynamic stream update interval. |
| `position_rate_ms` | `voice_position_rate_ms` | 100 | Position updates sent to Voice Bridge clients. |

**Clients**

| config.json | server.cfg | Default | Description |
|---|---|---|---|
| `allow_sampvoice` | `voice_allow_sampvoice` | true | Accept the SampVoice client. |
| `allow_voicebridge` | `voice_allow_voicebridge` | true | Accept the Voice Bridge client. |
| `allow_voice_activation` | `voice_allow_voice_activation` | false | Allow voice activation (talking without holding the key). |
| `show_speaker_list` | `voice_show_speaker_list` | true | Show who is talking. |
| `show_mic_icon` | `voice_show_mic_icon` | true | Show the microphone icon. |

**Security and diagnostics**

| config.json | server.cfg | Default | Description |
|---|---|---|---|
| `strict_ip` | `voice_strict_ip` | false | Only accept voice from the game IP. When off, another IP (proxy/anti-DDoS) is accepted and logged. |
| `max_packets_per_second` | `voice_max_packets_per_second` | 80 | Per-player flood limit. |
| `debug` | `voice_debug` | false | Log the details (who hears whom, vehicles, dropped voice) on the console and in the voice log. |
| `log_file` | `voice_log_file` | - | Voice-only log with date and time (details only with `debug` on). Default: `logs/voice-bridge.log` (open.mp) or `voice-bridge.log`. `off` disables it. |

Environment variables: `VOICE_BRIDGE_` + the name in upper case (e.g. `VOICE_BRIDGE_PORT`, `VOICE_BRIDGE_MAX_PACKETS`).

> Several servers on one machine? Set `voice_port` on each: one server's default port (game + 1) may be another's game port.

## Getting started in Pawn

A **stream** is a voice room: **speakers** talk in it and **listeners** hear it. Nobody hears themselves.

| Type | Use |
|---|---|
| Global | Everyone hears it at the same volume (radio, phone). |
| Static | Placed at a point/player/vehicle/object; you choose the listeners. |
| Dynamic | Placed; nearby players join and leave automatically. |

```pawn
#include <open.mp>      // or <a_samp>
#include <voice-bridge>

new gLocal[MAX_PLAYERS] = { VB_INVALID_STREAM, ... };

public VB_OnPlayerClientDetected(playerid, version, bool:extended, bool:hasmicro)
{
    gLocal[playerid] = VB_CreateDynamicStreamAtPlayer(35.0, VB_INFINITE, playerid, 0xFF66FF66, "Local");
    VB_AddKey(playerid, VB_KEY_B);
    return 1;
}

public OnPlayerDisconnect(playerid, reason)
{
    VB_DeleteStream(gLocal[playerid]);
    gLocal[playerid] = VB_INVALID_STREAM;
    return 1;
}

public OnPlayerActivationKeyPress(playerid, keyid)
{
    if (keyid == VB_KEY_B) VB_AddSpeaker(gLocal[playerid], playerid);
    return 1;
}

public OnPlayerActivationKeyRelease(playerid, keyid)
{
    if (keyid == VB_KEY_B) VB_RemoveSpeaker(gLocal[playerid], playerid);
    return 1;
}
```

> Include `voice-bridge.inc` **after** `<open.mp>`/`<a_samp>`.

## Examples

In [`examples/en`](examples/en) (Portuguese versions in [`examples/pt-BR`](examples/pt-BR)):

| File | Shows |
|---|---|
| `01-proximity-chat.pwn` | 3D local chat (B) and global chat (Z). |
| `02-radio-frequencies.pwn` | `/freq` with a radio effect. |
| `03-phone-calls.pwn` | `/call` and `/hangup` with a phone effect and open microphone. |
| `04-vehicles-and-megaphone.pwn` | Vehicle intercom and `/megaphone`. |
| `05-admin-tools.pwn` | `/vinfo`, `/vmute`, `/vblock`. |
| `06-sampvoice-compatible.pwn` | The official SampVoice gamemode, unchanged. |
| `07-client-control.pwn` | Accept only Voice Bridge, require a minimum version and hide who is talking. |
| `08-custom-effects.pwn` | Effects without presets: echo, hall, old radio, robot and monster, combining several effects. |

## Controlling the players' client

**Which client a player uses**

```pawn
switch (VB_GetClientType(playerid))
{
    case VB_CLIENT_VOICEBRIDGE: // voice-bridge.asi
    case VB_CLIENT_SAMPVOICE:   // sampvoice.asi
    case VB_CLIENT_NONE:        // no voice client
}
```

**Client version**: block a specific version or require a minimum one

```pawn
new version[16];
VB_GetClientVersionString(playerid, version); // "1.0.0" (Voice Bridge) or "3.1" (SampVoice)

if (VB_GetClientBuild(playerid) < VB_VERSION(1, 2, 0))
{
    SendClientMessage(playerid, -1, "Please update Voice Bridge to 1.2.0.");
    // Kick through SetTimerEx so the message arrives first
}

new major, minor, patch;
if (VB_GetClientVersionNumbers(playerid, major, minor, patch) == VB_CLIENT_VOICEBRIDGE && major == 1 && minor == 1)
{
    // block 1.1.x only
}
```

The version is already known in `VB_OnPlayerClientDetected` (usually before `OnPlayerConnect`), even for clients that are not allowed. The player's SA-MP version (R1, R3...) comes from the standard `GetPlayerVersion` native.

**Which clients may use voice**

```pawn
VB_AllowClientType(VB_CLIENT_SAMPVOICE, false); // same as voice_allow_sampvoice false
```

**What the player sees**

```pawn
VB_SetPlayerSpeakerList(playerid, false);     // hide who is talking (names are not even sent)
VB_SetPlayerMicIcon(playerid, false);         // hide the microphone icon
VB_SetPlayerVoiceActivation(playerid, true);  // allow voice activation for this player only
```

## API reference

Stream and effect handles are integers; `0` is invalid. SampVoice and Voice Bridge handles are interchangeable. Full list in [`include/voice-bridge.inc`](include/voice-bridge.inc).

### Server

| Native | Description |
|---|---|
| `VB_GetPluginVersion(version[], size)` | Plugin version. |
| `VB_GetVoicePort()` | Voice UDP port. |
| `VB_SetDebug(bool:enabled)` | Verbose log. |
| `VB_SetBitrate(bitrate)` / `VB_GetBitrate()` | Bitrate for players who connect afterwards. |
| `VB_AllowClientType(type, bool:allowed)` / `VB_IsClientTypeAllowed(type)` | Which clients may use voice. |

### Players: client

| Native | Description |
|---|---|
| `VB_HasPlugin(playerid)` | Has an active voice client. |
| `VB_GetClientType(playerid)` | `VB_CLIENT_NONE`, `VB_CLIENT_SAMPVOICE` or `VB_CLIENT_VOICEBRIDGE`. |
| `VB_IsExtendedClient(playerid)` | Uses the Voice Bridge client (extra features). |
| `VB_GetClientVersionString(playerid, version[], size)` | Version as text (`"1.0.0"`, `"3.1"`); returns the client type. |
| `VB_GetClientVersionNumbers(playerid, &major, &minor, &patch)` | Version as numbers; returns the client type. |
| `VB_GetClientBuild(playerid)` | Voice Bridge version as a number (`VB_VERSION(1, 2, 0)` = 10200); 0 otherwise. |
| `VB_GetClientVersion(playerid)` | SampVoice protocol (11). |
| `VB_GetTransport(playerid)` | `VB_TRANSPORT_NONE`, `_UDP` or `_TUNNEL`. |
| `VB_HasMicrophone(playerid)` | Has a microphone. |
| `VB_IsClientMicMuted(playerid)` / `VB_IsClientSoundMuted(playerid)` | Turned the microphone / sound off in the menu. |
| `VB_IsTalking(playerid)` | Talking right now. |

### Players: talking

| Native | Description |
|---|---|
| `VB_AddKey` / `VB_RemoveKey` / `VB_HasKey` / `VB_RemoveAllKeys` | Push-to-talk keys (VK codes, e.g. `VB_KEY_B`). |
| `VB_StartRecord` / `VB_StopRecord` / `VB_IsRecording` | Open microphone, no key. |
| `VB_MutePlayer(playerid, bool:mute)` / `VB_IsPlayerMuted` | Block the microphone. |

### Players: hearing and screen

| Native | Description |
|---|---|
| `VB_BlockSpeaker(listenerid, speakerid, bool:block)` / `VB_IsSpeakerBlocked` | `listenerid` stops hearing `speakerid`. |
| `VB_SetSpeakerVolume(listenerid, speakerid, Float:volume)` | Volume of one player for another only. |
| `VB_SetPlayerSpeakerList` / `VB_SetPlayerMicIcon` / `VB_SetPlayerVoiceActivation` | Per-player client options. |
| `VB_Notify(playerid, text[], color, duration)` | On-screen notification. |

### Streams

| Native | Description |
|---|---|
| `VB_CreateGlobalStream(color, name[])` | Global. |
| `VB_CreateStaticStreamAt{Point,Player,Vehicle,Object}(Float:distance, ..., color, name[])` | Static. |
| `VB_CreateDynamicStreamAt{Point,Player,Vehicle,Object}(Float:distance, maxlisteners, ..., color, name[])` | Dynamic (`VB_INFINITE` = no limit). |
| `VB_DeleteStream` / `VB_IsValidStream` / `VB_GetStreamType` | Management. |
| `VB_SetStreamDistance` / `VB_GetStreamDistance` | Range. |
| `VB_SetStreamPosition` / `VB_GetStreamPosition` | Position. |
| `VB_SetStreamMaxListeners` | Listener limit (dynamic). |
| `VB_SetStreamWorld(stream, worldid, interiorid)` | World/interior filter (dynamic). |
| `VB_SetStreamFlat(stream, bool:flat)` | No 3D. |
| `VB_SetStreamVolume(stream, Float:volume)` | Volume. |
| `VB_AddListener` / `VB_RemoveListener` / `VB_HasListener` / `VB_RemoveAllListeners` / `VB_GetListenerCount` / `VB_GetListeners` | Listeners. |
| `VB_AddSpeaker` / `VB_RemoveSpeaker` / `VB_HasSpeaker` / `VB_RemoveAllSpeakers` / `VB_GetSpeakerCount` / `VB_GetSpeakers` | Speakers. |
| `VB_SetStreamParameter` / `VB_GetStreamParameter` / `VB_HasStreamParameter` / `VB_ResetStreamParameter` | `VB_PARAM_VOLUME`, `_PANNING`, `_FREQUENCY`, `_EAXMIX`, `_SRC`. |
| `VB_SlideStreamParameter` / `VB_SlideStreamParameterTo` | Smooth change over `time` ms. |

### Effects

An effect is created once and attached to one or more streams. Everything heard in the stream goes through it.

```pawn
new echo = VB_CreateEchoEffect(0, 30.0, 40.0, 300.0, 300.0, false); // create
VB_AttachEffect(echo, stream);                                      // apply to the stream
VB_DetachEffect(echo, stream);                                      // remove from the stream
VB_DeleteEffect(echo);                                              // delete it
```

- **Presets**: `VB_CreatePresetEffect(VB_PRESET_RADIO)`, also `_PHONE`, `_MEGAPHONE`, `_HALL`, `_CAVE`, `_UNDERWATER`, `_ROBOT`, `_ECHO`, `_WALKIE_TALKIE`.
- **Combining**: attach several effects to the same stream. The first parameter (`priority`) sets the order: **higher priority is applied first**.
- To change one player's voice only, use a stream where only that player talks (e.g. their local stream).
- Full example: [`08-custom-effects.pwn`](examples/en/08-custom-effects.pwn) (`/voice echo`, `radio`, `robot`, `monster`...).

Parameters (DirectX 8 effects, same as SampVoice):

| Native | Parameters (range) |
|---|---|
| `VB_CreateEchoEffect(priority, wetdrymix, feedback, leftdelay, rightdelay, pandelay)` | mix 0-100 %, feedback 0-100 %, left/right delay 1-2000 ms, `pandelay` swaps sides |
| `VB_CreateReverbEffect(priority, ingain, reverbmix, reverbtime, highfreqrtratio)` | gain -96-0 dB, mix -96-0 dB, time 0.001-3000 ms, high frequency ratio 0.001-0.999 |
| `VB_CreateI3dl2ReverbEffect(priority, room, roomhf, roomrollofffactor, decaytime, decayhfratio, reflections, reflectionsdelay, reverb, reverbdelay, diffusion, density, hfreference)` | room/roomhf -10000-0 mB, rolloff 0-10, decay 0.1-20 s, decay HF 0.1-2, reflections -10000-1000 mB, delay 0-0.3 s, reverb -10000-2000 mB, delay 0-0.1 s, diffusion/density 0-100 %, reference 20-20000 Hz |
| `VB_CreateChorusEffect(priority, wetdrymix, depth, feedback, frequency, waveform, delay, phase)` | mix 0-100 %, depth 0-100 %, feedback -99-99 %, frequency 0-10 Hz, wave 0 triangle / 1 sine, delay 0-20 ms, phase 0-4 (-180°, -90°, 0°, 90°, 180°) |
| `VB_CreateFlangerEffect(priority, wetdrymix, depth, feedback, frequency, waveform, delay, phase)` | same as chorus, delay 0-4 ms |
| `VB_CreateDistortionEffect(priority, gain, edge, posteqcenterfrequency, posteqbandwidth, prelowpasscutoff)` | gain -60-0 dB, edge 0-100 %, EQ center/width 100-8000 Hz, lowpass cutoff 100-8000 Hz |
| `VB_CreateCompressorEffect(priority, gain, attack, release, threshold, ratio, predelay)` | gain -60-60 dB, attack 0.01-500 ms, release 50-3000 ms, threshold -60-0 dB, ratio 1-100, predelay 0-4 ms |
| `VB_CreateParamEqEffect(priority, center, bandwidth, gain)` | frequency 80-16000 Hz, width 1-36 semitones, gain -15-15 dB |
| `VB_CreateGargleEffect(priority, ratehz, waveshape)` | rate 1-1000 Hz, wave 0 triangle / 1 square |

Also: `VB_IsValidEffect(effect)`. Effects and streams are freed automatically when the script that created them unloads.

### Callbacks

| Callback | When |
|---|---|
| `OnPlayerActivationKeyPress(playerid, keyid)` / `OnPlayerActivationKeyRelease` | Push-to-talk key pressed/released. |
| `VB_OnPlayerClientDetected(playerid, version, bool:extended, bool:hasmicro)` | Voice client detected. |
| `VB_OnPlayerTransportChange(playerid, transport)` | Voice connected, lost or moved to the tunnel. |
| `VB_OnPlayerStartTalking(playerid)` / `VB_OnPlayerStopTalking(playerid)` | Started/stopped talking. |
| `VB_OnPlayerClientStatus(playerid, bool:micavailable, bool:micmuted, bool:soundmuted)` | Changed voice options in the menu. |
| `VB_OnPlayerVoiceIgnored(playerid, reason)` | The player talks but the voice is dropped (at most once every 10 s). `reason`: `VB_VOICE_IGNORED_NOT_SPEAKER` (not a speaker of any stream, missing `VB_AddSpeaker`), `_NO_KEY` (no talk key), `_MUTED`, `_CLIENT_NOT_ALLOWED`. |

## Client menu and settings

**F11** opens the menu (the key can be changed in it):

| Page | Contents |
|---|---|
| Status | Connected or not, ping, SA-MP version, microphone and push-to-talk keys. |
| Sound | Volume, 3D sound (realistic, simple stereo or off), room echo, distance fade, direction from the character or the camera, swap channels and push-to-talk beep. |
| Microphone | Device, gain, noise gate, level meter, loopback test and voice activation (when the server allows it). |
| Players | Per-player volume and mute. |
| Interface | Language (automatic, English or Portuguese), size, microphone icon position (or drag the icon while the menu is open) and menu key. |
| About | Version, project link and credits. |

Settings are stored in `voicebridge.ini` in the GTA folder. `voicebridge.log` records the detected samp.dll build and audio errors, never IPs or ports.

## Security

- Every player gets a random key; packets without a valid key or with a bad CRC are dropped.
- After the first valid packet the session is bound to that IP (the game connection's IP always wins): a guessed key is useless from another machine.
- An IP sending 30 invalid packets in 10 s (port scans or key guessing) is blocked for 5 minutes and logged.
- Each player has a packets-per-second limit.
- The client never shows IPs, ports or the connection type; those stay in the server log.

## Troubleshooting

| Symptom | Check |
|---|---|
| SampVoice players can't talk or hear, warning "none of its UDP packets reached port" with an odd port (e.g. 51665) | The voice port was busy when the server started and the plugin took a random one, which is not open. The start of `voice-bridge.log` says which port was busy. Free it (another server/program) or set `voice_port` to a free, open port. |
| Nobody connects to voice | Is the UDP port from the log open in the firewall/panel? With the tunnel on (default), Voice Bridge clients work anyway. |
| Stuck on "Connecting..." | The server log reports players whose UDP never arrives. |
| Several servers on one machine | Give each one a different `voice_port`. |
| The mod does not load | Missing ASI loader, or `sampvoice.asi` is still there. See `voicebridge.log`. |
| SampVoice player is heard but hears nobody | They joined by host name (`localhost` or a domain). The original SampVoice client only receives voice when the server is joined by IP (e.g. `127.0.0.1:7777`). The Voice Bridge client works with names. |
| Can't hear anyone | Menu volume (F11 > Sound) and whether the player is in a stream you listen to. |

Diagnostics in Pawn: `VB_GetTransport`, `VB_OnPlayerTransportChange`, `VB_HasMicrophone` and `VB_IsClientSoundMuted`.

What Voice Bridge fixes compared to SampVoice:

- A fixed port instead of a random one.
- Reception no longer stops after a Windows ICMP error.
- Works when connecting through a domain name.
- Works behind a proxy/anti-DDoS.
- Follows NAT port changes.
- Replies from the right IP on multi-IP machines.
- Uses the tunnel when UDP is blocked.
- Orders control messages on open.mp.
- Frees streams and effects after GMX/unloadfs.

## Migrating from SampVoice

1. Replace the `sampvoice` plugin with `voice-bridge` (on open.mp, in `components/`).
2. Use this project's `sampvoice.inc` or keep your current `.amx`. Nothing else changes.
3. Players with `sampvoice.asi` keep working; players with `voice-bridge.asi` get the tunnel, 3D sound, the menu and the new features.

Behaviour differences (all fixes): dynamic streams follow the entity's virtual world; streams and effects are deleted when the script that created them unloads; `maxplayers` 0 means unlimited.

## Building

Requirements: CMake 3.19+ and Visual Studio 2022 with C++ (Windows), or GCC with multilib and Ninja (Linux).

**Windows**: [`build.ps1`](build.ps1) builds, runs the tests and collects everything in `dist/`.

```powershell
.\build.ps1                         # server + client
.\build.ps1 client                  # only voice-bridge.asi
.\build.ps1 server                  # only the plugin (with tests)
.\build.ps1 -Clean                  # from scratch
.\build.ps1 -Package -Version 1.2.0 # release .zip files
.\build.ps1 client -GameDir "C:\Games\GTA San Andreas"   # also copy into the game
.\build.ps1 server -ServerDir "D:\Samp\my-server"        # also copy into the server
```

**Version**: stored in the [`VERSION`](VERSION) file. `-Version 1.2.0` writes the new version there and in `voice-bridge.inc`, and it goes into the plugin, the client (what `VB_GetClientBuild`/`VB_GetClientVersionString` return), the log, the `.dll`/`.asi` file properties and the `.zip` names. Without `-Version`, the one in the file is used. Limits: MAJOR 0-6, MINOR 0-99, PATCH 0-99.

If PowerShell blocks the script: `powershell -ExecutionPolicy Bypass -File .\build.ps1`.

Output:

```
dist/
  server/components/voice-bridge.dll   open.mp
  server/plugins/voice-bridge.dll      SA-MP
  server/include/*.inc
  client/voice-bridge.asi
```

**Linux** (server): `./build.sh` (options `--clean`, `--no-tests`, `--version 1.2.0`). Produces `dist/server/`.

**Manual** with CMake presets: `cmake --preset windows-server`, then `cmake --build --preset windows-server` (also `windows-client` and `linux-server`).

## How it works

- The SampVoice handshake (`0xDEADBEEF` appended to RPC 25) is kept; the Voice Bridge client also appends its own hello with its version.
- Control goes through RakNet packet 222 (SampVoice format + extensions from `0x100`). Voice goes over UDP with SampVoice's 24-byte header (CRC32C) and 48 kHz Opus.
- On SA-MP the plugin hooks `RakServer` by byte pattern (0.3.7/0.3.DL) and reads positions by calling the server's own natives. On open.mp it uses the SDK.
- The client plays through SA-MP's `bass.dll` and renders 3D sound relative to the character: timing and tone differences between the ears, high frequencies lost with distance, and room echo.

## Screenshots Client
<img width="1097" height="628" alt="Captura de tela 2026-10-07 031248" src="https://github.com/user-attachments/assets/4f9a444b-c85d-497b-a7bb-1f04bb038e54" />
<img width="1045" height="659" alt="Captura de tela 2026-10-07 031309" src="https://github.com/user-attachments/assets/ccd3cb0f-be8e-4a93-a6cc-2181c782b81f" />
<img width="1155" height="657" alt="Captura de tela 2026-10-07 031224" src="https://github.com/user-attachments/assets/bdfacfbf-5b63-4946-9f88-fb7d34d0e1ba" />
<img width="274" height="240" alt="Captura de tela 2026-10-07 031408" src="https://github.com/user-attachments/assets/f9272d9a-387b-4a6a-a1b2-02d94be5102a" />

## Credits

- **Voice Bridge**: created by [devbluen](https://github.com/devbluen). Code, issues and releases at [github.com/devbluen/omp-Voice-Bridge](https://github.com/devbluen/omp-Voice-Bridge).
- **MMV (Ramon)**: testing and ideas.
- **Claude (Anthropic)**: development assistance (server, client, protocol, tests and documentation).
- **SampVoice**: original protocol and API by MOR (CyberMor), followed for compatibility; open.mp port by AmyrAhmady (iAmir).
- Libraries: [Opus](https://opus-codec.org), [Dear ImGui](https://github.com/ocornut/imgui), BASS (shipped with SA-MP) and the open.mp SDK.

License: MIT.
