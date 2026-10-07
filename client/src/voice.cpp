/*
 *  Voice Bridge client
 */

#include "voice.hpp"
#include "audio.hpp"
#include "game.hpp"
#include "log.hpp"
#include "samp.hpp"
#include "settings.hpp"
#include "version.hpp"
#include <vb-protocol.hpp>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <algorithm>
#include <cctype>
#include <cstring>

#ifndef SIO_UDP_CONNRESET
	#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace vbc
{
namespace
{
constexpr uint64_t kUdpTimeoutMs = 12000;
constexpr uint64_t kTunnelAfterMs = 4000;
constexpr uint64_t kProbeIntervalMs = 300;
constexpr uint64_t kTunnelProbeIntervalMs = 3000;
constexpr std::size_t kMaxTunnelQueue = 64;
constexpr uintptr_t kInvalidSocket = ~uintptr_t(0);

uint64_t now()
{
	return GetTickCount64();
}

uint64_t packAddress(uint32_t ip, uint16_t port)
{
	return (uint64_t(1) << 48) | (uint64_t(ip) << 16) | port;
}



template <typename T>
bool read(const uint8_t* payload, std::size_t size, T& out)
{
	if (size < sizeof(T))
	{
		return false;
	}
	std::memcpy(&out, payload, sizeof(T));
	return true;
}

std::string trailingString(const uint8_t* payload, std::size_t size, std::size_t offset)
{
	if (size <= offset)
	{
		return {};
	}
	const char* text = reinterpret_cast<const char*>(payload + offset);
	return std::string(text, strnlen(text, size - offset));
}

game::Vec3 toVec(const vb::Vec3& value)
{
	return { value.x, value.y, value.z };
}

uint32_t resolve(const std::string& host)
{
	in_addr address {};
	if (inet_pton(AF_INET, host.c_str(), &address) == 1)
	{
		return address.s_addr;
	}
	addrinfo hints {};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	addrinfo* result = nullptr;
	if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result)
	{
		return 0;
	}
	const uint32_t ip = reinterpret_cast<const sockaddr_in*>(result->ai_addr)->sin_addr.s_addr;
	freeaddrinfo(result);
	return ip;
}
}

VoiceClient& VoiceClient::Get()
{
	// Never destroyed: on exit (/q) Windows has already killed the UDP thread
	// and destroying a std::thread that was running calls std::terminate,
	// which SA-MP reports as a crash.  The OS frees everything anyway.
	static VoiceClient* instance = new VoiceClient();
	return *instance;
}


// ---------------------------------------------------------------------------
// RakNet events (game thread)
// ---------------------------------------------------------------------------

void VoiceClient::onConnect(const char* host, unsigned short port)
{
	resetSession();
	host_ = host ? host : "";
	gamePort_ = port;
	Log("connecting to a server");
}

void VoiceClient::onConnectionAccepted(uint32_t serverIp, uint16_t)
{
	serverIp_ = serverIp;
}

void VoiceClient::onDisconnect()
{
	Log("disconnected from the server");
	resetSession();
}

std::vector<uint8_t> VoiceClient::joinPayload()
{
	const bool micro = !Audio::Get().microphones().empty();
	const vb::ConnectPacket legacy { vb::kLegacySignature, vb::kLegacyVersion, static_cast<uint8_t>(micro) };
	const vb::VbHello hello { vb::kVbMagic, vb::kVbProtocol, static_cast<uint8_t>(micro ? vb::helloflag::hasMicro : 0),
		static_cast<uint16_t>(VOICE_BRIDGE_CLIENT_BUILD), 0 };
	std::vector<uint8_t> payload(sizeof(legacy) + sizeof(hello));
	std::memcpy(payload.data(), &legacy, sizeof(legacy));
	std::memcpy(payload.data() + sizeof(legacy), &hello, sizeof(hello));
	return payload;
}


