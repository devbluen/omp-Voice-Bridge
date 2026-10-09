/*
 *  Voice Bridge client
 *
 *  Talks to both servers:
 *   - SampVoice servers: protocol 11, UDP only, exactly like sampvoice.asi.
 *   - Voice Bridge servers: extended protocol with ordered control messages,
 *     server-side positions and a fallback through the game connection when
 *     UDP is blocked.
 *
 *  Threads: the game thread drives everything (RakNet callbacks and the
 *  per-frame update), a UDP thread receives voice and the BASS recording
 *  thread sends it.
 */

#pragma once

#include "game.hpp"
#include "rakclient.hpp"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace vbc
{
enum class ServerKind
{
	None,
	SampVoice,
	VoiceBridge,
};

struct Notification
{
	std::string text;
	uint32_t color = 0xFFFFFFFF;
	uint64_t expires = 0;
	bool utf8 = false; // server texts are Windows-1252
};

struct VoiceStatus
{
	ServerKind server = ServerKind::None;
	uint8_t transport = 0; // vb::transport
	uint16_t port = 0;
	bool muted = false;
	bool recordingForced = false;
	bool transmitting = false;
	bool canTalk = false;
	bool allowVoiceActivation = false;
	bool showSpeakerList = true;
	bool showMicIcon = true;
	std::vector<uint8_t> keys;
};

class VoiceClient final : public rak::Events
{
public:
	static VoiceClient& Get();

	void setWindow(HWND window) noexcept { window_ = window; }
	void setMenuOpen(bool open) noexcept { menuOpen_ = open; }

	// Game thread, once per frame.
	void frame();
	void shutdown();

	VoiceStatus status() const;
	std::string playerName(uint16_t player) const;
	std::vector<Notification> notifications();
	void notify(const std::string& text, uint32_t color = 0xFFFFFFFF, uint32_t durationMs = 4000, bool utf8 = false);
	void settingsChanged();

	// rak::Events
	void onConnect(const char* host, unsigned short port) override;
	void onConnectionAccepted(uint32_t serverIp, uint16_t serverPort) override;
	void onDisconnect() override;
	std::vector<uint8_t> joinPayload() override;
	void onPacket(const uint8_t* data, std::size_t size) override;

private:
	VoiceClient() = default;

	void resetSession();
	void handleControl(uint16_t type, const uint8_t* payload, std::size_t size);
	void sendControl(uint16_t type, const void* payload, std::size_t size, bool reliable = true);
	void startUdp(const std::string& host);
	void stopUdp();
	void udpLoop();
	void handleDatagram(const uint8_t* data, int size);
	void sendKeepAlive();
	void updateTransport(uint64_t now);
	void updateKeys(uint64_t now);
	void sendStatus(bool force);
	void releaseKeys();
	void skipSequenceGap(bool force);
	void onEncodedFrame(const uint8_t* data, std::size_t size, uint32_t packid);
	bool gameFocused() const;

	HWND window_ = nullptr;
	bool menuOpen_ = false;
	bool proxyWarned_ = false;

	// Session (game thread)
	ServerKind server_ = ServerKind::None;
	std::string host_;
	uint16_t gamePort_ = 0;
	uint32_t serverIp_ = 0;
	uint32_t key_ = 0;
	uint16_t voicePort_ = 0;
	uint32_t bitrate_ = 24000;
	uint32_t frameMs_ = 100;
	uint32_t keepAliveMs_ = 2000;
	bool tunnelAllowed_ = false;
	bool forceTunnel_ = false;
	bool legacyReady_ = false;
	bool muted_ = false;
	bool recordingForced_ = false;
	bool allowVoiceActivation_ = false;
	bool showSpeakerList_ = true;
	bool showMicIcon_ = true;
	std::set<uint8_t> keys_;
	std::set<uint8_t> pressed_;
	bool vadKeyDown_ = false;
	bool pushToTalkCue_ = false;
	bool wasInVehicle_ = false;
	uint64_t lastVehicleLog_ = 0;
	std::atomic<uint32_t> voicePackets_ { 0 }; // received since start (diagnostics)
	void logVehicleDiagnostics(const game::Listener& listener, uint64_t t);
	uint64_t sessionStart_ = 0;
	uint64_t lastKeepAlive_ = 0;
	bool udpConfirmed_ = false;
	uint8_t transport_ = 0;
	uint8_t reportedTransport_ = 0xFF;
	bool reportedMicMuted_ = false;
	bool reportedSoundMuted_ = false;
	bool reportedMic_ = false;
	uint16_t expectedSequence_ = 0;
	std::map<uint16_t, std::vector<uint8_t>> pendingSequenced_;
	uint64_t sequenceGapSince_ = 0;
	std::map<uint16_t, std::string> names_;
	std::vector<Notification> notifications_;

	// Shared with the UDP and recording threads
	mutable std::mutex mutex_;
	uintptr_t socket_ = ~uintptr_t(0);
	std::atomic<uint64_t> serverAddress_ { 0 };
	std::atomic<uint32_t> udpKey_ { 0 };
	std::atomic<uint64_t> lastServerPacket_ { 0 };
	std::atomic<bool> udpRunning_ { false };
	std::atomic<bool> resolving_ { false };
	std::atomic<uint32_t> udpGeneration_ { 0 };
	std::atomic<bool> tunnelActive_ { false };
	std::atomic<bool> sending_ { false };
	std::atomic<bool> serverPositions_ { false };
	std::thread udpThread_;
	std::mutex tunnelMutex_;
	std::vector<std::vector<uint8_t>> tunnelOut_;
};
}
