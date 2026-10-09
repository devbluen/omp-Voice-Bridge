/*
 *  Voice Bridge for open.mp and SA-MP
 *
 *  Wire protocol shared by the server plugin and the client mod.
 *
 *  The first half of this file is the SampVoice 3.1 protocol (client
 *  version 11).  It must stay byte-for-byte identical: existing
 *  sampvoice.asi clients talk to the server through it.
 *
 *  The second half are Voice Bridge extensions.  They are only sent to
 *  clients that announced themselves with the Voice Bridge hello, and the
 *  extended control ids start at 0x100 so they can never collide with the
 *  legacy enumeration.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace vb
{
// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr uint8_t kRakPacketId = 222; // RakNet packet carrying control messages
constexpr uint8_t kRpcClientJoin = 25; // RPC that carries the connect hello
constexpr uint8_t kRpcServerCommand = 50; // RPC used by the client to send "/commands"

constexpr uint8_t kLegacyVersion = 11; // SampVoice 3.1 client version
constexpr uint32_t kLegacySignature = 0xDEADBEEF;

constexpr uint32_t kVbMagic = 0x31474256; // "VBG1" little endian
constexpr uint8_t kVbProtocol = 1;

constexpr uint32_t kFrequency = 48000;
constexpr uint16_t kNonePlayer = 0xFFFF;
constexpr uint32_t kDefaultBitrate = 24000;
constexpr uint32_t kLegacyFrameMs = 100; // legacy clients only accept 100 ms Opus frames
constexpr std::size_t kMaxVoicePacketSize = 1400;

// RakNet message identifiers used by SA-MP 0.3.7 / 0.3.DL.
constexpr uint8_t kIdDisconnectionNotification = 32;
constexpr uint8_t kIdConnectionLost = 33;
constexpr uint8_t kIdConnectionRequestAccepted = 34;

// ---------------------------------------------------------------------------
// Control packets (RakNet packet 222)
// ---------------------------------------------------------------------------

namespace ctl
{
enum : uint16_t
{
	// SampVoice 3.0
	serverInfo = 0,
	pluginInit,
	muteEnable,
	muteDisable,
	startRecord,
	stopRecord,
	addKey,
	removeKey,
	removeAllKeys,
	createGStream,
	createLPStream,
	createLStreamAtVehicle,
	createLStreamAtPlayer,
	createLStreamAtObject,
	updateLStreamDistance,
	updateLPStreamPosition,
	deleteStream,
	pressKey,
	releaseKey,
	// SampVoice 3.1
	setStreamParameter,
	slideStreamParameter,
	createEffect,
	deleteEffect,

	// Voice Bridge (server -> client)
	vbServerInfo = 0x100,
	vbTransport,
	vbPositions,
	vbPlayerVolume,
	vbSpeakerName,
	vbNotify,
	vbStreamFlags,
	vbVoiceDown,
	vbConfig,
	// Envelope: uint16 sequence + a complete control message.  open.mp can
	// only send custom packets as RELIABLE (unordered), so extended clients
	// reorder enveloped messages by sequence before handling them.
	vbSequenced = 0x10F,

	// Voice Bridge (client -> server)
	vbClientHello = 0x180,
	vbClientStatus,
	vbVoiceUp,
};
}

namespace voice
{
enum : uint32_t
{
	keepAlive = 0,
	voicePacket = 1,
	// Voice Bridge extensions sent over UDP to extended clients only
	vbPositions = 0x20,
	vbKeepAliveAck = 0x21,
};
}

namespace param
{
enum : uint8_t
{
	frequency = 1,
	volume = 2,
	panning = 3,
	eaxmix = 4,
	src = 8,
};
}

namespace effect
{
// Same numbering as BASS_FX_DX8_* so the client can hand them to BASS.
enum : uint8_t
{
	chorus = 0,
	compressor,
	distortion,
	echo,
	flanger,
	gargle,
	i3dl2reverb,
	parameq,
	reverb,
	count
};
}

namespace transport
{
enum : uint8_t
{
	none = 0,
	udp = 1,
	tunnel = 2,
};
}

namespace streamflag
{
enum : uint32_t
{
	// The stream should never be positioned (radio, phone...).
	forceFlat = 1u << 0,
	// The local player is the stream source: play it flat and centered.
	sourceIsListener = 1u << 1,
};
}

#pragma pack(push, 1)

struct Vec3
{
	float x;
	float y;
	float z;
};

struct ControlHeader
{
	uint16_t packet;
	uint16_t length;
};
static_assert(sizeof(ControlHeader) == 4, "ControlHeader must be 4 bytes");

struct VoiceHeader
{
	uint32_t hash;
	uint32_t svrkey;
	uint32_t packet;
	uint32_t stream;
	uint16_t sender;
	uint16_t length;
	uint32_t packid;
};
static_assert(sizeof(VoiceHeader) == 24, "VoiceHeader must be 24 bytes");

// --- SampVoice 3.0 payloads ------------------------------------------------

struct ConnectPacket
{
	uint32_t signature;
	uint8_t version;
	uint8_t micro;
};

struct ServerInfoPacket
{
	uint32_t serverKey;
	uint16_t serverPort;
};

struct PluginInitPacket
{
	uint32_t bitrate;
	uint8_t mute;
};

struct KeyPacket
{
	uint8_t keyId;
};

struct CreateGStreamPacket
{
	uint32_t stream;
	uint32_t color;
	// char name[] follows
};

struct CreateLPStreamPacket
{
	uint32_t stream;
	float distance;
	Vec3 position;
	uint32_t color;
	// char name[] follows
};

struct CreateLStreamAtPacket
{
	uint32_t stream;
	float distance;
	uint32_t target;
	uint32_t color;
	// char name[] follows
};

struct UpdateLStreamDistancePacket
{
	uint32_t stream;
	float distance;
};

struct UpdateLPStreamPositionPacket
{
	uint32_t stream;
	Vec3 position;
};

struct DeleteStreamPacket
{
	uint32_t stream;
};

// --- SampVoice 3.1 payloads ------------------------------------------------

struct SetStreamParameterPacket
{
	uint32_t stream;
	uint32_t parameter;
	float value;
};

struct SlideStreamParameterPacket
{
	uint32_t stream;
	uint32_t parameter;
	float startvalue;
	float endvalue;
	uint32_t time;
};

struct CreateEffectPacket
{
	uint32_t stream;
	uint32_t effect;
	uint32_t number;
	int32_t priority;
	// effect parameters (BASS_DX8_* layout) follow
};

struct DeleteEffectPacket
{
	uint32_t stream;
	uint32_t effect;
};

// Effect parameter layouts.  They match BASS_DX8_* exactly.
struct ChorusParams
{
	float wetdrymix;
	float depth;
	float feedback;
	float frequency;
	uint32_t waveform;
	float delay;
	uint32_t phase;
};

struct CompressorParams
{
	float gain;
	float attack;
	float release;
	float threshold;
	float ratio;
	float predelay;
};

struct DistortionParams
{
	float gain;
	float edge;
	float posteqcenterfrequency;
	float posteqbandwidth;
	float prelowpasscutoff;
};

struct EchoParams
{
	float wetdrymix;
	float feedback;
	float leftdelay;
	float rightdelay;
	uint32_t pandelay;
};

using FlangerParams = ChorusParams;

struct GargleParams
{
	uint32_t ratehz;
	uint32_t waveshape;
};

struct I3dl2reverbParams
{
	int32_t room;
	int32_t roomhf;
	float roomrollofffactor;
	float decaytime;
	float decayhfratio;
	int32_t reflections;
	float reflectionsdelay;
	int32_t reverb;
	float reverbdelay;
	float diffusion;
	float density;
	float hfreference;
};

struct ParameqParams
{
	float center;
	float bandwidth;
	float gain;
};

struct ReverbParams
{
	float ingain;
	float reverbmix;
	float reverbtime;
	float highfreqrtratio;
};

// --- Voice Bridge payloads -------------------------------------------------

// Appended to the RPC 25 parameters after ConnectPacket, and also sent as the
// vbClientHello control packet once the connection is up.
struct VbHello
{
	uint32_t magic;
	uint8_t protocol;
	uint8_t flags; // VbHelloFlags
	uint16_t build;
	uint32_t caps;
};

namespace helloflag
{
enum : uint8_t
{
	hasMicro = 1u << 0,
};
}

struct VbServerInfo
{
	uint32_t key;
	uint16_t port;
	uint8_t protocol;
	uint8_t flags; // serverflag
	uint16_t frameMs;
	uint16_t keepAliveMs;
	uint32_t bitrate;
	uint8_t mute;
	// char host[] follows (optional, empty = use the game server address)
};

namespace serverflag
{
enum : uint8_t
{
	tunnelAllowed = 1u << 0,
	forceTunnel = 1u << 1,
};
}

struct VbTransport
{
	uint8_t mode; // transport::*
};

struct VbClientStatus
{
	uint8_t transport;
	uint8_t micAvailable;
	uint8_t micMuted;
	uint8_t soundMuted;
};

struct VbPosition
{
	uint32_t stream;
	Vec3 position;
};

struct VbPositions
{
	uint16_t count;
	// VbPosition items[count] follow
};

struct VbPlayerVolume
{
	uint16_t player;
	float volume;
};

struct VbSpeakerName
{
	uint16_t player;
	// char name[] follows
};

struct VbNotify
{
	uint32_t color;
	uint16_t durationMs;
	// char text[] follows
};

struct VbStreamFlags
{
	uint32_t stream;
	uint32_t flags;
};

struct VbConfig
{
	uint8_t allowVoiceActivation;
	uint8_t showSpeakerList;
	uint8_t showMicIcon;
	uint8_t hideHeadIcons; // 1 = no microphone icons above heads (0 from older servers: allowed)
};

#pragma pack(pop)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

inline uint32_t crc32c(const uint8_t* buffer, std::size_t length, uint32_t crc = 0) noexcept
{
	crc = ~crc;
	while (length--)
	{
		crc ^= *buffer++;
		for (int k = 0; k < 8; ++k)
		{
			crc = (crc & 1) ? (crc >> 1) ^ 0x82F63B78u : crc >> 1;
		}
	}
	return ~crc;
}

// The hash only covers the header, exactly like SampVoice.
inline uint32_t voiceHeaderHash(const VoiceHeader& header) noexcept
{
	return crc32c(reinterpret_cast<const uint8_t*>(&header) + sizeof(header.hash), sizeof(header) - sizeof(header.hash));
}

inline void sealVoiceHeader(VoiceHeader& header) noexcept
{
	header.hash = voiceHeaderHash(header);
}

inline bool checkVoiceHeader(const VoiceHeader& header) noexcept
{
	return header.hash == voiceHeaderHash(header);
}

// Searches `data` for a little endian 32 bit value.
inline const uint8_t* findSignature(const uint8_t* data, std::size_t size, uint32_t value) noexcept
{
	if (!data || size < sizeof(value))
	{
		return nullptr;
	}
	for (std::size_t i = 0; i + sizeof(value) <= size; ++i)
	{
		uint32_t current;
		std::memcpy(&current, data + i, sizeof(current));
		if (current == value)
		{
			return data + i;
		}
	}
	return nullptr;
}
}