void VoiceClient::onPacket(const uint8_t* data, std::size_t size)
{
	vb::ControlHeader header;
	if (!read(data, size, header) || sizeof(header) + header.length != size)
	{
		return;
	}
	const uint8_t* payload = data + sizeof(header);
	if (header.packet != vb::ctl::vbSequenced)
	{
		handleControl(header.packet, payload, header.length);
		return;
	}

	uint16_t sequence;
	if (!read(payload, header.length, sequence))
	{
		return;
	}
	const auto delta = static_cast<uint16_t>(sequence - expectedSequence_);
	if (delta >= 0x8000)
	{
		return; // already handled
	}
	pendingSequenced_[sequence].assign(payload + sizeof(sequence), payload + header.length);
	// Deliver every message that is now in order.
	for (auto it = pendingSequenced_.find(expectedSequence_); it != pendingSequenced_.end(); it = pendingSequenced_.find(expectedSequence_))
	{
		const std::vector<uint8_t> message = std::move(it->second);
		pendingSequenced_.erase(it);
		++expectedSequence_;
		vb::ControlHeader inner;
		if (read(message.data(), message.size(), inner) && sizeof(inner) + inner.length == message.size())
		{
			handleControl(inner.packet, message.data() + sizeof(inner), inner.length);
		}
	}
	if (pendingSequenced_.empty())
	{
		sequenceGapSince_ = 0;
	}
	else if (!sequenceGapSince_)
	{
		sequenceGapSince_ = now();
	}
	skipSequenceGap(false);
}

void VoiceClient::skipSequenceGap(bool force)
{
	// The transport is reliable, so a gap only lasts while a message is
	// retransmitted.  If it never arrives (the server dropped it), continue
	// from the oldest buffered message instead of stalling forever.
	if (pendingSequenced_.empty() || !sequenceGapSince_)
	{
		return;
	}
	if (!force && now() - sequenceGapSince_ < 2000 && pendingSequenced_.size() <= 1024)
	{
		return;
	}
	uint16_t oldest = pendingSequenced_.begin()->first;
	uint16_t best = 0xFFFF;
	for (const auto& entry : pendingSequenced_)
	{
		const auto distance = static_cast<uint16_t>(entry.first - expectedSequence_);
		if (distance < best)
		{
			best = distance;
			oldest = entry.first;
		}
	}
	Log("control messages %u..%u never arrived; skipping them", expectedSequence_, static_cast<uint16_t>(oldest - 1));
	expectedSequence_ = oldest;
	sequenceGapSince_ = 0;
	for (auto it = pendingSequenced_.find(expectedSequence_); it != pendingSequenced_.end(); it = pendingSequenced_.find(expectedSequence_))
	{
		const std::vector<uint8_t> message = std::move(it->second);
		pendingSequenced_.erase(it);
		++expectedSequence_;
		vb::ControlHeader inner;
		if (read(message.data(), message.size(), inner) && sizeof(inner) + inner.length == message.size())
		{
			handleControl(inner.packet, message.data() + sizeof(inner), inner.length);
		}
	}
	sequenceGapSince_ = pendingSequenced_.empty() ? 0 : now();
}

// ---------------------------------------------------------------------------
// Control messages
// ---------------------------------------------------------------------------

