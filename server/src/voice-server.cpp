/*
 *  Voice Bridge for open.mp and SA-MP
 */

#include "voice-server.hpp"
#include "log.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>

namespace vbs
{
namespace
{
constexpr std::size_t kHeaderSize = sizeof(vb::VoiceHeader);
constexpr uint64_t kUdpWarnAfterMs = 10000;
constexpr uint64_t kProbeDelayMs = 1500;
constexpr std::size_t kMaxTunnelQueue = 8192;
constexpr std::size_t kPositionsPerPacket = 64;
// Movement (metres) before a SampVoice listener gets a new point position.
constexpr float kLegacyPointStep = 0.4f;

// voice_log_file: empty = logs/voice-bridge.log (open.mp has a logs folder),
// else voice-bridge.log next to the server; "off" disables the file.
void openLogFile(const std::string& setting)
{
	std::string lowered = setting;
	std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (lowered == "off" || lowered == "none" || lowered == "false" || lowered == "0")
	{
		LogSetFile("");
		return;
	}
	if (!setting.empty())
	{
		if (!LogSetFile(setting))
		{
			LogWarning("could not open the voice log file '%s'", setting.c_str());
		}
	}
	else if (!LogSetFile("logs/voice-bridge.log"))
	{
		LogSetFile("voice-bridge.log");
	}
	LogDebug("---------------- voice server starting ----------------");
}

template <typename T>
void append(std::vector<uint8_t>& buffer, const T& value)
{
	const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
	buffer.insert(buffer.end(), bytes, bytes + sizeof(T));
}

void appendString(std::vector<uint8_t>& buffer, const std::string& text)
{
	buffer.insert(buffer.end(), text.begin(), text.end());
	buffer.push_back(0);
}

float distanceBetween(const vb::Vec3& a, const vb::Vec3& b)
{
	const float dx = a.x - b.x;
	const float dy = a.y - b.y;
	const float dz = a.z - b.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool knownParameter(uint8_t parameter)
{
	return parameter == vb::param::frequency || parameter == vb::param::volume || parameter == vb::param::panning
		|| parameter == vb::param::eaxmix || parameter == vb::param::src;
}

float defaultParameter(uint8_t parameter)
{
	switch (parameter)
	{
	case vb::param::volume:
		return 1.f;
	case vb::param::eaxmix:
		return -1.f;
	case vb::param::src:
		return 1.f;
	default:
		return 0.f;
	}
}

template <typename T>
void eraseValue(std::vector<T>& values, const T& value)
{
	values.erase(std::remove(values.begin(), values.end(), value), values.end());
}

const char* transportName(uint8_t transport)
{
	switch (transport)
	{
	case vb::transport::udp:
		return "udp";
	case vb::transport::tunnel:
		return "tunnel";
	default:
		return "none";
	}
}
}

TargetKind StreamTarget(StreamType type)
{
	switch (type)
	{
	case StreamType::StaticPoint:
	case StreamType::DynamicPoint:
		return TargetKind::Point;
	case StreamType::StaticPlayer:
	case StreamType::DynamicPlayer:
		return TargetKind::Player;
	case StreamType::StaticVehicle:
	case StreamType::DynamicVehicle:
		return TargetKind::Vehicle;
	case StreamType::StaticObject:
	case StreamType::DynamicObject:
		return TargetKind::Object;
	default:
		return TargetKind::None;
	}
}

bool StreamIsDynamic(StreamType type)
{
	return type >= StreamType::DynamicPoint;
}

VoiceServer& VoiceServer::Get()
{
	static VoiceServer instance;
	return instance;
}

VoiceServer::VoiceServer() = default;

VoiceServer::~VoiceServer()
{
	// Never join here: static destruction can run under the loader lock.
	running_ = false;
	if (worker_.joinable())
	{
		worker_.detach();
	}
}

uint64_t VoiceServer::now() const
{
	using namespace std::chrono;
	return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool VoiceServer::start(const Config& config, ITransport* transport, IWorld* world, IScriptEvents* events)
{
	if (running_)
	{
		return true;
	}
	config_ = config;
	transport_ = transport;
	world_ = world;
	events_ = events;
	bitrate_ = config.bitrate;
	allowSampVoice_ = config.allowSampVoice;
	allowVoiceBridge_ = config.allowVoiceBridge;
	LogSetDebug(config.debug);
	openLogFile(config.logFile);

	const std::string bindIp = config.bind.empty() ? config.gameBind : config.bind;
	const uint16_t wanted = config.port ? config.port : static_cast<uint16_t>(config.gamePort < 65535 ? config.gamePort + 1 : 0);
	std::string error;
	busyPort_ = 0;
	// A server restarted right away may still hold the port for a moment.
	bool opened = false;
	for (int attempt = 0; attempt < 10 && !opened; ++attempt)
	{
		opened = socket_.open(bindIp, wanted, error);
		if (!opened && wanted)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
		}
	}
	if (!opened)
	{
		LogError("could not open voice UDP port %u on %s: %s. Another program or server is using it.", wanted,
			bindIp.empty() ? "*" : bindIp.c_str(), error.c_str());
		std::string retryError;
		if (!socket_.open(bindIp, 0, retryError))
		{
			LogError("voice server disabled, no UDP port could be opened: %s", retryError.c_str());
			return false;
		}
		busyPort_ = wanted;
		LogError("using random UDP port %u instead of %u. Players with SampVoice will NOT talk or hear (it has no fallback), and Voice"
			" Bridge players use the slower game-connection tunnel. Fix: free port %u, or set voice_port to a free port open in the"
			" firewall/hosting panel.", socket_.localPort(), wanted, wanted);
	}

	for (uint16_t i = 0; i < kMaxPlayers; ++i)
	{
		udpAddress_[i] = 0;
		lastVoiceMs_[i] = 0;
		lastUdpMs_[i] = 0;
		ipReported_[i] = false;
		lockedIp_[i] = 0;
		rateWindow_[i] = 0;
		rateCount_[i] = 0;
	}

	running_ = true;
	worker_ = std::thread(&VoiceServer::workerLoop, this);
	const uint64_t t = now();
	lastStreamTick_ = lastPositionTick_ = lastKeepAlive_ = lastDiagnostics_ = t;

	LogInfo("voice server listening on UDP %s:%u. Open this UDP port in the firewall/hosting panel.",
		bindIp.empty() ? "0.0.0.0" : bindIp.c_str(), socket_.localPort());
	LogInfo("bitrate %u bps, %u ms frames, tunnel fallback %s%s", bitrate_, config_.frameMs,
		config_.tunnel ? "enabled" : "disabled", config_.forceTunnel ? " (forced)" : "");
	return true;
}

void VoiceServer::stop()
{
	if (!running_ && !worker_.joinable())
	{
		return;
	}
	running_ = false;
	if (worker_.joinable())
	{
		worker_.join();
	}
	socket_.close();

	std::unique_lock<std::shared_mutex> lock(routeMutex_);
	for (auto& player : players_)
	{
		player = Player {};
	}
	streams_.clear();
	keys_.clear();
	effects_.clear();
	playerVolumes_.clear();
	outbox_.clear();
	{
		std::lock_guard<std::mutex> tunnelLock(tunnelMutex_);
		tunnelQueue_.clear();
	}
	{
		std::lock_guard<std::mutex> eventLock(workerEventsMutex_);
		identified_.clear();
		ipMismatches_.clear();
	}
}

void VoiceServer::tick()
{
	if (!running_)
	{
		return;
	}
	processWorkerEvents();

	std::vector<TunnelPacket> tunnel;
	{
		std::lock_guard<std::mutex> lock(tunnelMutex_);
		tunnel.swap(tunnelQueue_);
	}
	for (auto& packet : tunnel)
	{
		queueControl(packet.player, vb::ctl::vbVoiceDown, packet.data, false);
	}

	const uint64_t t = now();
	if (t - lastStreamTick_ >= config_.streamTickMs)
	{
		lastStreamTick_ = t;
		tickDynamicStreams();
	}
	if (t - lastPositionTick_ >= config_.positionRateMs)
	{
		lastPositionTick_ = t;
		tickPositions();
	}
	tickTalking();
	if (t - lastKeepAlive_ >= config_.keepAliveMs)
	{
		lastKeepAlive_ = t;
		tickKeepAlive();
	}
	if (t - lastDiagnostics_ >= 1000)
	{
		lastDiagnostics_ = t;
		tickDiagnostics();
	}
	flushOutbox();
}

// ---------------------------------------------------------------------------
// Outgoing control messages
// ---------------------------------------------------------------------------

void VoiceServer::queueControl(uint16_t player, uint16_t type, const void* payload, std::size_t size, bool reliable)
{
	if (!validPlayer(player) || size > 0xFFFF - 8)
	{
		return;
	}
	Outgoing message;
	message.player = player;
	message.reliable = reliable;
	message.data.reserve(1 + sizeof(vb::ControlHeader) * 2 + 2 + size);
	message.data.push_back(vb::kRakPacketId);

	Player& target = players_[player];
	if (reliable && target.extended && transport_ && !transport_->orderedDelivery())
	{
		const vb::ControlHeader outer { vb::ctl::vbSequenced, static_cast<uint16_t>(2 + sizeof(vb::ControlHeader) + size) };
		append(message.data, outer);
		append(message.data, target.sequence);
		++target.sequence;
	}
	const vb::ControlHeader header { type, static_cast<uint16_t>(size) };
	append(message.data, header);
	if (size)
	{
		const auto* bytes = static_cast<const uint8_t*>(payload);
		message.data.insert(message.data.end(), bytes, bytes + size);
	}
	outbox_.push_back(std::move(message));
}

void VoiceServer::queueControl(uint16_t player, uint16_t type, const std::vector<uint8_t>& payload, bool reliable)
{
	queueControl(player, type, payload.data(), payload.size(), reliable);
}

void VoiceServer::flushOutbox()
{
	while (!outbox_.empty())
	{
		std::vector<Outgoing> pending;
		pending.swap(outbox_);
		if (!transport_)
		{
			continue;
		}
		for (const auto& message : pending)
		{
			transport_->sendPacket(message.player, message.data.data(), message.data.size(), message.reliable);
		}
	}
}

// ---------------------------------------------------------------------------
// Players
// ---------------------------------------------------------------------------

uint32_t VoiceServer::generateKey()
{
	static std::mt19937 generator { std::random_device {}() ^ static_cast<uint32_t>(now()) };
	uint32_t key = 0;
	do
	{
		key = generator();
	} while (key == 0 || keys_.count(key));
	return key;
}

bool VoiceServer::isTracked(uint16_t player) const
{
	return validPlayer(player) && players_[player].connected;
}

void VoiceServer::onPlayerConnect(uint16_t player)
{
	if (!validPlayer(player) || !running_)
	{
		return;
	}
	{
		std::unique_lock<std::shared_mutex> lock(routeMutex_);
		Player& p = players_[player];
		if (!p.connected)
		{
			p.connected = true;
			p.connectedAt = now();
		}
		if (!p.gameIp && transport_)
		{
			p.gameIp = transport_->playerIp(player);
		}
	}
	broadcastName(player);
	flushOutbox();
}

void VoiceServer::onPlayerDisconnect(uint16_t player)
{
	if (!validPlayer(player))
	{
		return;
	}
	const Player& p = players_[player];
	if (p.plugin)
	{
		LogDebug("player %u left (%s, voice %s)", player, p.extended ? "Voice Bridge" : "SampVoice",
			p.transport == vb::transport::udp ? "over UDP" : p.transport == vb::transport::tunnel ? "over the tunnel" : "never connected");
	}
	resetPlayer(player, true);
	flushOutbox();
}

void VoiceServer::resetPlayer(uint16_t player, bool notifyScripts)
{
	bool wasTalking = false;
	{
		std::unique_lock<std::shared_mutex> lock(routeMutex_);
		Player& p = players_[player];
		wasTalking = p.talking;

		for (auto& entry : streams_)
		{
			Stream& stream = *entry.second;
			if (stream.listenerMask.test(player))
			{
				stream.listenerMask.reset(player);
				eraseValue(stream.listeners, player);
			}
			if (stream.speakerMask.test(player))
			{
				stream.speakerMask.reset(player);
				eraseValue(stream.speakers, player);
			}
		}
		if (p.key)
		{
			keys_.erase(p.key);
		}
		for (auto& other : players_)
		{
			other.blocked.reset(player);
		}
		playerVolumes_.erase(player);
		for (auto& entry : playerVolumes_)
		{
			entry.second.erase(player);
		}
		p = Player {};
		udpAddress_[player] = 0;
		lastVoiceMs_[player] = 0;
		lastUdpMs_[player] = 0;
		ipReported_[player] = false;
		lockedIp_[player] = 0;
	}
	{
		std::lock_guard<std::mutex> eventLock(workerEventsMutex_);
		identified_.erase(std::remove(identified_.begin(), identified_.end(), player), identified_.end());
	}
	{
		std::lock_guard<std::mutex> tunnelLock(tunnelMutex_);
		tunnelQueue_.erase(std::remove_if(tunnelQueue_.begin(), tunnelQueue_.end(), [player](const TunnelPacket& packet) { return packet.player == player; }),
			tunnelQueue_.end());
	}
	// Drop this player's queued control messages: its id may be reused.
	outbox_.erase(std::remove_if(outbox_.begin(), outbox_.end(), [player](const Outgoing& message) { return message.player == player; }), outbox_.end());

	if (notifyScripts && wasTalking && events_)
	{
		events_->onTalking(player, false);
	}
}

void VoiceServer::onClientJoin(uint16_t player, const uint8_t* data, std::size_t size)
{
	if (!validPlayer(player) || !running_)
	{
		return;
	}

	vb::ConnectPacket legacy {};
	bool hasLegacy = false;
	const uint8_t* legacyAt = vb::findSignature(data, size, vb::kLegacySignature);
	if (legacyAt && legacyAt + sizeof(legacy) <= data + size)
	{
		std::memcpy(&legacy, legacyAt, sizeof(legacy));
		hasLegacy = true;
	}
	// The hello is only accepted right after the SampVoice packet, where the
	// client appends it: a nickname may contain "VBG1".
	vb::VbHello hello {};
	bool hasHello = false;
	if (hasLegacy && legacyAt + sizeof(legacy) + sizeof(hello) <= data + size)
	{
		std::memcpy(&hello, legacyAt + sizeof(legacy), sizeof(hello));
		hasHello = hello.magic == vb::kVbMagic && hello.protocol == vb::kVbProtocol;
	}

	// A join on an id we still track means the previous session ended
	// between two polls (SA-MP) or without a disconnect event.
	if (players_[player].connected || players_[player].plugin || players_[player].probeSent)
	{
		resetPlayer(player, true);
	}

	const ClientType type = hasHello ? ClientType::VoiceBridge : hasLegacy ? ClientType::SampVoice : ClientType::None;
	const bool allowed = type != ClientType::None && isClientTypeAllowed(type);
	{
		std::unique_lock<std::shared_mutex> lock(routeMutex_);
		Player& p = players_[player];
		p.connected = true;
		p.connectedAt = now();
		p.gameIp = transport_ ? transport_->playerIp(player) : 0;
		p.clientType = type;
		p.showSpeakerList = config_.showSpeakerList;
		p.showMicIcon = config_.showMicIcon;
		p.allowVoiceActivation = config_.allowVoiceActivation;
		if (type != ClientType::None)
		{
			// Kept for disallowed clients too, so scripts can tell why.
			p.version = hasLegacy ? legacy.version : vb::kLegacyVersion;
			p.micro = hasHello ? (hello.flags & vb::helloflag::hasMicro) != 0 : legacy.micro != 0;
			p.protocol = hasHello ? hello.protocol : 0;
			p.build = hasHello ? hello.build : 0;
			p.caps = hasHello ? hello.caps : 0;
		}
		if (allowed)
		{
			p.plugin = true;
			p.extended = hasHello;
			p.handshakeAt = p.connectedAt;
			p.key = generateKey();
			keys_[p.key] = player;
		}
	}

	if (allowed)
	{
		LogDebug("player %u joined with a voice client (version %u, %s, micro %s, ip %s)", player, players_[player].version,
			hasHello ? "Voice Bridge" : "SampVoice", players_[player].micro ? "yes" : "no", FormatIp(players_[player].gameIp).c_str());
		completeHandshake(player);
	}
	else if (type == ClientType::None)
	{
		LogDebug("player %u joined without a voice client (ip %s)", player, FormatIp(players_[player].gameIp).c_str());
	}
	else
	{
		LogInfo("player %u uses %s, which is not allowed on this server", player, hasHello ? "Voice Bridge" : "SampVoice");
		if (events_)
		{
			events_->onHandshake(player, hasLegacy ? legacy.version : vb::kLegacyVersion, hasHello,
				hasHello ? (hello.flags & vb::helloflag::hasMicro) != 0 : legacy.micro != 0);
		}
	}
	flushOutbox();
}

void VoiceServer::completeHandshake(uint16_t player)
{
	Player& p = players_[player];
	p.sequence = 0;
	sendServerInfo(player);
	if (p.extended)
	{
		sendClientConfig(player);
		sendNames(player);
	}
	if (events_)
	{
		events_->onHandshake(player, p.version, p.extended, p.micro);
	}
}

void VoiceServer::sendClientConfig(uint16_t player)
{
	const Player& p = players_[player];
	if (!p.plugin || !p.extended)
	{
		return;
	}
	const vb::VbConfig clientConfig { static_cast<uint8_t>(p.allowVoiceActivation), static_cast<uint8_t>(p.showSpeakerList),
		static_cast<uint8_t>(p.showMicIcon), 0 };
	queueControl(player, vb::ctl::vbConfig, &clientConfig, sizeof(clientConfig));
}

void VoiceServer::sendServerInfo(uint16_t player)
{
	const Player& p = players_[player];
	if (!p.extended)
	{
		const vb::ServerInfoPacket info { p.key, socket_.localPort() };
		queueControl(player, vb::ctl::serverInfo, &info, sizeof(info));
		return;
	}

	vb::VbServerInfo info {};
	info.key = p.key;
	info.port = socket_.localPort();
	info.protocol = vb::kVbProtocol;
	info.flags = static_cast<uint8_t>((config_.tunnel ? vb::serverflag::tunnelAllowed : 0) | (config_.forceTunnel ? vb::serverflag::forceTunnel : 0));
	info.frameMs = config_.frameMs;
	info.keepAliveMs = 2000;
	info.bitrate = bitrate_;
	info.mute = p.muted ? 1 : 0;
	std::vector<uint8_t> payload;
	append(payload, info);
	appendString(payload, config_.publicHost);
	queueControl(player, vb::ctl::vbServerInfo, payload);
}

void VoiceServer::sendNames(uint16_t listener)
{
	if (!world_ || !players_[listener].showSpeakerList)
	{
		return;
	}
	for (uint16_t id = 0; id < kMaxPlayers; ++id)
	{
		if (!players_[id].connected)
		{
			continue;
		}
		std::string name;
		if (!world_->playerName(id, name))
		{
			continue;
		}
		std::vector<uint8_t> payload;
		append(payload, vb::VbSpeakerName { id });
		appendString(payload, name);
		queueControl(listener, vb::ctl::vbSpeakerName, payload);
	}
}

void VoiceServer::broadcastName(uint16_t player)
{
	if (!world_ || !validPlayer(player))
	{
		return;
	}
	std::string name;
	if (!world_->playerName(player, name) || name == players_[player].sentName)
	{
		return;
	}
	players_[player].sentName = name;
	std::vector<uint8_t> payload;
	append(payload, vb::VbSpeakerName { player });
	appendString(payload, name);
	for (uint16_t id = 0; id < kMaxPlayers; ++id)
	{
		if (players_[id].plugin && players_[id].extended && players_[id].showSpeakerList)
		{
			queueControl(id, vb::ctl::vbSpeakerName, payload);
		}
	}
}

bool VoiceServer::hasPlugin(uint16_t player) const
{
	return validPlayer(player) && players_[player].plugin;
}

ClientType VoiceServer::clientType(uint16_t player) const
{
	return validPlayer(player) ? players_[player].clientType : ClientType::None;
}

void VoiceServer::setClientTypeAllowed(ClientType type, bool allowed)
{
	if (type == ClientType::SampVoice)
	{
		allowSampVoice_ = allowed;
	}
	else if (type == ClientType::VoiceBridge)
	{
		allowVoiceBridge_ = allowed;
	}
}

bool VoiceServer::isClientTypeAllowed(ClientType type) const
{
	return type == ClientType::SampVoice ? allowSampVoice_ : type == ClientType::VoiceBridge ? allowVoiceBridge_ : false;
}

bool VoiceServer::setPlayerSpeakerList(uint16_t player, bool visible)
{
	if (!validPlayer(player) || !players_[player].connected)
	{
		return false;
	}
	Player& p = players_[player];
	const bool wasVisible = p.showSpeakerList;
	p.showSpeakerList = visible;
	sendClientConfig(player);
	if (visible && !wasVisible)
	{
		sendNames(player);
	}
	flushOutbox();
	return true;
}

bool VoiceServer::setPlayerMicIcon(uint16_t player, bool visible)
{
	if (!validPlayer(player) || !players_[player].connected)
	{
		return false;
	}
	players_[player].showMicIcon = visible;
	sendClientConfig(player);
	flushOutbox();
	return true;
}

bool VoiceServer::setPlayerVoiceActivation(uint16_t player, bool allowed)
{
	if (!validPlayer(player) || !players_[player].connected)
	{
		return false;
	}
	players_[player].allowVoiceActivation = allowed;
	sendClientConfig(player);
	flushOutbox();
	return true;
}

uint8_t VoiceServer::clientVersion(uint16_t player) const
{
	return clientType(player) != ClientType::None ? players_[player].version : 0;
}

uint16_t VoiceServer::clientBuild(uint16_t player) const
{
	return clientType(player) == ClientType::VoiceBridge ? players_[player].build : 0;
}

bool VoiceServer::isExtended(uint16_t player) const
{
	return hasPlugin(player) && players_[player].extended;
}

bool VoiceServer::hasMicro(uint16_t player) const
{
	return hasPlugin(player) && players_[player].micro;
}

uint8_t VoiceServer::transportOf(uint16_t player) const
{
	return hasPlugin(player) ? players_[player].transport : vb::transport::none;
}

bool VoiceServer::isTalking(uint16_t player) const
{
	return hasPlugin(player) && players_[player].talking;
}

bool VoiceServer::clientMicMuted(uint16_t player) const
{
	return hasPlugin(player) && players_[player].clientMicMuted;
}

bool VoiceServer::clientSoundMuted(uint16_t player) const
{
	return hasPlugin(player) && players_[player].clientSoundMuted;
}

void VoiceServer::setBitrate(uint32_t bitrate)
{
	bitrate_ = std::max<uint32_t>(6000, std::min<uint32_t>(bitrate, 128000));
}

bool VoiceServer::setMuted(uint16_t player, bool muted)
{
	if (!hasPlugin(player))
	{
		return false;
	}
	{
		std::unique_lock<std::shared_mutex> lock(routeMutex_);
		if (players_[player].muted == muted)
		{
			return false;
		}
		players_[player].muted = muted;
	}
	queueControl(player, muted ? vb::ctl::muteEnable : vb::ctl::muteDisable, nullptr, 0);
	flushOutbox();
	return true;
}

bool VoiceServer::isMuted(uint16_t player) const
{
	return hasPlugin(player) && players_[player].muted;
}

bool VoiceServer::startRecord(uint16_t player)
{
	if (!hasPlugin(player))
	{
		return false;
	}
	{
		std::unique_lock<std::shared_mutex> lock(routeMutex_);
		if (players_[player].recording)
		{
			return false;
		}
		players_[player].recording = true;
	}
	queueControl(player, vb::ctl::startRecord, nullptr, 0);
	flushOutbox();
	return true;
}

bool VoiceServer::stopRecord(uint16_t player)
{
	if (!hasPlugin(player))
	{
		return false;
	}
	{
		std::unique_lock<std::shared_mutex> lock(routeMutex_);
		if (!players_[player].recording)
		{
			return false;
		}
		players_[player].recording = false;
	}
	queueControl(player, vb::ctl::stopRecord, nullptr, 0);
	flushOutbox();
	return true;
}

bool VoiceServer::isRecording(uint16_t player) const
{
	return hasPlugin(player) && players_[player].recording;
}

bool VoiceServer::addKey(uint16_t player, uint8_t key)
{
	if (!hasPlugin(player))
	{
		return false;
	}
	{
		std::unique_lock<std::shared_mutex> lock(routeMutex_);
		if (!players_[player].keys.insert(key).second)
		{
			return false;
		}
	}
	const vb::KeyPacket packet { key };
	queueControl(player, vb::ctl::addKey, &packet, sizeof(packet));
	flushOutbox();
	return true;
}

bool VoiceServer::hasKey(uint16_t player, uint8_t key) const
{
	return hasPlugin(player) && players_[player].keys.count(key) != 0;
}

bool VoiceServer::removeKey(uint16_t player, uint8_t key)
{
	if (!hasPlugin(player))
	{
		return false;
	}
	{
		std::unique_lock<std::shared_mutex> lock(routeMutex_);
		if (!players_[player].keys.erase(key))
		{
			return false;
		}
	}
	const vb::KeyPacket packet { key };
	queueControl(player, vb::ctl::removeKey, &packet, sizeof(packet));
	flushOutbox();
	return true;
}

void VoiceServer::removeAllKeys(uint16_t player)
{
	if (!hasPlugin(player))
	{
		return;
	}
	{
		std::unique_lock<std::shared_mutex> lock(routeMutex_);
		players_[player].keys.clear();
	}
	queueControl(player, vb::ctl::removeAllKeys, nullptr, 0);
	flushOutbox();
}

bool VoiceServer::setBlocked(uint16_t listener, uint16_t speaker, bool blocked)
{
	if (!validPlayer(listener) || !validPlayer(speaker) || listener == speaker || !players_[listener].connected)
	{
		return false;
	}
	std::unique_lock<std::shared_mutex> lock(routeMutex_);
	players_[listener].blocked.set(speaker, blocked);
	return true;
}

bool VoiceServer::isBlocked(uint16_t listener, uint16_t speaker) const
{
	return validPlayer(listener) && validPlayer(speaker) && players_[listener].blocked.test(speaker);
}

bool VoiceServer::setPlayerVolume(uint16_t listener, uint16_t speaker, float volume)
{
	if (!isExtended(listener) || !validPlayer(speaker))
	{
		return false;
	}
	volume = std::max(0.f, std::min(volume, 4.f));
	playerVolumes_[listener][speaker] = volume;
	const vb::VbPlayerVolume packet { speaker, volume };
	queueControl(listener, vb::ctl::vbPlayerVolume, &packet, sizeof(packet));
	flushOutbox();
	return true;
}

bool VoiceServer::notify(uint16_t player, const std::string& text, uint32_t color, uint16_t durationMs)
{
	if (!isExtended(player))
	{
		return false;
	}
	std::vector<uint8_t> payload;
	append(payload, vb::VbNotify { color, durationMs });
	appendString(payload, text.substr(0, 512));
	queueControl(player, vb::ctl::vbNotify, payload);
	flushOutbox();
	return true;
}

// ---------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------

Stream* VoiceServer::findStream(uint32_t stream)
{
	const auto it = streams_.find(stream);
	return it == streams_.end() ? nullptr : it->second.get();
}

const Stream* VoiceServer::findStream(uint32_t stream) const
{
	const auto it = streams_.find(stream);
	return it == streams_.end() ? nullptr : it->second.get();
}

uint32_t VoiceServer::createStream(StreamType type, float distance, uint32_t maxListeners, const vb::Vec3& position, uint16_t target,
	uint32_t color, const std::string& name, const void* owner)
{
	auto stream = std::make_unique<Stream>();
	stream->id = nextStream_++;
	stream->type = type;
	stream->color = color;
	stream->name = name.substr(0, 128);
	stream->distance = std::max(0.f, distance);
	stream->position = position;
	stream->target = target;
	stream->maxListeners = maxListeners == 0 ? kInfiniteListeners : maxListeners;
	stream->owner = owner;
	if (!nextStream_)
	{
		nextStream_ = 1;
	}
	const uint32_t id = stream->id;
	{
		std::unique_lock<std::shared_mutex> lock(routeMutex_);
		streams_.emplace(id, std::move(stream));
	}
	return id;
}

bool VoiceServer::deleteStream(uint32_t streamId)
{
	Stream* stream = findStream(streamId);
	if (!stream)
	{
		return false;
	}
	{
		std::unique_lock<std::shared_mutex> lock(routeMutex_);
		for (uint16_t player : std::vector<uint16_t>(stream->listeners))
		{
			detachListenerLocked(*stream, player, true);
		}
		for (uint16_t player : std::vector<uint16_t>(stream->speakers))
		{
			detachSpeakerLocked(*stream, player);
		}
		for (uint32_t effectId : stream->effects)
		{
			if (const auto it = effects_.find(effectId); it != effects_.end())
			{
				eraseValue(it->second->streams, streamId);
			}
		}
		streams_.erase(streamId);
	}
	flushOutbox();
	return true;
}

bool VoiceServer::isValidStream(uint32_t stream) const
{
	return findStream(stream) != nullptr;
}

int VoiceServer::streamType(uint32_t streamId) const
{
	const Stream* stream = findStream(streamId);
	return stream ? static_cast<int>(stream->type) : -1;
}

bool VoiceServer::setStreamDistance(uint32_t streamId, float distance)
{
	Stream* stream = findStream(streamId);
	if (!stream || StreamTarget(stream->type) == TargetKind::None)
	{
		return false;
	}
	stream->distance = std::max(0.f, distance);
	const vb::UpdateLStreamDistancePacket packet { streamId, stream->distance };
	for (uint16_t player : stream->listeners)
	{
		queueControl(player, vb::ctl::updateLStreamDistance, &packet, sizeof(packet));
	}
	flushOutbox();
	return true;
}

float VoiceServer::streamDistance(uint32_t streamId) const
{
	const Stream* stream = findStream(streamId);
	return stream ? stream->distance : 0.f;
}

bool VoiceServer::setStreamPosition(uint32_t streamId, const vb::Vec3& position)
{
	Stream* stream = findStream(streamId);
	if (!stream || StreamTarget(stream->type) != TargetKind::Point)
	{
		return false;
	}
	stream->position = position;
	const vb::UpdateLPStreamPositionPacket packet { streamId, position };
	for (uint16_t player : stream->listeners)
	{
		queueControl(player, vb::ctl::updateLPStreamPosition, &packet, sizeof(packet));
	}
	flushOutbox();
	return true;
}

bool VoiceServer::streamPosition(uint32_t streamId, vb::Vec3& out)
{
	const Stream* stream = findStream(streamId);
	if (!stream)
	{
		return false;
	}
	Pose pose;
	if (!sourcePose(*stream, pose))
	{
		return false;
	}
	out = pose.position;
	return true;
}

bool VoiceServer::setStreamMaxListeners(uint32_t streamId, uint32_t maxListeners)
{
	Stream* stream = findStream(streamId);
	if (!stream || !StreamIsDynamic(stream->type))
	{
		return false;
	}
	stream->maxListeners = maxListeners == 0 ? kInfiniteListeners : maxListeners;
	return true;
}

bool VoiceServer::setStreamWorld(uint32_t streamId, int worldId, int interior)
{
	Stream* stream = findStream(streamId);
	if (!stream)
	{
		return false;
	}
	stream->world = worldId;
	stream->interior = interior;
	stream->worldOverride = true;
	return true;
}

bool VoiceServer::setStreamFlat(uint32_t streamId, bool flat)
{
	Stream* stream = findStream(streamId);
	if (!stream)
	{
		return false;
	}
	if (flat)
	{
		stream->flags |= vb::streamflag::forceFlat;
	}
	else
	{
		stream->flags &= ~vb::streamflag::forceFlat;
	}
	for (uint16_t player : stream->listeners)
	{
		if (players_[player].extended)
		{
			uint32_t flags = stream->flags;
			if (StreamTarget(stream->type) == TargetKind::Player && stream->target == player)
			{
				flags |= vb::streamflag::sourceIsListener;
			}
			const vb::VbStreamFlags packet { streamId, flags };
			queueControl(player, vb::ctl::vbStreamFlags, &packet, sizeof(packet));
		}
	}
	flushOutbox();
	return true;
}

std::vector<uint8_t> VoiceServer::createPacketFor(const Stream& stream) const
{
	// Returns [uint16 type][payload]; the caller splits it.
	std::vector<uint8_t> packet;
	uint16_t type = vb::ctl::createGStream;
	std::vector<uint8_t> payload;
	switch (StreamTarget(stream.type))
	{
	case TargetKind::None:
		append(payload, vb::CreateGStreamPacket { stream.id, stream.color });
		break;
	case TargetKind::Point:
		type = vb::ctl::createLPStream;
		append(payload, vb::CreateLPStreamPacket { stream.id, stream.distance, stream.position, stream.color });
		break;
	case TargetKind::Player:
		type = vb::ctl::createLStreamAtPlayer;
		append(payload, vb::CreateLStreamAtPacket { stream.id, stream.distance, stream.target, stream.color });
		break;
	case TargetKind::Vehicle:
		type = vb::ctl::createLStreamAtVehicle;
		append(payload, vb::CreateLStreamAtPacket { stream.id, stream.distance, stream.target, stream.color });
		break;
	case TargetKind::Object:
		type = vb::ctl::createLStreamAtObject;
		append(payload, vb::CreateLStreamAtPacket { stream.id, stream.distance, stream.target, stream.color });
		break;
	}
	appendString(payload, stream.name);
	append(packet, type);
	packet.insert(packet.end(), payload.begin(), payload.end());
	return packet;
}

float VoiceServer::currentValue(const ParameterState& state) const
{
	if (!state.sliding || state.durationMs == 0)
	{
		return state.value;
	}
	const uint64_t elapsed = now() - state.startMs;
	if (elapsed >= state.durationMs)
	{
		return state.to;
	}
	const float progress = static_cast<float>(elapsed) / static_cast<float>(state.durationMs);
	return state.from + (state.to - state.from) * progress;
}

void VoiceServer::sendParameter(const Stream& stream, uint8_t parameter, const ParameterState& state, uint16_t player)
{
	const uint64_t elapsed = now() - state.startMs;
	if (state.sliding && elapsed < state.durationMs)
	{
		const vb::SlideStreamParameterPacket packet { stream.id, parameter, currentValue(state), state.to,
			static_cast<uint32_t>(state.durationMs - elapsed) };
		queueControl(player, vb::ctl::slideStreamParameter, &packet, sizeof(packet));
		return;
	}
	const vb::SetStreamParameterPacket packet { stream.id, parameter, state.sliding ? state.to : state.value };
	queueControl(player, vb::ctl::setStreamParameter, &packet, sizeof(packet));
}

void VoiceServer::sendStreamState(const Stream& stream, uint16_t player)
{
	Player& p = players_[player];
	Pose pose;
	if (!p.extended && StreamTarget(stream.type) == TargetKind::Player && sourcePose(stream, pose))
	{
		// The SampVoice client places a "stream at player" at that player's
		// ped matrix, which GTA does not update inside a vehicle: the voice
		// went silent as soon as the speaker got in a car.  It gets a point
		// stream instead, kept at the server's position (tickPositions).
		std::vector<uint8_t> payload;
		append(payload, vb::CreateLPStreamPacket { stream.id, stream.distance, pose.position, stream.color });
		appendString(payload, stream.name);
		queueControl(player, vb::ctl::createLPStream, payload);
		p.legacyPoints[stream.id] = pose.position;
	}
	else
	{
		const std::vector<uint8_t> create = createPacketFor(stream);
		uint16_t type;
		std::memcpy(&type, create.data(), sizeof(type));
		queueControl(player, type, create.data() + sizeof(type), create.size() - sizeof(type));
	}

	if (p.extended)
	{
		uint32_t flags = stream.flags;
		if (StreamTarget(stream.type) == TargetKind::Player && stream.target == player)
		{
			flags |= vb::streamflag::sourceIsListener;
		}
		if (flags)
		{
			const vb::VbStreamFlags packet { stream.id, flags };
			queueControl(player, vb::ctl::vbStreamFlags, &packet, sizeof(packet));
		}
	}
	for (const auto& parameter : stream.parameters)
	{
		sendParameter(stream, parameter.first, parameter.second, player);
	}
	for (uint32_t effectId : stream.effects)
	{
		const auto it = effects_.find(effectId);
		if (it == effects_.end())
		{
			continue;
		}
		for (const EffectItem& item : it->second->items)
		{
			std::vector<uint8_t> payload;
			append(payload, vb::CreateEffectPacket { stream.id, item.wireId, item.number, item.priority });
			payload.insert(payload.end(), item.params.begin(), item.params.end());
			queueControl(player, vb::ctl::createEffect, payload);
		}
	}
}

bool VoiceServer::attachListenerLocked(Stream& stream, uint16_t player)
{
	Player& p = players_[player];
	if (!p.plugin || stream.listenerMask.test(player))
	{
		return false;
	}
	stream.listenerMask.set(player);
	stream.listeners.push_back(player);
	p.listenerStreams.push_back(stream.id);
	sendStreamState(stream, player);
	return true;
}

bool VoiceServer::detachListenerLocked(Stream& stream, uint16_t player, bool sendDelete)
{
	if (!stream.listenerMask.test(player))
	{
		return false;
	}
	stream.listenerMask.reset(player);
	eraseValue(stream.listeners, player);
	eraseValue(players_[player].listenerStreams, stream.id);
	if (sendDelete && players_[player].plugin)
	{
		const vb::DeleteStreamPacket packet { stream.id };
		queueControl(player, vb::ctl::deleteStream, &packet, sizeof(packet));
	}
	return true;
}

bool VoiceServer::detachSpeakerLocked(Stream& stream, uint16_t player)
{
	if (!stream.speakerMask.test(player))
	{
		return false;
	}
	stream.speakerMask.reset(player);
	eraseValue(stream.speakers, player);
	eraseValue(players_[player].speakerStreams, stream.id);
	return true;
}

bool VoiceServer::attachListener(uint32_t streamId, uint16_t player)
{
	Stream* stream = findStream(streamId);
	if (!stream || !validPlayer(player) || StreamIsDynamic(stream->type))
	{
		return false;
	}
	bool attached;
	{
		std::unique_lock<std::shared_mutex> lock(routeMutex_);
		attached = attachListenerLocked(*stream, player);
	}
	flushOutbox();
	return attached;
}

bool VoiceServer::hasListener(uint32_t streamId, uint16_t player) const
{
	const Stream* stream = findStream(streamId);
	return stream && validPlayer(player) && stream->listenerMask.test(player);
}

bool VoiceServer::detachListener(uint32_t streamId, uint16_t player)
{
	Stream* stream = findStream(streamId);
	if (!stream || !validPlayer(player) || StreamIsDynamic(stream->type))
	{
		return false;
	}
	bool detached;
	{
		std::unique_lock<std::shared_mutex> lock(routeMutex_);
		detached = detachListenerLocked(*stream, player, true);
	}
	flushOutbox();
	return detached;
}

void VoiceServer::detachAllListeners(uint32_t streamId)
{
	Stream* stream = findStream(streamId);
	if (!stream || StreamIsDynamic(stream->type))
	{
		return;
	}
	{
		std::unique_lock<std::shared_mutex> lock(routeMutex_);
		for (uint16_t player : std::vector<uint16_t>(stream->listeners))
		{
			detachListenerLocked(*stream, player, true);
		}
	}
	flushOutbox();
}

int VoiceServer::listenerCount(uint32_t streamId) const
{
	const Stream* stream = findStream(streamId);
	return stream ? static_cast<int>(stream->listeners.size()) : 0;
}

bool VoiceServer::attachSpeaker(uint32_t streamId, uint16_t player)
{
	Stream* stream = findStream(streamId);
	if (!stream || !hasPlugin(player))
	{
		return false;
	}
	std::unique_lock<std::shared_mutex> lock(routeMutex_);
	if (stream->speakerMask.test(player))
	{
		return false;
	}
	stream->speakerMask.set(player);
	stream->speakers.push_back(player);
	players_[player].speakerStreams.push_back(streamId);
	return true;
}

bool VoiceServer::hasSpeaker(uint32_t streamId, uint16_t player) const
{
	const Stream* stream = findStream(streamId);
	return stream && validPlayer(player) && stream->speakerMask.test(player);
}

bool VoiceServer::detachSpeaker(uint32_t streamId, uint16_t player)
{
	Stream* stream = findStream(streamId);
	if (!stream || !validPlayer(player))
	{
		return false;
	}
	std::unique_lock<std::shared_mutex> lock(routeMutex_);
	return detachSpeakerLocked(*stream, player);
}

void VoiceServer::detachAllSpeakers(uint32_t streamId)
{
	Stream* stream = findStream(streamId);
	if (!stream)
	{
		return;
	}
	std::unique_lock<std::shared_mutex> lock(routeMutex_);
	for (uint16_t player : std::vector<uint16_t>(stream->speakers))
	{
		detachSpeakerLocked(*stream, player);
	}
}

int VoiceServer::speakerCount(uint32_t streamId) const
{
	const Stream* stream = findStream(streamId);
	return stream ? static_cast<int>(stream->speakers.size()) : 0;
}

int VoiceServer::listenersOf(uint32_t streamId, std::vector<uint16_t>& out) const
{
	const Stream* stream = findStream(streamId);
	out = stream ? stream->listeners : std::vector<uint16_t> {};
	return static_cast<int>(out.size());
}

int VoiceServer::speakersOf(uint32_t streamId, std::vector<uint16_t>& out) const
{
	const Stream* stream = findStream(streamId);
	out = stream ? stream->speakers : std::vector<uint16_t> {};
	return static_cast<int>(out.size());
}

// ---------------------------------------------------------------------------
// Stream parameters
// ---------------------------------------------------------------------------

bool VoiceServer::setParameter(uint32_t streamId, uint8_t parameter, float value)
{
	Stream* stream = findStream(streamId);
	if (!stream || !knownParameter(parameter))
	{
		return false;
	}
	ParameterState& state = stream->parameters[parameter];
	state = ParameterState {};
	state.value = value;
	for (uint16_t player : stream->listeners)
	{
		sendParameter(*stream, parameter, state, player);
	}
	flushOutbox();
	return true;
}

bool VoiceServer::resetParameter(uint32_t streamId, uint8_t parameter)
{
	Stream* stream = findStream(streamId);
	if (!stream || !knownParameter(parameter))
	{
		return false;
	}
	const auto it = stream->parameters.find(parameter);
	if (it == stream->parameters.end())
	{
		return false;
	}
	ParameterState reset {};
	reset.value = defaultParameter(parameter);
	for (uint16_t player : stream->listeners)
	{
		sendParameter(*stream, parameter, reset, player);
	}
	stream->parameters.erase(it);
	flushOutbox();
	return true;
}

bool VoiceServer::hasParameter(uint32_t streamId, uint8_t parameter) const
{
	const Stream* stream = findStream(streamId);
	return stream && stream->parameters.count(parameter) != 0;
}

float VoiceServer::parameter(uint32_t streamId, uint8_t parameter) const
{
	const Stream* stream = findStream(streamId);
	if (!stream || !knownParameter(parameter))
	{
		return -1.f;
	}
	const auto it = stream->parameters.find(parameter);
	return it == stream->parameters.end() ? defaultParameter(parameter) : currentValue(it->second);
}

bool VoiceServer::slideParameter(uint32_t streamId, uint8_t parameter, float from, float to, uint32_t timeMs)
{
	Stream* stream = findStream(streamId);
	if (!stream || !knownParameter(parameter))
	{
		return false;
	}
	ParameterState& state = stream->parameters[parameter];
	state.value = to;
	state.sliding = timeMs != 0;
	state.from = from;
	state.to = to;
	state.startMs = now();
	state.durationMs = timeMs;
	for (uint16_t player : stream->listeners)
	{
		sendParameter(*stream, parameter, state, player);
	}
	flushOutbox();
	return true;
}

bool VoiceServer::slideParameterTo(uint32_t streamId, uint8_t parameter, float to, uint32_t timeMs)
{
	return slideParameter(streamId, parameter, this->parameter(streamId, parameter), to, timeMs);
}

bool VoiceServer::slideParameterBy(uint32_t streamId, uint8_t parameter, float delta, uint32_t timeMs)
{
	const float from = this->parameter(streamId, parameter);
	return slideParameter(streamId, parameter, from, from + delta, timeMs);
}

// ---------------------------------------------------------------------------
// Effects
// ---------------------------------------------------------------------------

uint32_t VoiceServer::createEffect(uint32_t number, int32_t priority, const void* params, std::size_t size, const void* owner)
{
	EffectItem item;
	item.number = number;
	item.priority = priority;
	item.params.assign(static_cast<const uint8_t*>(params), static_cast<const uint8_t*>(params) + size);
	return createEffectChain({ item }, owner);
}

uint32_t VoiceServer::createEffectChain(const std::vector<EffectItem>& items, const void* owner)
{
	if (items.empty())
	{
		return 0;
	}
	auto effect = std::make_unique<Effect>();
	effect->id = nextEffect_++;
	effect->owner = owner;
	effect->items = items;
	for (EffectItem& item : effect->items)
	{
		item.wireId = nextWireEffect_++;
	}
	const uint32_t id = effect->id;
	effects_.emplace(id, std::move(effect));
	return id;
}

bool VoiceServer::attachEffect(uint32_t effectId, uint32_t streamId)
{
	const auto it = effects_.find(effectId);
	Stream* stream = findStream(streamId);
	if (it == effects_.end() || !stream)
	{
		return false;
	}
	Effect& effect = *it->second;
	if (std::find(effect.streams.begin(), effect.streams.end(), streamId) != effect.streams.end())
	{
		return false;
	}
	effect.streams.push_back(streamId);
	stream->effects.push_back(effectId);
	for (uint16_t player : stream->listeners)
	{
		for (const EffectItem& item : effect.items)
		{
			std::vector<uint8_t> payload;
			append(payload, vb::CreateEffectPacket { streamId, item.wireId, item.number, item.priority });
			payload.insert(payload.end(), item.params.begin(), item.params.end());
			queueControl(player, vb::ctl::createEffect, payload);
		}
	}
	flushOutbox();
	return true;
}

bool VoiceServer::detachEffect(uint32_t effectId, uint32_t streamId)
{
	const auto it = effects_.find(effectId);
	Stream* stream = findStream(streamId);
	if (it == effects_.end() || !stream)
	{
		return false;
	}
	Effect& effect = *it->second;
	const auto attached = std::find(effect.streams.begin(), effect.streams.end(), streamId);
	if (attached == effect.streams.end())
	{
		return false;
	}
	effect.streams.erase(attached);
	eraseValue(stream->effects, effectId);
	for (uint16_t player : stream->listeners)
	{
		for (const EffectItem& item : effect.items)
		{
			const vb::DeleteEffectPacket packet { streamId, item.wireId };
			queueControl(player, vb::ctl::deleteEffect, &packet, sizeof(packet));
		}
	}
	flushOutbox();
	return true;
}

bool VoiceServer::deleteEffect(uint32_t effectId)
{
	const auto it = effects_.find(effectId);
	if (it == effects_.end())
	{
		return false;
	}
	for (uint32_t streamId : std::vector<uint32_t>(it->second->streams))
	{
		detachEffect(effectId, streamId);
	}
	effects_.erase(effectId);
	return true;
}

bool VoiceServer::isValidEffect(uint32_t effect) const
{
	return effects_.count(effect) != 0;
}

void VoiceServer::releaseOwner(const void* owner)
{
	if (!owner)
	{
		return;
	}
	std::vector<uint32_t> effects;
	for (const auto& entry : effects_)
	{
		if (entry.second->owner == owner)
		{
			effects.push_back(entry.first);
		}
	}
	for (uint32_t effect : effects)
	{
		deleteEffect(effect);
	}
	std::vector<uint32_t> streams;
	for (const auto& entry : streams_)
	{
		if (entry.second->owner == owner)
		{
			streams.push_back(entry.first);
		}
	}
	for (uint32_t stream : streams)
	{
		deleteStream(stream);
	}
	if (!streams.empty() || !effects.empty())
	{
		LogDebug("released %zu stream(s) and %zu effect(s) of an unloaded script", streams.size(), effects.size());
	}
}

// ---------------------------------------------------------------------------
// Periodic work
// ---------------------------------------------------------------------------

bool VoiceServer::sourcePose(const Stream& stream, Pose& out)
{
	switch (StreamTarget(stream.type))
	{
	case TargetKind::Point:
		out.position = stream.position;
		out.world = stream.world;
		out.interior = stream.interior;
		return true;
	case TargetKind::Player:
		return world_ && validPlayer(stream.target) && world_->playerPose(stream.target, out);
	case TargetKind::Vehicle:
		return world_ && world_->vehiclePose(stream.target, out);
	case TargetKind::Object:
		return world_ && world_->objectPose(stream.target, out);
	default:
		return false;
	}
}

void VoiceServer::tickDynamicStreams()
{
	if (!world_)
	{
		return;
	}

	struct Candidate
	{
		uint16_t player;
		Pose pose;
	};
	std::vector<Candidate> candidates;
	candidates.reserve(64);
	std::vector<int> index(kMaxPlayers, -1);
	bool anyDynamic = false;
	for (const auto& entry : streams_)
	{
		if (StreamIsDynamic(entry.second->type))
		{
			anyDynamic = true;
			break;
		}
	}
	if (!anyDynamic)
	{
		return;
	}
	for (uint16_t id = 0; id < kMaxPlayers; ++id)
	{
		if (!players_[id].plugin || !players_[id].connected)
		{
			continue;
		}
		Pose pose;
		if (world_->playerPose(id, pose))
		{
			index[id] = static_cast<int>(candidates.size());
			candidates.push_back({ id, pose });
		}
	}

	struct Change
	{
		Stream* stream;
		std::vector<uint16_t> detach;
		std::vector<uint16_t> attach;
	};
	std::vector<Change> changes;

	for (const auto& entry : streams_)
	{
		Stream& stream = *entry.second;
		if (!StreamIsDynamic(stream.type))
		{
			continue;
		}
		Change change { &stream, {}, {} };
		Pose source;
		if (!sourcePose(stream, source))
		{
			change.detach = stream.listeners;
			if (!change.detach.empty())
			{
				changes.push_back(std::move(change));
			}
			continue;
		}
		const bool entity = StreamTarget(stream.type) != TargetKind::Point;
		const int worldFilter = stream.worldOverride || !entity ? stream.world : source.world;
		const int interiorFilter = stream.worldOverride || !entity ? stream.interior : kAnyWorld;
		const uint16_t excluded = StreamTarget(stream.type) == TargetKind::Player ? stream.target : vb::kNonePlayer;

		const auto audible = [&](const Pose& pose, float& distance)
		{
			if (worldFilter != kAnyWorld && pose.world != worldFilter)
			{
				return false;
			}
			if (interiorFilter != kAnyWorld && pose.interior != interiorFilter)
			{
				return false;
			}
			distance = distanceBetween(pose.position, source.position);
			return distance <= stream.distance;
		};

		std::size_t kept = 0;
		for (uint16_t listener : stream.listeners)
		{
			float distance = 0.f;
			const int at = index[listener];
			if (listener == excluded || at < 0 || !audible(candidates[at].pose, distance))
			{
				change.detach.push_back(listener);
			}
			else
			{
				++kept;
			}
		}

		if (kept < stream.maxListeners)
		{
			std::vector<std::pair<float, uint16_t>> nearby;
			for (const Candidate& candidate : candidates)
			{
				float distance = 0.f;
				if (candidate.player != excluded && !stream.listenerMask.test(candidate.player) && audible(candidate.pose, distance))
				{
					nearby.emplace_back(distance, candidate.player);
				}
			}
			std::sort(nearby.begin(), nearby.end());
			for (const auto& near : nearby)
			{
				if (kept >= stream.maxListeners)
				{
					break;
				}
				change.attach.push_back(near.second);
				++kept;
			}
		}
		if (!change.detach.empty() || !change.attach.empty())
		{
			changes.push_back(std::move(change));
		}
	}

	if (changes.empty())
	{
		return;
	}
	std::unique_lock<std::shared_mutex> lock(routeMutex_);
	for (Change& change : changes)
	{
		for (uint16_t player : change.detach)
		{
			detachListenerLocked(*change.stream, player, true);
		}
		for (uint16_t player : change.attach)
		{
			attachListenerLocked(*change.stream, player);
		}
	}
}

void VoiceServer::tickPositions()
{
	if (!world_)
	{
		return;
	}
	std::map<std::pair<int, uint16_t>, std::pair<bool, vb::Vec3>> cache;
	const auto lookup = [this, &cache](const Stream& stream, vb::Vec3& out)
	{
		const auto key = std::make_pair(static_cast<int>(StreamTarget(stream.type)), stream.target);
		const auto it = cache.find(key);
		if (it != cache.end())
		{
			out = it->second.second;
			return it->second.first;
		}
		Pose pose;
		const bool ok = sourcePose(stream, pose);
		cache[key] = { ok, pose.position };
		out = pose.position;
		return ok;
	};

	for (uint16_t id = 0; id < kMaxPlayers; ++id)
	{
		Player& legacy = players_[id];
		if (!legacy.plugin || legacy.extended || legacy.legacyPoints.empty())
		{
			continue;
		}
		for (auto it = legacy.legacyPoints.begin(); it != legacy.legacyPoints.end();)
		{
			const Stream* stream = findStream(it->first);
			if (!stream || !stream->listenerMask.test(id))
			{
				it = legacy.legacyPoints.erase(it);
				continue;
			}
			vb::Vec3 position {};
			if (lookup(*stream, position))
			{
				const float mx = position.x - it->second.x;
				const float my = position.y - it->second.y;
				const float mz = position.z - it->second.z;
				if (mx * mx + my * my + mz * mz > kLegacyPointStep * kLegacyPointStep)
				{
					it->second = position;
					const vb::UpdateLPStreamPositionPacket packet { stream->id, position };
					queueControl(id, vb::ctl::updateLPStreamPosition, &packet, sizeof(packet));
				}
			}
			++it;
		}
	}

	for (uint16_t id = 0; id < kMaxPlayers; ++id)
	{
		const Player& p = players_[id];
		if (!p.plugin || !p.extended || p.listenerStreams.empty() || p.transport == vb::transport::none)
		{
			continue;
		}
		std::vector<vb::VbPosition> items;
		for (uint32_t streamId : p.listenerStreams)
		{
			const Stream* stream = findStream(streamId);
			if (!stream)
			{
				continue;
			}
			const TargetKind kind = StreamTarget(stream->type);
			if (kind == TargetKind::None || kind == TargetKind::Point || (kind == TargetKind::Player && stream->target == id))
			{
				continue;
			}
			vb::Vec3 position {};
			if (lookup(*stream, position))
			{
				items.push_back({ streamId, position });
			}
		}

		for (std::size_t offset = 0; offset < items.size(); offset += kPositionsPerPacket)
		{
			const std::size_t count = std::min(kPositionsPerPacket, items.size() - offset);
			std::vector<uint8_t> payload;
			append(payload, vb::VbPositions { static_cast<uint16_t>(count) });
			for (std::size_t i = 0; i < count; ++i)
			{
				append(payload, items[offset + i]);
			}

			const uint64_t address = udpAddress_[id];
			if (p.transport == vb::transport::udp && address)
			{
				std::vector<uint8_t> packet(kHeaderSize);
				vb::VoiceHeader header {};
				header.packet = vb::voice::vbPositions;
				header.sender = vb::kNonePlayer;
				header.length = static_cast<uint16_t>(payload.size());
				vb::sealVoiceHeader(header);
				std::memcpy(packet.data(), &header, kHeaderSize);
				packet.insert(packet.end(), payload.begin(), payload.end());
				socket_.sendTo(address, packet.data(), static_cast<int>(packet.size()));
			}
			else
			{
				queueControl(id, vb::ctl::vbPositions, payload, false);
			}
		}
	}
}

void VoiceServer::tickTalking()
{
	const uint64_t t = now();
	const uint64_t hold = std::max<uint64_t>(350, config_.frameMs * 3ull);
	for (uint16_t id = 0; id < kMaxPlayers; ++id)
	{
		Player& p = players_[id];
		if (!p.plugin)
		{
			continue;
		}
		const uint64_t last = lastVoiceMs_[id];
		const bool talking = last != 0 && t - last <= hold;
		if (talking == p.talking)
		{
			continue;
		}
		p.talking = talking;
		if (talking)
		{
			broadcastName(id);
		}
		if (events_)
		{
			events_->onTalking(id, talking);
		}
	}
}

void VoiceServer::tickKeepAlive()
{
	vb::VoiceHeader header {};
	header.packet = vb::voice::keepAlive;
	vb::sealVoiceHeader(header);
	for (uint16_t id = 0; id < kMaxPlayers; ++id)
	{
		if (players_[id].plugin)
		{
			if (const uint64_t address = udpAddress_[id])
			{
				socket_.sendTo(address, &header, sizeof(header));
			}
		}
	}
}

void VoiceServer::tickDiagnostics()
{
	const uint64_t t = now();
	for (uint16_t id = 0; id < kMaxPlayers; ++id)
	{
		Player& p = players_[id];
		if (p.plugin && !p.identified && !p.udpWarned && p.handshakeAt && t - p.handshakeAt >= kUdpWarnAfterMs)
		{
			p.udpWarned = true;
			if (p.extended && p.transport == vb::transport::tunnel)
			{
				LogInfo("player %u: UDP port %u is unreachable from the client, voice is going through the game connection (tunnel)", id, port());
			}
			else
			{
				LogWarning("player %u (%s) has a voice client but none of its UDP packets reached port %u. It will not talk or hear until"
					" UDP %u is open in the firewall/hosting panel%s.", id, FormatIp(p.gameIp).c_str(), port(), port(),
					p.extended ? "" : " (SampVoice clients cannot use the tunnel)");
				if (busyPort_)
				{
					LogWarning("note: %u is a random port because the voice port %u was busy at startup (see the start of the voice log)",
						port(), busyPort_);
				}
			}
		}

		if (probeMode_ && p.connected && !p.plugin && !p.probeSent && t - p.connectedAt >= kProbeDelayMs)
		{
			{
				std::unique_lock<std::shared_mutex> lock(routeMutex_);
				p.probeSent = true;
				p.probe = true;
				p.key = generateKey();
				keys_[p.key] = id;
				p.handshakeAt = 0;
			}
			const vb::ServerInfoPacket info { p.key, port() };
			queueControl(id, vb::ctl::serverInfo, &info, sizeof(info));
		}
	}
}

void VoiceServer::processWorkerEvents()
{
	std::vector<uint16_t> identified;
	std::vector<std::pair<uint16_t, uint32_t>> mismatches;
	{
		std::lock_guard<std::mutex> lock(workerEventsMutex_);
		identified.swap(identified_);
		mismatches.swap(ipMismatches_);
	}

	std::vector<uint32_t> blocked;
	{
		std::lock_guard<std::mutex> lock(workerEventsMutex_);
		blocked.swap(blockedSources_);
	}
	for (uint32_t ip : blocked)
	{
		LogWarning("blocked %s for 5 minutes: too many voice packets without a valid session (port scan or key guessing)",
			FormatIp(ip).c_str());
	}

	for (const auto& mismatch : mismatches)
	{
		const Player& p = players_[mismatch.first];
		LogWarning("player %u sends voice from %s but plays from %s (proxy/NAT?)%s", mismatch.first, FormatIp(mismatch.second).c_str(),
			FormatIp(p.gameIp).c_str(), config_.strictIp ? "; packets rejected because voice_strict_ip is on" : "; accepted");
		if (p.clientType == ClientType::SampVoice)
		{
			// The original client converts the host the player typed with
			// inet_addr: a name ("localhost", a domain) becomes 255.255.255.255,
			// its voice leaves as a broadcast and it rejects every reply.
			LogWarning("player %u uses SampVoice: if they cannot hear anyone, they joined with a host name (e.g. localhost or a domain); "
				"SampVoice only receives voice when the server is joined by IP. The Voice Bridge client does not have this problem.",
				mismatch.first);
		}
	}

	for (uint16_t id : identified)
	{
		Player& p = players_[id];
		if (p.identified || (!p.plugin && !p.probe))
		{
			continue;
		}
		const bool probed = !p.plugin;
		{
			std::unique_lock<std::shared_mutex> lock(routeMutex_);
			p.identified = true;
			if (probed)
			{
				p.clientType = ClientType::SampVoice;
				p.plugin = true;
				p.version = vb::kLegacyVersion;
				p.micro = true;
				p.handshakeAt = now();
			}
		}
		const uint64_t address = udpAddress_[id];
		LogDebug("player %u voice UDP identified from %s:%u", id, FormatIp(AddressIp(address)).c_str(), AddressPort(address));
		if (probed && events_)
		{
			events_->onHandshake(id, p.version, false, p.micro);
		}
		if (!p.extended)
		{
			const vb::PluginInitPacket init { bitrate_, static_cast<uint8_t>(p.muted ? 1 : 0) };
			queueControl(id, vb::ctl::pluginInit, &init, sizeof(init));
		}
		if (!p.extended || p.reportedTransport != vb::transport::tunnel)
		{
			setTransport(id, vb::transport::udp);
		}
	}
}

void VoiceServer::setTransport(uint16_t player, uint8_t transport)
{
	Player& p = players_[player];
	if (p.transport == transport)
	{
		return;
	}
	{
		std::unique_lock<std::shared_mutex> lock(routeMutex_);
		p.transport = transport;
	}
	LogDebug("player %u voice transport is now %s", player, transportName(transport));
	if (events_)
	{
		events_->onTransport(player, transport);
	}
}

// ---------------------------------------------------------------------------
// Control packets from clients
// ---------------------------------------------------------------------------

void VoiceServer::onControlPacket(uint16_t player, const uint8_t* data, std::size_t size)
{
	if (!validPlayer(player) || !running_ || size < sizeof(vb::ControlHeader))
	{
		return;
	}
	vb::ControlHeader header;
	std::memcpy(&header, data, sizeof(header));
	if (sizeof(header) + header.length != size)
	{
		return;
	}
	handleControl(player, header.packet, data + sizeof(header), header.length);
	flushOutbox();
}

void VoiceServer::handleControl(uint16_t player, uint16_t type, const uint8_t* payload, std::size_t size)
{
	Player& p = players_[player];
	switch (type)
	{
	case vb::ctl::pressKey:
	case vb::ctl::releaseKey:
	{
		if (size != sizeof(vb::KeyPacket) || !p.plugin)
		{
			break;
		}
		const uint8_t key = payload[0];
		if (p.keys.count(key) && events_)
		{
			events_->onActivationKey(player, key, type == vb::ctl::pressKey);
		}
		break;
	}
	case vb::ctl::vbClientHello:
	{
		vb::VbHello hello {};
		if (size < sizeof(hello))
		{
			break;
		}
		std::memcpy(&hello, payload, sizeof(hello));
		if (hello.magic != vb::kVbMagic || hello.protocol < 1 || p.extended)
		{
			break;
		}
		p.clientType = ClientType::VoiceBridge;
		p.build = hello.build;
		if (!isClientTypeAllowed(ClientType::VoiceBridge))
		{
			LogInfo("player %u uses Voice Bridge, which is not allowed on this server", player);
			break;
		}
		// The client only got a blind/legacy serverInfo: upgrade it.
		const bool wasPlugin = p.plugin;
		{
			std::unique_lock<std::shared_mutex> lock(routeMutex_);
			p.connected = true;
			p.plugin = true;
			p.extended = true;
			p.probe = false;
			p.protocol = hello.protocol;
			p.build = hello.build;
			p.caps = hello.caps;
			p.micro = (hello.flags & vb::helloflag::hasMicro) != 0;
			if (!wasPlugin)
			{
				p.version = vb::kLegacyVersion;
			}
			if (!p.key)
			{
				p.key = generateKey();
				keys_[p.key] = player;
			}
			if (!p.handshakeAt)
			{
				p.handshakeAt = now();
			}
		}
		LogDebug("player %u upgraded to the Voice Bridge protocol", player);
		if (wasPlugin)
		{
			p.sequence = 0;
			sendServerInfo(player);
			sendClientConfig(player);
			sendNames(player);
			for (uint32_t streamId : p.listenerStreams)
			{
				const Stream* stream = findStream(streamId);
				if (!stream)
				{
					continue;
				}
				uint32_t flags = stream->flags;
				if (StreamTarget(stream->type) == TargetKind::Player && stream->target == player)
				{
					flags |= vb::streamflag::sourceIsListener;
				}
				if (flags)
				{
					const vb::VbStreamFlags packet { streamId, flags };
					queueControl(player, vb::ctl::vbStreamFlags, &packet, sizeof(packet));
				}
			}
		}
		else
		{
			completeHandshake(player);
		}
		break;
	}
	case vb::ctl::vbClientStatus:
	{
		vb::VbClientStatus status {};
		if (size < sizeof(status) || !p.extended)
		{
			break;
		}
		std::memcpy(&status, payload, sizeof(status));
		p.clientMicAvailable = status.micAvailable != 0;
		p.clientMicMuted = status.micMuted != 0;
		{
			std::unique_lock<std::shared_mutex> lock(routeMutex_);
			p.clientSoundMuted = status.soundMuted != 0;
			p.reportedTransport = status.transport;
		}
		if (status.transport == vb::transport::tunnel && config_.tunnel)
		{
			setTransport(player, vb::transport::tunnel);
		}
		else if (status.transport == vb::transport::udp && p.identified)
		{
			setTransport(player, vb::transport::udp);
		}
		else if (status.transport == vb::transport::none && !p.identified)
		{
			// A client reports "none" until it has decided; once UDP was
			// identified that report is stale.
			setTransport(player, vb::transport::none);
		}
		if (events_)
		{
			events_->onClientStatus(player, p.clientMicAvailable, p.clientMicMuted, p.clientSoundMuted);
		}
		break;
	}
	case vb::ctl::vbVoiceUp:
	{
		if (!config_.tunnel || !p.extended || size <= sizeof(uint32_t) || size - sizeof(uint32_t) > vb::kMaxVoicePacketSize - kHeaderSize)
		{
			break;
		}
		uint32_t packid;
		std::memcpy(&packid, payload, sizeof(packid));
		std::shared_lock<std::shared_mutex> lock(routeMutex_);
		relayVoice(player, packid, payload + sizeof(packid), static_cast<uint16_t>(size - sizeof(packid)));
		break;
	}
	default:
		break;
	}
}

// ---------------------------------------------------------------------------
// Voice relay
// ---------------------------------------------------------------------------

void VoiceServer::relayVoice(uint16_t sender, uint32_t packid, const uint8_t* opus, uint16_t size)
{
	const Player& speaker = players_[sender];
	if (!speaker.plugin || speaker.muted || (!speaker.recording && speaker.keys.empty()))
	{
		return;
	}
	if (size == 0 || kHeaderSize + size > vb::kMaxVoicePacketSize)
	{
		return;
	}
	lastVoiceMs_[sender] = now();
	if (speaker.speakerStreams.empty())
	{
		return;
	}

	uint8_t packet[vb::kMaxVoicePacketSize];
	std::memcpy(packet + kHeaderSize, opus, size);
	for (uint32_t streamId : speaker.speakerStreams)
	{
		const auto it = streams_.find(streamId);
		if (it == streams_.end())
		{
			continue;
		}
		const Stream& stream = *it->second;
		vb::VoiceHeader header {};
		header.packet = vb::voice::voicePacket;
		header.stream = streamId;
		header.sender = sender;
		header.length = size;
		header.packid = packid;
		vb::sealVoiceHeader(header);
		std::memcpy(packet, &header, kHeaderSize);

		for (uint16_t listener : stream.listeners)
		{
			if (listener == sender)
			{
				continue;
			}
			const Player& target = players_[listener];
			if (!target.plugin || target.blocked.test(sender) || target.clientSoundMuted)
			{
				continue;
			}
			deliverVoice(listener, target, packet, kHeaderSize + size);
		}
	}
}

void VoiceServer::deliverVoice(uint16_t listener, const Player& player, const uint8_t* packet, std::size_t size)
{
	if (player.transport == vb::transport::tunnel)
	{
		std::lock_guard<std::mutex> lock(tunnelMutex_);
		if (tunnelQueue_.size() < kMaxTunnelQueue)
		{
			tunnelQueue_.push_back({ listener, std::vector<uint8_t>(packet, packet + size) });
		}
		return;
	}
	if (const uint64_t address = udpAddress_[listener])
	{
		socket_.sendTo(address, packet, static_cast<int>(size));
	}
}

bool VoiceServer::rejectSource(uint32_t ip, uint64_t t)
{
	const auto it = sources_.find(ip);
	return it != sources_.end() && it->second.blockedUntil > t;
}

void VoiceServer::noteInvalid(uint32_t ip, uint64_t t)
{
	constexpr uint64_t kWindowMs = 10000;
	constexpr uint32_t kMaxInvalid = 30;
	constexpr uint64_t kBlockMs = 5 * 60 * 1000;
	if (sources_.size() > 8192)
	{
		for (auto it = sources_.begin(); it != sources_.end();)
		{
			it = it->second.blockedUntil <= t && t - it->second.windowStart > kWindowMs ? sources_.erase(it) : std::next(it);
		}
		if (sources_.size() > 8192)
		{
			return; // flood from many addresses: stay cheap
		}
	}
	SourceState& state = sources_[ip];
	if (t - state.windowStart > kWindowMs)
	{
		state.windowStart = t;
		state.invalid = 0;
	}
	if (++state.invalid > kMaxInvalid && state.blockedUntil <= t)
	{
		state.blockedUntil = t + kBlockMs;
		std::lock_guard<std::mutex> lock(workerEventsMutex_);
		if (blockedSources_.size() < 64)
		{
			blockedSources_.push_back(ip);
		}
	}
}

void VoiceServer::workerLoop()
{
	std::vector<uint8_t> buffer(vb::kMaxVoicePacketSize + 64);
	while (running_)
	{
		uint32_t ip = 0;
		uint16_t port = 0;
		const int received = socket_.receive(buffer.data(), static_cast<int>(buffer.size()), ip, port);
		if (received < 0)
		{
			break;
		}
		if (received > 0)
		{
			handleDatagram(buffer.data(), received, ip, port);
		}
	}
}

void VoiceServer::handleDatagram(const uint8_t* data, int size, uint32_t ip, uint16_t port)
{
	const uint64_t t = now();
	if (rejectSource(ip, t))
	{
		return;
	}
	if (size < static_cast<int>(kHeaderSize) || size > static_cast<int>(vb::kMaxVoicePacketSize))
	{
		noteInvalid(ip, t);
		return;
	}
	vb::VoiceHeader header;
	std::memcpy(&header, data, kHeaderSize);
	if (!vb::checkVoiceHeader(header) || kHeaderSize + header.length != static_cast<std::size_t>(size))
	{
		noteInvalid(ip, t);
		return;
	}

	bool firstContact = false;
	bool replyKeepAlive = false;
	uint64_t address = 0;
	{
		std::shared_lock<std::shared_mutex> lock(routeMutex_);
		const auto it = keys_.find(header.svrkey);
		if (it == keys_.end())
		{
			noteInvalid(ip, t);
			return;
		}
		const uint16_t player = it->second;
		const Player& p = players_[player];
		if ((!p.plugin && !p.probe) || p.key != header.svrkey)
		{
			noteInvalid(ip, t);
			return;
		}
		// After the first valid packet the session is bound to that IP: a
		// guessed key from elsewhere can neither inject voice nor redirect
		// the player's audio.  (The port may still change: NAT rebinding.)
		uint32_t expected = 0;
		if (!lockedIp_[player].compare_exchange_strong(expected, ip) && expected != ip)
		{
			if (ip != p.gameIp)
			{
				noteInvalid(ip, t);
				return;
			}
			lockedIp_[player] = ip; // the game connection's own IP always wins
		}
		if (p.gameIp && p.gameIp != ip)
		{
			if (!ipReported_[player].exchange(true))
			{
				std::lock_guard<std::mutex> eventLock(workerEventsMutex_);
				ipMismatches_.emplace_back(player, ip);
			}
			if (config_.strictIp)
			{
				return;
			}
		}

		if (t - rateWindow_[player] >= 1000)
		{
			rateWindow_[player] = t;
			rateCount_[player] = 0;
		}
		if (++rateCount_[player] > config_.maxPacketsPerSecond)
		{
			return;
		}

		address = PackAddress(ip, port);
		const uint64_t previous = udpAddress_[player].exchange(address);
		lastUdpMs_[player] = t;
		if (!previous)
		{
			firstContact = true;
			std::lock_guard<std::mutex> eventLock(workerEventsMutex_);
			identified_.push_back(player);
		}
		else if (previous != address)
		{
			LogDebug("player %u voice address moved to %s:%u", player, FormatIp(ip).c_str(), port);
		}
		replyKeepAlive = firstContact || (p.extended && header.packet == vb::voice::keepAlive);

		if (header.packet == vb::voice::voicePacket && header.length)
		{
			relayVoice(player, header.packid, data + kHeaderSize, header.length);
		}
	}

	if (replyKeepAlive)
	{
		vb::VoiceHeader reply {};
		reply.packet = vb::voice::keepAlive;
		vb::sealVoiceHeader(reply);
		socket_.sendTo(address, &reply, sizeof(reply));
	}
}
}
