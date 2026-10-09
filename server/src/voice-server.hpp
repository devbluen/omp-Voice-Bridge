/*
 *  Voice Bridge for open.mp and SA-MP
 *
 *  Loader independent voice server.  The open.mp component and the SA-MP
 *  plugin only translate their events into calls on this class and provide
 *  the three host interfaces below.
 *
 *  Threading: every public method runs on the server thread except the UDP
 *  worker, which only relays voice.  Routing data is guarded by routeMutex_;
 *  the server thread is its only writer.
 */

#pragma once

#include "config.hpp"
#include "udp-socket.hpp"
#include <vb-protocol.hpp>
#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vbs
{
constexpr uint16_t kMaxPlayers = 1000;
constexpr uint32_t kInfiniteListeners = 0xFFFFFFFFu;
constexpr int kAnyWorld = -1;

struct Pose
{
	vb::Vec3 position {};
	int world = 0;
	int interior = 0;
};

class ITransport
{
public:
	virtual ~ITransport() = default;
	// `data` starts with the RakNet packet id.  Reliable messages must keep
	// their order when orderedDelivery() is true.
	virtual bool sendPacket(uint16_t player, const uint8_t* data, std::size_t size, bool reliable) = 0;
	// IPv4 address of the game connection, network byte order (0 if unknown).
	virtual uint32_t playerIp(uint16_t player) = 0;
	// SA-MP sends RELIABLE_ORDERED; open.mp custom packets are only RELIABLE.
	virtual bool orderedDelivery() const = 0;
};

class IWorld
{
public:
	virtual ~IWorld() = default;
	virtual bool playerPose(uint16_t player, Pose& out) = 0;
	virtual bool vehiclePose(uint16_t vehicle, Pose& out) = 0;
	virtual bool objectPose(uint16_t object, Pose& out) = 0;
	virtual bool playerName(uint16_t player, std::string& out) = 0;
};

class IScriptEvents
{
public:
	virtual ~IScriptEvents() = default;
	virtual void onActivationKey(uint16_t player, uint8_t key, bool pressed) = 0;
	virtual void onHandshake(uint16_t player, uint8_t version, bool extended, bool micro) = 0;
	virtual void onTransport(uint16_t player, uint8_t transport) = 0;
	virtual void onTalking(uint16_t player, bool talking) = 0;
	virtual void onClientStatus(uint16_t player, bool micAvailable, bool micMuted, bool soundMuted) = 0;
};

enum class ClientType : uint8_t
{
	None = 0,
	SampVoice = 1,
	VoiceBridge = 2,
};

enum class StreamType : uint8_t
{
	Global = 0,
	StaticPoint,
	StaticPlayer,
	StaticVehicle,
	StaticObject,
	DynamicPoint,
	DynamicPlayer,
	DynamicVehicle,
	DynamicObject,
};

enum class TargetKind : uint8_t
{
	None,
	Point,
	Player,
	Vehicle,
	Object,
};

TargetKind StreamTarget(StreamType type);
bool StreamIsDynamic(StreamType type);

struct ParameterState
{
	float value = 0.f;
	bool sliding = false;
	float from = 0.f;
	float to = 0.f;
	uint64_t startMs = 0;
	uint32_t durationMs = 0;
};

struct Stream
{
	uint32_t id = 0;
	StreamType type = StreamType::Global;
	uint32_t color = 0;
	std::string name;
	float distance = 0.f;
	vb::Vec3 position {};
	uint16_t target = 0xFFFF;
	uint32_t maxListeners = kInfiniteListeners;
	int world = kAnyWorld; // point streams; entity streams follow their source unless overridden
	int interior = kAnyWorld;
	bool worldOverride = false;
	uint32_t flags = 0;
	const void* owner = nullptr; // AMX that created the stream

	std::vector<uint16_t> listeners;
	std::bitset<kMaxPlayers> listenerMask;
	std::vector<uint16_t> speakers;
	std::bitset<kMaxPlayers> speakerMask;
	std::map<uint8_t, ParameterState> parameters;
	std::vector<uint32_t> effects;
};

struct EffectItem
{
	uint32_t wireId = 0;
	uint32_t number = 0;
	int32_t priority = 0;
	std::vector<uint8_t> params;
};

struct Effect
{
	uint32_t id = 0;
	const void* owner = nullptr;
	std::vector<EffectItem> items;
	std::vector<uint32_t> streams;
};

struct Player
{
	ClientType clientType = ClientType::None; // detected, even when not allowed
	bool showSpeakerList = true;
	bool showMicIcon = true;
	bool allowVoiceActivation = false;
	bool connected = false;
	bool plugin = false;
	bool extended = false;
	bool probe = false; // detected through a blind serverInfo (no handshake seen)
	uint8_t version = 0;
	uint8_t protocol = 0;
	uint16_t build = 0;
	bool micro = false;
	uint32_t caps = 0;
	uint32_t key = 0;
	uint32_t gameIp = 0;
	uint8_t transport = vb::transport::none;
	uint8_t reportedTransport = vb::transport::none;
	bool identified = false;
	bool udpWarned = false;
	bool ipWarned = false;
	uint64_t connectedAt = 0;
	uint64_t handshakeAt = 0;
	bool probeSent = false;
	uint16_t sequence = 0;

	bool muted = false;
	bool recording = false;
	std::set<uint8_t> keys;
	std::vector<uint32_t> speakerStreams;
	std::vector<uint32_t> listenerStreams;
	// SampVoice listeners hear streams that follow a player as point streams
	// moved by the server (last position sent, per stream).
	std::map<uint32_t, vb::Vec3> legacyPoints;
	std::bitset<kMaxPlayers> blocked;

	bool talking = false;
	bool clientMicAvailable = false;
	bool clientMicMuted = false;
	bool clientSoundMuted = false;
	std::string sentName;
};

class VoiceServer
{
public:
	static VoiceServer& Get();

	bool start(const Config& config, ITransport* transport, IWorld* world, IScriptEvents* events);
	void stop();
	bool running() const noexcept { return running_; }
	const Config& config() const noexcept { return config_; }
	IWorld* world() const noexcept { return world_; }
	uint16_t port() const noexcept { return socket_.localPort(); }
	// Without the RPC 25 hook (some SA-MP builds) players are probed with a
	// blind serverInfo so SampVoice clients still connect.
	void setProbeMode(bool enabled) noexcept { probeMode_ = enabled; }

	void tick();

	// Loader events (server thread)
	void onPlayerConnect(uint16_t player);
	void onPlayerDisconnect(uint16_t player);
	void onClientJoin(uint16_t player, const uint8_t* data, std::size_t size);
	// `data` excludes the RakNet packet id.
	void onControlPacket(uint16_t player, const uint8_t* data, std::size_t size);
	bool isTracked(uint16_t player) const;

	// Players
	bool hasPlugin(uint16_t player) const;
	ClientType clientType(uint16_t player) const;
	void setClientTypeAllowed(ClientType type, bool allowed);
	bool isClientTypeAllowed(ClientType type) const;
	bool setPlayerSpeakerList(uint16_t player, bool visible);
	bool setPlayerMicIcon(uint16_t player, bool visible);
	bool setPlayerVoiceActivation(uint16_t player, bool allowed);
	uint8_t clientVersion(uint16_t player) const;
	uint16_t clientBuild(uint16_t player) const;
	bool isExtended(uint16_t player) const;
	bool hasMicro(uint16_t player) const;
	uint8_t transportOf(uint16_t player) const;
	bool isTalking(uint16_t player) const;
	bool clientMicMuted(uint16_t player) const;
	bool clientSoundMuted(uint16_t player) const;

	void setBitrate(uint32_t bitrate);
	uint32_t bitrate() const noexcept { return bitrate_; }
	bool setMuted(uint16_t player, bool muted);
	bool isMuted(uint16_t player) const;
	bool startRecord(uint16_t player);
	bool stopRecord(uint16_t player);
	bool isRecording(uint16_t player) const;
	bool addKey(uint16_t player, uint8_t key);
	bool hasKey(uint16_t player, uint8_t key) const;
	bool removeKey(uint16_t player, uint8_t key);
	void removeAllKeys(uint16_t player);
	bool setBlocked(uint16_t listener, uint16_t speaker, bool blocked);
	bool isBlocked(uint16_t listener, uint16_t speaker) const;
	bool setPlayerVolume(uint16_t listener, uint16_t speaker, float volume);
	bool notify(uint16_t player, const std::string& text, uint32_t color, uint16_t durationMs);

	// Streams
	uint32_t createStream(StreamType type, float distance, uint32_t maxListeners, const vb::Vec3& position, uint16_t target, uint32_t color,
		const std::string& name, const void* owner = nullptr);
	bool deleteStream(uint32_t stream);
	bool isValidStream(uint32_t stream) const;
	int streamType(uint32_t stream) const;
	bool setStreamDistance(uint32_t stream, float distance);
	float streamDistance(uint32_t stream) const;
	bool setStreamPosition(uint32_t stream, const vb::Vec3& position);
	bool streamPosition(uint32_t stream, vb::Vec3& out);
	bool setStreamMaxListeners(uint32_t stream, uint32_t maxListeners);
	bool setStreamWorld(uint32_t stream, int world, int interior);
	bool setStreamFlat(uint32_t stream, bool flat);

	bool attachListener(uint32_t stream, uint16_t player);
	bool hasListener(uint32_t stream, uint16_t player) const;
	bool detachListener(uint32_t stream, uint16_t player);
	void detachAllListeners(uint32_t stream);
	int listenerCount(uint32_t stream) const;
	bool attachSpeaker(uint32_t stream, uint16_t player);
	bool hasSpeaker(uint32_t stream, uint16_t player) const;
	bool detachSpeaker(uint32_t stream, uint16_t player);
	void detachAllSpeakers(uint32_t stream);
	int speakerCount(uint32_t stream) const;
	int listenersOf(uint32_t stream, std::vector<uint16_t>& out) const;
	int speakersOf(uint32_t stream, std::vector<uint16_t>& out) const;

	bool setParameter(uint32_t stream, uint8_t parameter, float value);
	bool resetParameter(uint32_t stream, uint8_t parameter);
	bool hasParameter(uint32_t stream, uint8_t parameter) const;
	float parameter(uint32_t stream, uint8_t parameter) const;
	bool slideParameter(uint32_t stream, uint8_t parameter, float from, float to, uint32_t timeMs);
	bool slideParameterTo(uint32_t stream, uint8_t parameter, float to, uint32_t timeMs);
	bool slideParameterBy(uint32_t stream, uint8_t parameter, float delta, uint32_t timeMs);

	// Effects
	uint32_t createEffect(uint32_t number, int32_t priority, const void* params, std::size_t size, const void* owner = nullptr);
	uint32_t createEffectChain(const std::vector<EffectItem>& items, const void* owner = nullptr);
	// Deletes every stream and effect created by `owner` (an unloading script).
	void releaseOwner(const void* owner);
	bool attachEffect(uint32_t effect, uint32_t stream);
	bool detachEffect(uint32_t effect, uint32_t stream);
	bool deleteEffect(uint32_t effect);
	bool isValidEffect(uint32_t effect) const;

private:
	VoiceServer();
	~VoiceServer();

	struct Outgoing
	{
		uint16_t player;
		bool reliable;
		std::vector<uint8_t> data;
	};

	struct TunnelPacket
	{
		uint16_t player;
		std::vector<uint8_t> data;
	};

	uint64_t now() const;
	bool validPlayer(uint16_t player) const noexcept { return player < kMaxPlayers; }
	Stream* findStream(uint32_t stream);
	const Stream* findStream(uint32_t stream) const;

	// Outgoing control messages are queued while routing data is locked and
	// sent afterwards, so a transport callback can never re-enter a lock.
	void queueControl(uint16_t player, uint16_t type, const void* payload, std::size_t size, bool reliable = true);
	void queueControl(uint16_t player, uint16_t type, const std::vector<uint8_t>& payload, bool reliable = true);
	void flushOutbox();

	void resetPlayer(uint16_t player, bool notifyScripts);
	uint32_t generateKey();
	void completeHandshake(uint16_t player);
	void sendClientConfig(uint16_t player);
	void sendServerInfo(uint16_t player);
	void sendNames(uint16_t player);
	void broadcastName(uint16_t player);

	std::vector<uint8_t> createPacketFor(const Stream& stream) const;
	void sendStreamState(const Stream& stream, uint16_t player);
	void sendParameter(const Stream& stream, uint8_t parameter, const ParameterState& state, uint16_t player);
	float currentValue(const ParameterState& state) const;
	bool attachListenerLocked(Stream& stream, uint16_t player);
	bool detachListenerLocked(Stream& stream, uint16_t player, bool sendDelete);
	bool detachSpeakerLocked(Stream& stream, uint16_t player);
	bool sourcePose(const Stream& stream, Pose& out);

	void tickDynamicStreams();
	void tickPositions();
	void tickTalking();
	void tickKeepAlive();
	void tickDiagnostics();
	void processWorkerEvents();

	void handleControl(uint16_t player, uint16_t type, const uint8_t* payload, std::size_t size);
	void setTransport(uint16_t player, uint8_t transport);

	// Voice routing (any thread, routeMutex_ held shared)
	void relayVoice(uint16_t sender, uint32_t packid, const uint8_t* opus, uint16_t size);
	void deliverVoice(uint16_t listener, const Player& player, const uint8_t* packet, std::size_t size);
	void workerLoop();
	void handleDatagram(const uint8_t* data, int size, uint32_t ip, uint16_t port);

	Config config_;
	// Port that should have been used, when it was busy at startup and a
	// random one was taken instead (0 = the wanted port is in use).
	uint16_t busyPort_ = 0;
	ITransport* transport_ = nullptr;
	IWorld* world_ = nullptr;
	IScriptEvents* events_ = nullptr;
	std::atomic<bool> running_ { false };
	bool probeMode_ = false;
	uint32_t bitrate_ = vb::kDefaultBitrate;
	bool allowSampVoice_ = true;
	bool allowVoiceBridge_ = true;

	UdpSocket socket_;
	std::thread worker_;

	mutable std::shared_mutex routeMutex_;
	std::array<Player, kMaxPlayers> players_;
	std::unordered_map<uint32_t, std::unique_ptr<Stream>> streams_;
	std::unordered_map<uint32_t, uint16_t> keys_;
	std::unordered_map<uint32_t, std::unique_ptr<Effect>> effects_;
	std::map<uint32_t, std::map<uint16_t, float>> playerVolumes_;
	uint32_t nextStream_ = 1;
	uint32_t nextEffect_ = 1;
	uint32_t nextWireEffect_ = 1;

	std::array<std::atomic<uint64_t>, kMaxPlayers> udpAddress_ {};
	std::array<std::atomic<uint64_t>, kMaxPlayers> lastVoiceMs_ {};
	std::array<std::atomic<uint64_t>, kMaxPlayers> lastUdpMs_ {};
	std::array<std::atomic<bool>, kMaxPlayers> ipReported_ {};
	// IP a player's voice is locked to after its first valid packet.
	std::array<std::atomic<uint32_t>, kMaxPlayers> lockedIp_ {};

	// Worker thread only: sources sending packets without a valid session
	// are blocked for a while (key guessing / flooding).
	struct SourceState
	{
		uint64_t windowStart = 0;
		uint32_t invalid = 0;
		uint64_t blockedUntil = 0;
	};
	std::unordered_map<uint32_t, SourceState> sources_;
	bool rejectSource(uint32_t ip, uint64_t now);
	void noteInvalid(uint32_t ip, uint64_t now);
	std::vector<uint32_t> blockedSources_; // reported by the worker, logged by tick
	// Owned by the worker thread.
	std::array<uint64_t, kMaxPlayers> rateWindow_ {};
	std::array<uint32_t, kMaxPlayers> rateCount_ {};

	std::mutex workerEventsMutex_;
	std::vector<uint16_t> identified_;
	std::vector<std::pair<uint16_t, uint32_t>> ipMismatches_;
	std::mutex tunnelMutex_;
	std::vector<TunnelPacket> tunnelQueue_;

	std::vector<Outgoing> outbox_;
	uint64_t lastStreamTick_ = 0;
	uint64_t lastPositionTick_ = 0;
	uint64_t lastKeepAlive_ = 0;
	uint64_t lastDiagnostics_ = 0;
};
}