void VoiceClient::handleControl(uint16_t type, const uint8_t* p, std::size_t size)
{
	Audio& audio = Audio::Get();
	switch (type)
	{
	case vb::ctl::serverInfo:
	{
		vb::ServerInfoPacket info;
		if (!read(p, size, info) || server_ == ServerKind::VoiceBridge)
		{
			break;
		}
		Log("SampVoice server detected");
		server_ = ServerKind::SampVoice;
		key_ = info.serverKey;
		voicePort_ = info.serverPort;
		frameMs_ = vb::kLegacyFrameMs;
		sessionStart_ = now();
		serverPositions_ = false;
		startUdp(host_);
		// Ask for the extended protocol; SampVoice servers ignore it.
		expectedSequence_ = 0;
		pendingSequenced_.clear();
		const auto hello = joinPayload();
		sendControl(vb::ctl::vbClientHello, hello.data() + sizeof(vb::ConnectPacket), sizeof(vb::VbHello));
		break;
	}
	case vb::ctl::pluginInit:
	{
		vb::PluginInitPacket init;
		if (!read(p, size, init))
		{
			break;
		}
		bitrate_ = init.bitrate;
		muted_ = init.mute != 0;
		legacyReady_ = true;
		audio.configureEncoder(bitrate_, vb::kLegacyFrameMs);
		Log("voice ready (bitrate %u, muted %u)", bitrate_, init.mute);
		break;
	}
	case vb::ctl::vbServerInfo:
	{
		vb::VbServerInfo info;
		if (!read(p, size, info))
		{
			break;
		}
		const std::string publicHost = trailingString(p, size, sizeof(info));
		const bool upgraded = server_ == ServerKind::SampVoice;
		server_ = ServerKind::VoiceBridge;
		key_ = info.key;
		voicePort_ = info.port;
		frameMs_ = info.frameMs;
		bitrate_ = info.bitrate;
		keepAliveMs_ = std::max<uint32_t>(500, info.keepAliveMs);
		tunnelAllowed_ = (info.flags & vb::serverflag::tunnelAllowed) != 0;
		forceTunnel_ = (info.flags & vb::serverflag::forceTunnel) != 0;
		muted_ = info.mute != 0;
		serverPositions_ = true;
		if (!upgraded)
		{
			sessionStart_ = now();
		}
		audio.configureEncoder(bitrate_, frameMs_);
		Log("Voice Bridge server detected (%u ms frames, tunnel %s)", frameMs_, tunnelAllowed_ ? "allowed" : "disabled");
		if (upgraded)
		{
			udpKey_ = key_;
		}
		startUdp(publicHost.empty() ? host_ : publicHost);
		reportedTransport_ = 0xFF;
		break;
	}
	case vb::ctl::vbConfig:
	{
		vb::VbConfig config;
		if (read(p, size, config))
		{
			allowVoiceActivation_ = config.allowVoiceActivation != 0;
			showSpeakerList_ = config.showSpeakerList != 0;
			showMicIcon_ = config.showMicIcon != 0;
		}
		break;
	}
	case vb::ctl::muteEnable:
		muted_ = true;
		releaseKeys();
		break;
	case vb::ctl::muteDisable:
		muted_ = false;
		break;
	case vb::ctl::startRecord:
		recordingForced_ = true;
		break;
	case vb::ctl::stopRecord:
		recordingForced_ = false;
		break;
	case vb::ctl::addKey:
		if (size == 1)
		{
			keys_.insert(p[0]);
		}
		break;
	case vb::ctl::removeKey:
		if (size == 1)
		{
			keys_.erase(p[0]);
			if (pressed_.erase(p[0]))
			{
				sendControl(vb::ctl::releaseKey, p, 1);
			}
		}
		break;
	case vb::ctl::removeAllKeys:
		releaseKeys();
		keys_.clear();
		break;
	case vb::ctl::createGStream:
	{
		vb::CreateGStreamPacket packet;
		if (read(p, size, packet))
		{
			audio.createStream(packet.stream, StreamKind::Global, packet.color, trailingString(p, size, sizeof(packet)), 0.f, {}, 0xFFFF);
		}
		break;
	}
	case vb::ctl::createLPStream:
	{
		vb::CreateLPStreamPacket packet;
		if (read(p, size, packet))
		{
			audio.createStream(packet.stream, StreamKind::Point, packet.color, trailingString(p, size, sizeof(packet)), packet.distance,
				toVec(packet.position), 0xFFFF);
		}
		break;
	}
	case vb::ctl::createLStreamAtPlayer:
	case vb::ctl::createLStreamAtVehicle:
	case vb::ctl::createLStreamAtObject:
	{
		vb::CreateLStreamAtPacket packet;
		if (!read(p, size, packet))
		{
			break;
		}
		const StreamKind kind = type == vb::ctl::createLStreamAtPlayer ? StreamKind::Player
			: type == vb::ctl::createLStreamAtVehicle				   ? StreamKind::Vehicle
																	   : StreamKind::Object;
		audio.createStream(packet.stream, kind, packet.color, trailingString(p, size, sizeof(packet)), packet.distance, {},
			static_cast<uint16_t>(packet.target));
		break;
	}
	case vb::ctl::updateLStreamDistance:
	{
		vb::UpdateLStreamDistancePacket packet;
		if (read(p, size, packet))
		{
			audio.setStreamDistance(packet.stream, packet.distance);
		}
		break;
	}
	case vb::ctl::updateLPStreamPosition:
	{
		vb::UpdateLPStreamPositionPacket packet;
		if (read(p, size, packet))
		{
			audio.setStreamPosition(packet.stream, toVec(packet.position));
		}
		break;
	}
	case vb::ctl::deleteStream:
	{
		vb::DeleteStreamPacket packet;
		if (read(p, size, packet))
		{
			audio.deleteStream(packet.stream);
		}
		break;
	}
	case vb::ctl::setStreamParameter:
	{
		vb::SetStreamParameterPacket packet;
		if (read(p, size, packet))
		{
			audio.setParameter(packet.stream, packet.parameter, packet.value);
		}
		break;
	}
	case vb::ctl::slideStreamParameter:
	{
		vb::SlideStreamParameterPacket packet;
		if (read(p, size, packet))
		{
			audio.slideParameter(packet.stream, packet.parameter, packet.startvalue, packet.endvalue, packet.time);
		}
		break;
	}
	case vb::ctl::createEffect:
	{
		vb::CreateEffectPacket packet;
		if (read(p, size, packet))
		{
			audio.createEffect(packet.stream, packet.effect, packet.number, packet.priority, p + sizeof(packet), size - sizeof(packet));
		}
		break;
	}
	case vb::ctl::deleteEffect:
	{
		vb::DeleteEffectPacket packet;
		if (read(p, size, packet))
		{
			audio.deleteEffect(packet.stream, packet.effect);
		}
		break;
	}
	case vb::ctl::vbPositions:
	{
		vb::VbPositions header;
		if (!read(p, size, header) || sizeof(header) + header.count * sizeof(vb::VbPosition) > size)
		{
			break;
		}
		for (uint16_t i = 0; i < header.count; ++i)
		{
			vb::VbPosition item;
			std::memcpy(&item, p + sizeof(header) + i * sizeof(item), sizeof(item));
			audio.setSourcePosition(item.stream, toVec(item.position));
		}
		break;
	}
	case vb::ctl::vbPlayerVolume:
	{
		vb::VbPlayerVolume packet;
		if (read(p, size, packet))
		{
			audio.setServerPlayerVolume(packet.player, packet.volume);
		}
		break;
	}
	case vb::ctl::vbSpeakerName:
	{
		vb::VbSpeakerName packet;
		if (read(p, size, packet))
		{
			names_[packet.player] = trailingString(p, size, sizeof(packet));
		}
		break;
	}
	case vb::ctl::vbNotify:
	{
		vb::VbNotify packet;
		if (read(p, size, packet))
		{
			notify(trailingString(p, size, sizeof(packet)), packet.color, packet.durationMs);
		}
		break;
	}
	case vb::ctl::vbStreamFlags:
	{
		vb::VbStreamFlags packet;
		if (read(p, size, packet))
		{
			audio.setStreamFlags(packet.stream, packet.flags);
		}
		break;
	}
	case vb::ctl::vbVoiceDown:
	{
		vb::VoiceHeader header;
		if (read(p, size, header) && vb::checkVoiceHeader(header) && sizeof(header) + header.length == size && header.packet == vb::voice::voicePacket)
		{
			audio.pushVoice(header.stream, header.sender, header.packid, p + sizeof(header), header.length);
		}
		break;
	}
	default:
		break;
	}
}

