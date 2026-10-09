/*
 *  Voice Bridge for open.mp and SA-MP
 *
 *  End-to-end tests of the voice server with real UDP sockets on loopback.
 */

#include "voice-server.hpp"
#include "log.hpp"
#include <iterator>
#include <fstream>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <thread>
#include <vector>

using namespace vbs;

namespace
{
int g_failures = 0;

#define CHECK(condition) \
	do \
	{ \
		if (!(condition)) \
		{ \
			std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
			++g_failures; \
		} \
	} while (0)

struct SentPacket
{
	uint16_t player;
	bool reliable;
	std::vector<uint8_t> data;
};

class FakeTransport final : public ITransport
{
public:
	bool ordered = true;
	std::vector<SentPacket> sent;

	bool sendPacket(uint16_t player, const uint8_t* data, std::size_t size, bool reliable) override
	{
		sent.push_back({ player, reliable, std::vector<uint8_t>(data, data + size) });
		return true;
	}

	uint32_t playerIp(uint16_t) override
	{
		return ParseIp("127.0.0.1");
	}

	bool orderedDelivery() const override
	{
		return ordered;
	}

	// Returns control messages of `type` sent to `player`, unwrapping the
	// sequenced envelope.
	std::vector<std::vector<uint8_t>> controls(uint16_t player, uint16_t type) const
	{
		std::vector<std::vector<uint8_t>> result;
		for (const auto& packet : sent)
		{
			if (packet.player != player || packet.data.size() < 5 || packet.data[0] != vb::kRakPacketId)
			{
				continue;
			}
			vb::ControlHeader header;
			std::memcpy(&header, packet.data.data() + 1, sizeof(header));
			std::size_t offset = 1 + sizeof(header);
			if (header.packet == vb::ctl::vbSequenced)
			{
				offset += sizeof(uint16_t);
				std::memcpy(&header, packet.data.data() + offset, sizeof(header));
				offset += sizeof(header);
			}
			if (header.packet == type)
			{
				result.emplace_back(packet.data.begin() + offset, packet.data.begin() + offset + header.length);
			}
		}
		return result;
	}
};

class FakeWorld final : public IWorld
{
public:
	std::map<uint16_t, Pose> players;

	bool playerPose(uint16_t player, Pose& out) override
	{
		const auto it = players.find(player);
		if (it == players.end())
		{
			return false;
		}
		out = it->second;
		return true;
	}
	bool vehiclePose(uint16_t, Pose&) override { return false; }
	bool objectPose(uint16_t, Pose&) override { return false; }
	bool playerName(uint16_t player, std::string& out) override
	{
		out = "Player_" + std::to_string(player);
		return true;
	}
};

class FakeEvents final : public IScriptEvents
{
public:
	std::vector<std::pair<uint16_t, uint8_t>> transports;
	std::vector<std::pair<uint16_t, bool>> talking;
	int handshakes = 0;
	int keys = 0;

	void onActivationKey(uint16_t, uint8_t, bool) override { ++keys; }
	void onHandshake(uint16_t, uint8_t, bool, bool) override { ++handshakes; }
	void onTransport(uint16_t player, uint8_t transport) override { transports.emplace_back(player, transport); }
	void onTalking(uint16_t player, bool value) override { talking.emplace_back(player, value); }
	void onClientStatus(uint16_t, bool, bool, bool) override { }
	std::vector<std::pair<uint16_t, uint8_t>> ignored;
	void onVoiceIgnored(uint16_t player, uint8_t reason) override { ignored.emplace_back(player, reason); }
};

struct Client
{
	UdpSocket socket;
	uint64_t server = 0;
	uint32_t key = 0;

	bool open(uint16_t port, const char* local = "127.0.0.1")
	{
		std::string error;
		if (!socket.open(local, 0, error))
		{
			std::printf("client socket: %s\n", error.c_str());
			return false;
		}
		server = PackAddress(ParseIp("127.0.0.1"), port);
		return true;
	}