void VoiceClient::sendControl(uint16_t type, const void* payload, std::size_t size, bool reliable)
{
	std::vector<uint8_t> packet(1 + sizeof(vb::ControlHeader) + size);
	packet[0] = vb::kRakPacketId;
	const vb::ControlHeader header { type, static_cast<uint16_t>(size) };
	std::memcpy(packet.data() + 1, &header, sizeof(header));
	if (size)
	{
		std::memcpy(packet.data() + 1 + sizeof(header), payload, size);
	}
	rak::Send(packet.data(), packet.size(), reliable);
}

// ---------------------------------------------------------------------------
// UDP
// ---------------------------------------------------------------------------

void VoiceClient::startUdp(const std::string& host)
{
	stopUdp();
	udpKey_ = key_;
	lastServerPacket_ = 0;
	udpConfirmed_ = false;
	lastKeepAlive_ = 0;

	WSADATA data {};
	WSAStartup(MAKEWORD(2, 2), &data);
	const SOCKET handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (handle == INVALID_SOCKET)
	{
		Log("could not create the voice socket (%d)", WSAGetLastError());
		return;
	}
	// SampVoice's client stopped receiving forever after one ICMP
	// "port unreachable"; ignore those resets.
	BOOL reportReset = FALSE;
	DWORD returned = 0;
	WSAIoctl(handle, SIO_UDP_CONNRESET, &reportReset, sizeof(reportReset), nullptr, 0, &returned, nullptr, nullptr);
	const DWORD timeout = 200;
	setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
	const int receiveBuffer = 1024 * 1024;
	setsockopt(handle, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receiveBuffer), sizeof(receiveBuffer));
	sockaddr_in local {};
	local.sin_family = AF_INET;
	bind(handle, reinterpret_cast<sockaddr*>(&local), sizeof(local));

	{
		std::lock_guard<std::mutex> lock(mutex_);
		socket_ = static_cast<uintptr_t>(handle);
	}
	// Prefer the address RakNet actually connected to: it already went
	// through DNS, while SampVoice failed on host names.
	const bool sameHost = host == host_;
	serverAddress_ = sameHost && serverIp_ ? packAddress(serverIp_, voicePort_) : 0;
	const uint16_t port = voicePort_;
	udpRunning_ = true;
	const uint32_t generation = ++udpGeneration_;
	udpThread_ = std::thread([this, host, port, generation]
	{
		if (!serverAddress_)
		{
			resolving_ = true;
			const uint32_t ip = resolve(host);
			resolving_ = false;
			if (generation != udpGeneration_)
			{
				return; // the session ended while resolving
			}
			if (!ip)
			{
				Log("could not resolve the voice server address");
			}
			else
			{
				serverAddress_ = packAddress(ip, port);
			}
		}
		udpLoop();
	});
}

void VoiceClient::stopUdp()
{
	udpRunning_ = false;
	++udpGeneration_;
	if (udpThread_.joinable())
	{
		// A thread blocked in DNS resolution is detached rather than waited
		// for on the game thread; it exits once resolution returns.
		if (resolving_)
		{
			udpThread_.detach();
		}
		else
		{
			udpThread_.join();
		}
	}
	std::lock_guard<std::mutex> lock(mutex_);
	if (socket_ != kInvalidSocket)
	{
		closesocket(static_cast<SOCKET>(socket_));
		socket_ = kInvalidSocket;
		WSACleanup();
	}
	serverAddress_ = 0;
}

void VoiceClient::udpLoop()
{
	uint8_t buffer[vb::kMaxVoicePacketSize + 64];
	const uint32_t generation = udpGeneration_;
	while (udpRunning_ && generation == udpGeneration_)
	{
		SOCKET handle;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			handle = static_cast<SOCKET>(socket_);
		}
		sockaddr_in from {};
		int fromSize = sizeof(from);
		const int size = recvfrom(handle, reinterpret_cast<char*>(buffer), sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &fromSize);
		if (size <= 0)
		{
			continue; // timeout or transient error: never give up
		}
		const uint64_t address = serverAddress_;
		if (!address || from.sin_addr.s_addr != static_cast<uint32_t>(address >> 16))
		{
			continue;
		}
		handleDatagram(buffer, size);
	}
}

void VoiceClient::handleDatagram(const uint8_t* data, int size)
{
	vb::VoiceHeader header;
	if (size < static_cast<int>(sizeof(header)))
	{
		return;
	}
	std::memcpy(&header, data, sizeof(header));
	if (!vb::checkVoiceHeader(header) || sizeof(header) + header.length != static_cast<std::size_t>(size))
	{
		return;
	}
	lastServerPacket_ = now();
	const uint8_t* payload = data + sizeof(header);
	switch (header.packet)
	{
	case vb::voice::voicePacket:
		Audio::Get().pushVoice(header.stream, header.sender, header.packid, payload, header.length);
		break;
	case vb::voice::vbPositions:
	{
		vb::VbPositions positions;
		if (!read(payload, header.length, positions) || sizeof(positions) + positions.count * sizeof(vb::VbPosition) > header.length)
		{
			break;
		}
		for (uint16_t i = 0; i < positions.count; ++i)
		{
			vb::VbPosition item;
			std::memcpy(&item, payload + sizeof(positions) + i * sizeof(item), sizeof(item));
			Audio::Get().setSourcePosition(item.stream, toVec(item.position));
		}
		break;
	}
	default:
		break;
	}
}