	void send(uint32_t type, const std::vector<uint8_t>& payload = {}, uint32_t packid = 0)
	{
		std::vector<uint8_t> packet(sizeof(vb::VoiceHeader));
		vb::VoiceHeader header {};
		header.svrkey = key;
		header.packet = type;
		header.length = static_cast<uint16_t>(payload.size());
		header.packid = packid;
		vb::sealVoiceHeader(header);
		std::memcpy(packet.data(), &header, sizeof(header));
		packet.insert(packet.end(), payload.begin(), payload.end());
		socket.sendTo(server, packet.data(), static_cast<int>(packet.size()));
	}

	// Waits for a voice packet, skipping keep-alives.
	bool receiveVoice(vb::VoiceHeader& header, std::vector<uint8_t>& payload, int attempts = 8)
	{
		uint8_t buffer[2048];
		for (int i = 0; i < attempts; ++i)
		{
			uint32_t ip = 0;
			uint16_t port = 0;
			const int size = socket.receive(buffer, sizeof(buffer), ip, port);
			if (size < static_cast<int>(sizeof(header)))
			{
				continue;
			}
			std::memcpy(&header, buffer, sizeof(header));
			if (!vb::checkVoiceHeader(header) || header.packet != vb::voice::voicePacket)
			{
				continue;
			}
			payload.assign(buffer + sizeof(header), buffer + size);
			return true;
		}
		return false;
	}
};

std::vector<uint8_t> legacyJoin(bool micro)
{
	std::vector<uint8_t> data = { 0x12, 0x34, 0x56, 0x78, 0x01, 0x08, 'P', 'l', 'a', 'y', 'e', 'r' };
	const vb::ConnectPacket hello { vb::kLegacySignature, vb::kLegacyVersion, static_cast<uint8_t>(micro) };
	const auto* bytes = reinterpret_cast<const uint8_t*>(&hello);
	data.insert(data.end(), bytes, bytes + sizeof(hello));
	return data;
}

std::vector<uint8_t> extendedJoin()
{
	std::vector<uint8_t> data = legacyJoin(true);
	const vb::VbHello hello { vb::kVbMagic, vb::kVbProtocol, vb::helloflag::hasMicro, 7, 0 };
	const auto* bytes = reinterpret_cast<const uint8_t*>(&hello);
	data.insert(data.end(), bytes, bytes + sizeof(hello));
	return data;
}

void pump(int milliseconds)
{
	const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
	while (std::chrono::steady_clock::now() < end)
	{
		VoiceServer::Get().tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

void testProtocol()
{
	const char* check = "123456789";
	CHECK(vb::crc32c(reinterpret_cast<const uint8_t*>(check), 9) == 0xE3069283u);
	CHECK(sizeof(vb::VoiceHeader) == 24);
	CHECK(sizeof(vb::ConnectPacket) == 6);
	CHECK(sizeof(vb::ServerInfoPacket) == 6);
	CHECK(sizeof(vb::PluginInitPacket) == 5);
	CHECK(sizeof(vb::CreateLPStreamPacket) == 24);
	CHECK(sizeof(vb::CreateEffectPacket) == 16);
	CHECK(sizeof(vb::EchoParams) == 20);
	CHECK(sizeof(vb::I3dl2reverbParams) == 48);
}

void testVoiceRouting()
{
	FakeTransport transport;
	FakeWorld world;
	FakeEvents events;
	Config config;
	config.logFile = "off"; // no voice log file from the tests
	config.port = 0;
	config.gamePort = 65535; // makes the server pick a free port
	config.bind = "127.0.0.1";
	VoiceServer& server = VoiceServer::Get();
	CHECK(server.start(config, &transport, &world, &events));
	const uint16_t port = server.port();
	CHECK(port != 0);

	// Player 0: SampVoice client.  Player 1: Voice Bridge client.
	const auto legacy = legacyJoin(true);
	server.onClientJoin(0, legacy.data(), legacy.size());
	const auto extended = extendedJoin();
	server.onClientJoin(1, extended.data(), extended.size());
	// Player 2: vanilla client.
	const std::vector<uint8_t> vanilla = { 1, 2, 3, 4, 5, 6, 7, 8 };
	server.onClientJoin(2, vanilla.data(), vanilla.size());

	CHECK(server.hasPlugin(0) && !server.isExtended(0));
	CHECK(server.hasPlugin(1) && server.isExtended(1));
	CHECK(!server.hasPlugin(2));
	// A nickname containing the Voice Bridge magic must not look like a client.
	std::vector<uint8_t> tricky = { 0x12, 0x34, 0x56, 0x78, 0x01, 0x08, 'V', 'B', 'G', '1', 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	server.onClientJoin(5, tricky.data(), tricky.size());
	CHECK(!server.hasPlugin(5));
	const auto legacyTricky = legacyJoin(true);
	tricky.insert(tricky.end(), legacyTricky.begin() + 12, legacyTricky.end());
	server.onClientJoin(6, tricky.data(), tricky.size());
	CHECK(server.hasPlugin(6) && !server.isExtended(6));
	server.onPlayerDisconnect(5);
	server.onPlayerDisconnect(6);
	CHECK(server.clientVersion(0) == vb::kLegacyVersion);
	CHECK(events.handshakes == 3);

	const auto info0 = transport.controls(0, vb::ctl::serverInfo);
	CHECK(info0.size() == 1);
	vb::ServerInfoPacket legacyInfo {};
	if (!info0.empty())
	{
		std::memcpy(&legacyInfo, info0[0].data(), sizeof(legacyInfo));
	}
	CHECK(legacyInfo.serverPort == port && legacyInfo.serverKey != 0);

	const auto info1 = transport.controls(1, vb::ctl::vbServerInfo);
	CHECK(info1.size() == 1);
	vb::VbServerInfo extendedInfo {};
	if (!info1.empty())
	{
		std::memcpy(&extendedInfo, info1[0].data(), sizeof(extendedInfo));
	}
	CHECK(extendedInfo.port == port && extendedInfo.key != 0 && extendedInfo.frameMs == 100);
	CHECK(transport.controls(1, vb::ctl::vbSpeakerName).size() >= 2);

	// Stream: player 0 speaks, player 1 listens.
	const uint32_t stream = server.createStream(StreamType::Global, 0.f, kInfiniteListeners, vb::Vec3 {}, vb::kNonePlayer, 0xFF00FF00, "Global");
	CHECK(stream != 0);
	CHECK(server.attachSpeaker(stream, 0));
	CHECK(server.attachListener(stream, 1));
	CHECK(!server.attachListener(stream, 2)); // no plugin
	CHECK(transport.controls(1, vb::ctl::createGStream).size() == 1);
	CHECK(server.addKey(0, 0x42));

	Client client0;
	Client client1;
	CHECK(client0.open(port));
	CHECK(client1.open(port));
	client0.key = legacyInfo.serverKey;
	client1.key = extendedInfo.key;
	client0.send(vb::voice::keepAlive);
	client1.send(vb::voice::keepAlive);
	pump(150);

	CHECK(transport.controls(0, vb::ctl::pluginInit).size() == 1);
	CHECK(server.transportOf(0) == vb::transport::udp);
	CHECK(server.transportOf(1) == vb::transport::udp);

	// Voice from player 0 reaches player 1 over UDP.
	const std::vector<uint8_t> opus = { 0xF8, 0xFF, 0xFE, 0x01, 0x02, 0x03 };
	client0.send(vb::voice::voicePacket, opus, 5);
	vb::VoiceHeader header {};
	std::vector<uint8_t> payload;
	CHECK(client1.receiveVoice(header, payload));
	CHECK(header.stream == stream && header.sender == 0 && header.packid == 5 && payload == opus);
	pump(50);
	CHECK(server.isTalking(0));

	// Without a key or recording the speaker is ignored, and the script is told why.
	CHECK(server.removeKey(0, 0x42));
	client0.send(vb::voice::voicePacket, opus, 6);
	CHECK(!client1.receiveVoice(header, payload, 3));
	pump(50);
	CHECK(events.ignored.size() == 1 && events.ignored[0].first == 0
		&& events.ignored[0].second == static_cast<uint8_t>(VoiceIgnored::NoKey));
	CHECK(server.startRecord(0));
	client0.send(vb::voice::voicePacket, opus, 7);
	CHECK(client1.receiveVoice(header, payload));

	// Server side block.
	CHECK(server.setBlocked(1, 0, true));
	client0.send(vb::voice::voicePacket, opus, 8);
	CHECK(!client1.receiveVoice(header, payload, 3));
	CHECK(server.setBlocked(1, 0, false));

	// Tunnel: player 1 reports blocked UDP, voice arrives through RakNet.
	const vb::VbClientStatus status { vb::transport::tunnel, 1, 0, 0 };
	std::vector<uint8_t> control(sizeof(vb::ControlHeader) + sizeof(status));
	const vb::ControlHeader statusHeader { vb::ctl::vbClientStatus, sizeof(status) };
	std::memcpy(control.data(), &statusHeader, sizeof(statusHeader));
	std::memcpy(control.data() + sizeof(statusHeader), &status, sizeof(status));
	server.onControlPacket(1, control.data(), control.size());
	CHECK(server.transportOf(1) == vb::transport::tunnel);
	transport.sent.clear();
	client0.send(vb::voice::voicePacket, opus, 9);
	pump(100);
	const auto tunneled = transport.controls(1, vb::ctl::vbVoiceDown);
	CHECK(tunneled.size() == 1);
	if (!tunneled.empty())
	{
		vb::VoiceHeader down {};
		std::memcpy(&down, tunneled[0].data(), sizeof(down));
		CHECK(vb::checkVoiceHeader(down) && down.sender == 0 && down.stream == stream);
	}

	// Upstream tunnel from player 1 to player 0.
	CHECK(server.attachSpeaker(stream, 1));
	CHECK(server.attachListener(stream, 0));
	CHECK(server.addKey(1, 0x42));
	std::vector<uint8_t> up(sizeof(vb::ControlHeader) + sizeof(uint32_t) + opus.size());
	const vb::ControlHeader upHeader { vb::ctl::vbVoiceUp, static_cast<uint16_t>(sizeof(uint32_t) + opus.size()) };
	const uint32_t packid = 42;
	std::memcpy(up.data(), &upHeader, sizeof(upHeader));
	std::memcpy(up.data() + sizeof(upHeader), &packid, sizeof(packid));
	std::memcpy(up.data() + sizeof(upHeader) + sizeof(packid), opus.data(), opus.size());
	server.onControlPacket(1, up.data(), up.size());
	CHECK(client0.receiveVoice(header, payload));
	CHECK(header.sender == 1 && header.packid == 42);

	// Effects and parameters are replayed to new listeners.
	const vb::ReverbParams reverb { 0.f, -4.f, 1000.f, 0.5f };
	const uint32_t effect = server.createEffect(vb::effect::reverb, 0, &reverb, sizeof(reverb));
	CHECK(server.attachEffect(effect, stream));
	CHECK(server.setParameter(stream, vb::param::volume, 0.5f));
	CHECK(server.parameter(stream, vb::param::volume) == 0.5f);
	CHECK(server.detachListener(stream, 1));
	transport.sent.clear();
	CHECK(server.attachListener(stream, 1));
	CHECK(transport.controls(1, vb::ctl::createEffect).size() == 1);
	CHECK(transport.controls(1, vb::ctl::setStreamParameter).size() == 1);

	// Script ownership.
	int owner = 0;
	const uint32_t owned = server.createStream(StreamType::Global, 0.f, kInfiniteListeners, vb::Vec3 {}, vb::kNonePlayer, 0, "owned", &owner);
	server.releaseOwner(&owner);
	CHECK(!server.isValidStream(owned));
	CHECK(server.isValidStream(stream));

	// Disconnect cleans everything up.
	server.onPlayerDisconnect(0);
	CHECK(!server.hasPlugin(0));
	CHECK(!server.hasSpeaker(stream, 0));
	CHECK(!server.hasListener(stream, 0));
	CHECK(server.deleteStream(stream));
	server.stop();
}

void testDynamicStreams()
{
	FakeTransport transport;
	FakeWorld world;
	FakeEvents events;
	Config config;
	config.logFile = "dynamic-streams-test.log"; // checked below, then removed
	config.debug = true; // the details are only logged in debug mode
	std::remove(config.logFile.c_str());
	config.gamePort = 65535;
	config.bind = "127.0.0.1";
	config.streamTickMs = 20;
	config.positionRateMs = 20;
	VoiceServer& server = VoiceServer::Get();
	CHECK(server.start(config, &transport, &world, &events));

	const auto join = legacyJoin(true);
	for (uint16_t id = 0; id < 4; ++id)
	{
		server.onClientJoin(id, join.data(), join.size());
	}
	world.players[0] = Pose { { 0.f, 0.f, 0.f }, 0, 0 };
	world.players[1] = Pose { { 5.f, 0.f, 0.f }, 0, 0 };
	world.players[2] = Pose { { 50.f, 0.f, 0.f }, 0, 0 };
	world.players[3] = Pose { { 3.f, 0.f, 0.f }, 7, 0 }; // other virtual world

	const uint32_t stream = server.createStream(StreamType::DynamicPlayer, 20.f, kInfiniteListeners, vb::Vec3 {}, 0, 0, "Local");
	CHECK(!server.attachListener(stream, 1)); // managed automatically
	pump(60);
	CHECK(server.hasListener(stream, 1));
	CHECK(!server.hasListener(stream, 0)); // the source does not hear itself
	CHECK(!server.hasListener(stream, 2)); // too far
	CHECK(!server.hasListener(stream, 3)); // other world

	world.players[1].position.x = 30.f;
	world.players[2].position.x = 10.f;
	pump(60);
	CHECK(!server.hasListener(stream, 1));
	CHECK(server.hasListener(stream, 2));
	CHECK(transport.controls(1, vb::ctl::deleteStream).size() == 1);

	// SampVoice places a "stream at player" at the ped matrix, which GTA
	// leaves stale inside vehicles: SampVoice listeners get a point stream
	// that the server moves with the speaker instead.
	CHECK(transport.controls(2, vb::ctl::createLStreamAtPlayer).empty());
	const auto points = transport.controls(2, vb::ctl::createLPStream);
	CHECK(points.size() == 1);
	if (!points.empty())
	{
		vb::CreateLPStreamPacket point {};
		std::memcpy(&point, points[0].data(), sizeof(point));
		CHECK(point.stream == stream && point.position.x == 0.f && point.distance == 20.f);
	}
	world.players[0].position = { 4.f, 1.f, 0.f }; // the speaker drives away a bit
	pump(80);
	const auto moves = transport.controls(2, vb::ctl::updateLPStreamPosition);
	CHECK(!moves.empty());
	if (!moves.empty())
	{
		vb::UpdateLPStreamPositionPacket move {};
		std::memcpy(&move, moves.back().data(), sizeof(move));
		CHECK(move.stream == stream && move.position.x == 4.f && move.position.y == 1.f);
	}
	const std::size_t sent = moves.size();
	pump(80); // standing still: no more updates
	CHECK(transport.controls(2, vb::ctl::updateLPStreamPosition).size() == sent);

	// Vehicle diagnostics: entering a vehicle is logged with what the player
	// hears, and the console report describes the same.
	world.players[2].vehicle = 412;
	pump(1100);
	const auto report = server.statusReport(2);
	CHECK(report.size() == 1 && report[0].find("in vehicle 412") != std::string::npos
		&& report[0].find("player 0") != std::string::npos && report[0].find("voice sent TO him") != std::string::npos);

	// Inside a vehicle the SampVoice client mutes 3D voice (it places itself
	// at the stale ped matrix): the stream is recreated without 3D, and as a
	// 3D point again when the player gets out.
	const auto flatStreams = transport.controls(2, vb::ctl::createGStream);
	CHECK(!flatStreams.empty());
	if (!flatStreams.empty())
	{
		vb::CreateGStreamPacket flatStream {};
		std::memcpy(&flatStream, flatStreams.back().data(), sizeof(flatStream));
		CHECK(flatStream.stream == stream);
	}
	CHECK(server.hasListener(stream, 2));
	const std::size_t pointsBefore = transport.controls(2, vb::ctl::createLPStream).size();
	world.players[2].vehicle = -1;
	pump(80);
	CHECK(transport.controls(2, vb::ctl::createLPStream).size() == pointsBefore + 1);

	// The same when it is the speaker who is in a vehicle.
	const std::size_t flatBefore = transport.controls(2, vb::ctl::createGStream).size();
	world.players[0].vehicle = 77;
	pump(80);
	CHECK(transport.controls(2, vb::ctl::createGStream).size() == flatBefore + 1);
	CHECK(server.hasListener(stream, 2));
	world.players[0].vehicle = -1;
	pump(80);
	CHECK(transport.controls(2, vb::ctl::createLPStream).size() == pointsBefore + 2);

	CHECK(server.setStreamMaxListeners(stream, 1));
	world.players[1].position.x = 2.f;
	pump(60);
	CHECK(server.listenerCount(stream) == 1);

	server.stop();
	{
		// The voice log explains every listener change (who, where, why).
		std::ifstream log(config.logFile);
		const std::string text((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
		CHECK(text.find("starts hearing - player 1 at (5.0, 0.0, 0.0) world 0 interior 0 on foot, 5.0 m") != std::string::npos);
		CHECK(text.find("stops hearing - player 1 at (30.0, 0.0, 0.0)") != std::string::npos);
		CHECK(text.find("ENTERED a vehicle - player 2 (SampVoice") != std::string::npos);
	}
	{
		// Without voice_debug the details stay out of the file; info stays.
		vbs::LogSetDebug(false);
		vbs::LogDebug("detail line that must not be written");
		vbs::LogInfo("info line that must be written");
		vbs::LogSetFile("");
		std::ifstream log(config.logFile);
		const std::string text((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
		CHECK(text.find("detail line that must not be written") == std::string::npos);
		CHECK(text.find("info line that must be written") != std::string::npos);
	}
	std::remove(config.logFile.c_str());
}

void testSequencedEnvelope()
{
	FakeTransport transport;
	transport.ordered = false;
	FakeWorld world;
	FakeEvents events;
	Config config;
	config.logFile = "off"; // no voice log file from the tests
	config.gamePort = 65535;
	config.bind = "127.0.0.1";
	VoiceServer& server = VoiceServer::Get();
	CHECK(server.start(config, &transport, &world, &events));
	const auto extended = extendedJoin();
	server.onClientJoin(3, extended.data(), extended.size());
	uint16_t expected = 0;
	bool ordered = true;
	for (const auto& packet : transport.sent)
	{
		vb::ControlHeader header;
		std::memcpy(&header, packet.data.data() + 1, sizeof(header));
		if (header.packet != vb::ctl::vbSequenced)
		{
			ordered = false;
			continue;
		}
		uint16_t sequence;
		std::memcpy(&sequence, packet.data.data() + 1 + sizeof(header), sizeof(sequence));
		ordered = ordered && sequence == expected++;
	}
	CHECK(ordered && expected > 1);
	CHECK(transport.controls(3, vb::ctl::vbServerInfo).size() == 1);
	server.stop();
}
}

void testSecurityAndClientTypes()
{
	FakeTransport transport;
	FakeWorld world;
	FakeEvents events;
	Config config;
	config.logFile = "off"; // no voice log file from the tests
	config.gamePort = 65535;
	config.bind = "127.0.0.1";
	VoiceServer& server = VoiceServer::Get();
	CHECK(server.start(config, &transport, &world, &events));
	const uint16_t port = server.port();

	// Client types and allowance.
	const auto legacy = legacyJoin(true);
	const auto extended = extendedJoin();
	server.onClientJoin(0, legacy.data(), legacy.size());
	server.onClientJoin(1, extended.data(), extended.size());
	CHECK(server.clientType(0) == ClientType::SampVoice);
	CHECK(server.clientType(1) == ClientType::VoiceBridge);
	server.setClientTypeAllowed(ClientType::SampVoice, false);
	server.onClientJoin(2, legacy.data(), legacy.size());
	CHECK(server.clientType(2) == ClientType::SampVoice && !server.hasPlugin(2));
	CHECK(transport.controls(2, vb::ctl::serverInfo).empty());
	CHECK(server.clientVersion(2) == vb::kLegacyVersion);
	server.setClientTypeAllowed(ClientType::SampVoice, true);
	CHECK(server.clientBuild(0) == 0 && server.clientBuild(1) != 0);
	server.setClientTypeAllowed(ClientType::VoiceBridge, false);
	server.onClientJoin(3, extended.data(), extended.size());
	CHECK(!server.hasPlugin(3) && server.clientBuild(3) == server.clientBuild(1));
	server.setClientTypeAllowed(ClientType::VoiceBridge, true);
	server.onPlayerDisconnect(3);

	vb::ServerInfoPacket info {};
	const auto infos = transport.controls(0, vb::ctl::serverInfo);
	CHECK(!infos.empty());
	if (!infos.empty())
	{
		std::memcpy(&info, infos[0].data(), sizeof(info));
	}
	vb::VbServerInfo extendedInfo {};
	const auto extendedInfos = transport.controls(1, vb::ctl::vbServerInfo);
	if (!extendedInfos.empty())
	{
		std::memcpy(&extendedInfo, extendedInfos[0].data(), sizeof(extendedInfo));
	}
	// Hidden speaker list: no names are sent to that player.
	transport.sent.clear();
	CHECK(server.setPlayerSpeakerList(1, false));
	CHECK(transport.controls(1, vb::ctl::vbConfig).size() == 1);
	server.onPlayerConnect(3);
	CHECK(transport.controls(1, vb::ctl::vbSpeakerName).empty());

	const uint32_t stream = server.createStream(StreamType::Global, 0.f, kInfiniteListeners, vb::Vec3 {}, vb::kNonePlayer, 0, "g");
	CHECK(server.attachSpeaker(stream, 0) && server.attachListener(stream, 1) && server.addKey(0, 0x42));

	Client owner;
	Client listener;
	Client attacker;
	CHECK(owner.open(port));
	CHECK(listener.open(port));
	const bool secondLoopback = attacker.open(port, "127.0.0.2");
	owner.key = info.serverKey;
	listener.key = extendedInfo.key;
	owner.send(vb::voice::keepAlive);
	listener.send(vb::voice::keepAlive);
	pump(150);

	const std::vector<uint8_t> opus = { 0xF8, 0xFF, 0xFE };
	vb::VoiceHeader header {};
	std::vector<uint8_t> payload;
	if (secondLoopback)
	{
		// A stolen/guessed key from another IP cannot inject voice.
		attacker.key = info.serverKey;
		attacker.send(vb::voice::voicePacket, opus, 1);
		CHECK(!listener.receiveVoice(header, payload, 3));
		// Guessing keys gets the source blocked, even with a valid key later.
		for (uint32_t i = 0; i < 40; ++i)
		{
			attacker.key = 0x1000 + i;
			attacker.send(vb::voice::keepAlive);
		}
		pump(100);
	}
	else
	{
		std::printf("note: 127.0.0.2 unavailable, IP lock checks skipped\n");
	}
	// The real owner still talks.
	owner.send(vb::voice::voicePacket, opus, 2);
	CHECK(listener.receiveVoice(header, payload));
	CHECK(header.sender == 0 && header.packid == 2);
	server.stop();
}

int main()
{
	testProtocol();
	testVoiceRouting();
	testDynamicStreams();
	testSequencedEnvelope();
	testSecurityAndClientTypes();
	LogFlush();
	if (g_failures)
	{
		std::printf("%d check(s) failed\n", g_failures);
		return EXIT_FAILURE;
	}
	std::printf("all server tests passed\n");
	return EXIT_SUCCESS;
}