void VoiceClient::sendKeepAlive()
{
	const uint64_t address = serverAddress_;
	if (!address)
	{
		return;
	}
	vb::VoiceHeader header {};
	header.svrkey = udpKey_;
	header.packet = vb::voice::keepAlive;
	vb::sealVoiceHeader(header);
	sockaddr_in to {};
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = static_cast<uint32_t>(address >> 16);
	to.sin_port = htons(static_cast<uint16_t>(address & 0xFFFF));
	std::lock_guard<std::mutex> lock(mutex_);
	if (socket_ != kInvalidSocket)
	{
		sendto(static_cast<SOCKET>(socket_), reinterpret_cast<const char*>(&header), sizeof(header), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
	}
}

void VoiceClient::onEncodedFrame(const uint8_t* data, std::size_t size, uint32_t packid)
{
	if (!sending_ || !size)
	{
		return;
	}
	if (tunnelActive_)
	{
		std::vector<uint8_t> packet(1 + sizeof(vb::ControlHeader) + sizeof(packid) + size);
		packet[0] = vb::kRakPacketId;
		const vb::ControlHeader header { vb::ctl::vbVoiceUp, static_cast<uint16_t>(sizeof(packid) + size) };
		std::memcpy(packet.data() + 1, &header, sizeof(header));
		std::memcpy(packet.data() + 1 + sizeof(header), &packid, sizeof(packid));
		std::memcpy(packet.data() + 1 + sizeof(header) + sizeof(packid), data, size);
		std::lock_guard<std::mutex> lock(tunnelMutex_);
		if (tunnelOut_.size() < kMaxTunnelQueue)
		{
			tunnelOut_.push_back(std::move(packet));
		}
		return;
	}

	const uint64_t address = serverAddress_;
	if (!address)
	{
		return;
	}
	uint8_t packet[vb::kMaxVoicePacketSize];
	if (sizeof(vb::VoiceHeader) + size > sizeof(packet))
	{
		return;
	}
	vb::VoiceHeader header {};
	header.svrkey = udpKey_;
	header.packet = vb::voice::voicePacket;
	header.length = static_cast<uint16_t>(size);
	header.packid = packid;
	vb::sealVoiceHeader(header);
	std::memcpy(packet, &header, sizeof(header));
	std::memcpy(packet + sizeof(header), data, size);
	sockaddr_in to {};
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = static_cast<uint32_t>(address >> 16);
	to.sin_port = htons(static_cast<uint16_t>(address & 0xFFFF));
	std::lock_guard<std::mutex> lock(mutex_);
	if (socket_ != kInvalidSocket)
	{
		sendto(static_cast<SOCKET>(socket_), reinterpret_cast<const char*>(packet), static_cast<int>(sizeof(header) + size), 0,
			reinterpret_cast<sockaddr*>(&to), sizeof(to));
	}
}

// ---------------------------------------------------------------------------
// Per-frame logic
// ---------------------------------------------------------------------------

void VoiceClient::resetSession()
{
	stopUdp();
	releaseKeys();
	Audio::Get().setTransmitting(false);
	Audio::Get().clearStreams();
	Audio::Get().clearPlayerSettings();
	{
		std::lock_guard<std::mutex> lock(tunnelMutex_);
		tunnelOut_.clear();
	}
	server_ = ServerKind::None;
	serverIp_ = 0;
	key_ = 0;
	voicePort_ = 0;
	legacyReady_ = false;
	muted_ = false;
	recordingForced_ = false;
	allowVoiceActivation_ = false;
	showSpeakerList_ = true;
	showMicIcon_ = true;
	keys_.clear();
	pressed_.clear();
	vadKeyDown_ = false;
	names_.clear();
	transport_ = vb::transport::none;
	reportedTransport_ = 0xFF;
	expectedSequence_ = 0;
	pendingSequenced_.clear();
	sequenceGapSince_ = 0;
	udpConfirmed_ = false;
	tunnelActive_ = false;
	sending_ = false;
	serverPositions_ = false;
	lastServerPacket_ = 0;
}

void VoiceClient::releaseKeys()
{
	if (vadKeyDown_ && !keys_.empty())
	{
		uint8_t primary = *keys_.begin();
		if (!pressed_.count(primary))
		{
			sendControl(vb::ctl::releaseKey, &primary, 1);
		}
	}
	for (uint8_t key : pressed_)
	{
		sendControl(vb::ctl::releaseKey, &key, 1);
	}
	pressed_.clear();
	vadKeyDown_ = false;
}

bool VoiceClient::gameFocused() const
{
	const HWND foreground = GetForegroundWindow();
	if (!foreground)
	{
		return false;
	}
	if (window_)
	{
		return foreground == window_;
	}
	DWORD process = 0;
	GetWindowThreadProcessId(foreground, &process);
	return process == GetCurrentProcessId();
}

void VoiceClient::updateTransport(uint64_t t)
{
	if (server_ == ServerKind::None)
	{
		return;
	}
	const uint64_t last = lastServerPacket_;
	const bool alive = last != 0 && t - last < kUdpTimeoutMs;
	if (alive && !udpConfirmed_)
	{
		udpConfirmed_ = true;
		Log("voice UDP connected");
	}
	else if (!alive && udpConfirmed_)
	{
		udpConfirmed_ = false;
		Log("voice UDP lost");
	}

	const uint64_t interval = udpConfirmed_ ? keepAliveMs_ : (tunnelActive_ ? kTunnelProbeIntervalMs : kProbeIntervalMs);
	if (t - lastKeepAlive_ >= interval)
	{
		lastKeepAlive_ = t;
		sendKeepAlive();
	}

	uint8_t desired = vb::transport::none;
	if (server_ == ServerKind::SampVoice)
	{
		desired = legacyReady_ ? vb::transport::udp : vb::transport::none;
	}
	else if (forceTunnel_ && tunnelAllowed_)
	{
		desired = vb::transport::tunnel;
	}
	else if (udpConfirmed_)
	{
		desired = vb::transport::udp;
	}
	else if (tunnelAllowed_ && t - sessionStart_ >= kTunnelAfterMs)
	{
		desired = vb::transport::tunnel;
	}

	if (desired != transport_)
	{
		Log("voice transport: %s", desired == vb::transport::udp ? "udp" : desired == vb::transport::tunnel ? "tunnel" : "none");
		transport_ = desired;
		tunnelActive_ = desired == vb::transport::tunnel;
	}
}

void VoiceClient::updateKeys(uint64_t)
{
	Audio& audio = Audio::Get();
	const Settings& settings = GetSettings();
	const bool canTalk = transport_ != vb::transport::none && !muted_ && settings.micEnabled && audio.hasMicrophone();
	const bool focused = gameFocused() && !menuOpen_ && !samp::IsChatInputActive() && !samp::IsDialogActive() && !game::IsMenuActive();

	for (uint8_t key : keys_)
	{
		const bool down = focused && (GetAsyncKeyState(key) & 0x8000) != 0;
		const bool wasDown = pressed_.count(key) != 0;
		if (down && !wasDown)
		{
			pressed_.insert(key);
			if (!muted_ && !(vadKeyDown_ && key == *keys_.begin()))
			{
				sendControl(vb::ctl::pressKey, &key, 1);
			}
		}
		else if (!down && wasDown)
		{
			pressed_.erase(key);
			if (!muted_ && !(vadKeyDown_ && key == *keys_.begin()))
			{
				sendControl(vb::ctl::releaseKey, &key, 1);
			}
		}
	}

	// Voice activation drives the first activation key, so gamemodes that
	// attach speakers on OnPlayerActivationKeyPress keep working.
	const bool vadAllowed = allowVoiceActivation_ && settings.voiceActivation && !keys_.empty() && canTalk;
	audio.setVoiceActivation(vadAllowed);
	const bool vad = vadAllowed && !menuOpen_ && audio.voiceDetected();
	if (!keys_.empty())
	{
		uint8_t primary = *keys_.begin();
		if (vad && !vadKeyDown_)
		{
			vadKeyDown_ = true;
			if (!pressed_.count(primary))
			{
				sendControl(vb::ctl::pressKey, &primary, 1);
			}
		}
		else if (!vad && vadKeyDown_)
		{
			vadKeyDown_ = false;
			if (!pressed_.count(primary))
			{
				sendControl(vb::ctl::releaseKey, &primary, 1);
			}
		}
	}

	const bool transmit = canTalk && (recordingForced_ || !pressed_.empty() || vadKeyDown_);
	sending_ = transmit;
	audio.setTransmitting(transmit);
	// Beep only for the key: voice activation would beep on every word.
	const bool keyHeld = canTalk && !pressed_.empty();
	if (keyHeld != pushToTalkCue_)
	{
		pushToTalkCue_ = keyHeld;
		if (GetSettings().pushToTalkSound)
		{
			audio.playCue(keyHeld);
		}
	}
	// Not paused between key presses: a paused bass.dll recording delivers
	// stale audio on resume (old words at the start of the next sentence).
	audio.setRecordingActive(canTalk || audio.micTest() || menuOpen_);
}

void VoiceClient::sendStatus(bool force)
{
	if (server_ != ServerKind::VoiceBridge)
	{
		return;
	}
	const Settings& settings = GetSettings();
	const bool mic = Audio::Get().hasMicrophone();
	const bool micMuted = !settings.micEnabled;
	const bool soundMuted = !settings.soundEnabled;
	if (!force && transport_ == reportedTransport_ && mic == reportedMic_ && micMuted == reportedMicMuted_ && soundMuted == reportedSoundMuted_)
	{
		return;
	}
	reportedTransport_ = transport_;
	reportedMic_ = mic;
	reportedMicMuted_ = micMuted;
	reportedSoundMuted_ = soundMuted;
	const vb::VbClientStatus status { transport_, static_cast<uint8_t>(mic), static_cast<uint8_t>(micMuted), static_cast<uint8_t>(soundMuted) };
	sendControl(vb::ctl::vbClientStatus, &status, sizeof(status));
}

void VoiceClient::settingsChanged()
{
	sendStatus(false);
}

void VoiceClient::frame()
{
	if (!rak::Installed())
	{
		if (void** slot = samp::RakClientSlot(rak::ProxyVtable()))
		{
			rak::Install(slot, this);
			Audio::Get().setFrameSink([this](const uint8_t* data, std::size_t size, uint32_t packid) { onEncodedFrame(data, size, packid); });
		}
		else if (!proxyWarned_ && samp::NetGame())
		{
			proxyWarned_ = true;
			Log("waiting for the RakClient of this samp.dll build");
		}
	}

	std::vector<std::vector<uint8_t>> tunnel;
	{
		std::lock_guard<std::mutex> lock(tunnelMutex_);
		tunnel.swap(tunnelOut_);
	}
	for (const auto& packet : tunnel)
	{
		rak::Send(packet.data(), packet.size(), false);
	}

	const uint64_t t = now();
	skipSequenceGap(false);
	updateTransport(t);
	updateKeys(t);
	sendStatus(false);
	Audio::Get().update(game::GetListener(GetSettings().orientationFromCharacter), serverPositions_);

	notifications_.erase(std::remove_if(notifications_.begin(), notifications_.end(), [t](const Notification& n) { return n.expires <= t; }),
		notifications_.end());
}

void VoiceClient::shutdown()
{
	stopUdp();
	Audio::Get().setFrameSink(nullptr);
}

VoiceStatus VoiceClient::status() const
{
	VoiceStatus s;
	s.server = server_;
	s.transport = transport_;
	s.port = voicePort_;
	s.muted = muted_;
	s.recordingForced = recordingForced_;
	s.transmitting = sending_;
	s.canTalk = transport_ != vb::transport::none && !muted_;
	s.allowVoiceActivation = allowVoiceActivation_;
	s.showSpeakerList = showSpeakerList_;
	s.showMicIcon = showMicIcon_;
	s.keys.assign(keys_.begin(), keys_.end());
	return s;
}

std::string VoiceClient::playerName(uint16_t player) const
{
	// Voice Bridge servers send names; on SampVoice servers they come from
	// the game's player list.
	const auto it = names_.find(player);
	if (it != names_.end() && !it->second.empty())
	{
		return it->second;
	}
	std::string name;
	return samp::PlayerName(player, name) ? name : "Player " + std::to_string(player);
}

std::vector<Notification> VoiceClient::notifications()
{
	return notifications_;
}

void VoiceClient::notify(const std::string& text, uint32_t color, uint32_t durationMs, bool utf8)
{
	if (text.empty())
	{
		return;
	}
	notifications_.push_back({ text, color, now() + std::max<uint32_t>(durationMs, 500), utf8 });
	if (notifications_.size() > 5)
	{
		notifications_.erase(notifications_.begin());
	}
}
}
