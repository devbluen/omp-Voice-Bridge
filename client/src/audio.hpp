/*
 *  Voice Bridge client
 *
 *  Playback: one BASS push stream per (stream, speaker) pair, fed by an Opus
 *  decoder.  Positioning is computed by the mod (volume + panning), so it
 *  does not depend on how SA-MP initialised BASS.
 *
 *  Capture: BASS recording, Opus encoding, gain, noise gate and voice
 *  activity detection.  Encoded frames are handed to a sink on the BASS
 *  recording thread.
 */

#pragma once

#include "bass.hpp"
#include "game.hpp"
#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct OpusDecoder;
struct OpusEncoder;

namespace vbc
{
enum class StreamKind
{
	Global,
	Point,
	Player,
	Vehicle,
	Object,
};

struct SpeakerInfo
{
	uint16_t player = 0;
	uint32_t color = 0;
	std::string stream;
	float level = 0.f;
};

class Audio
{
public:
	static Audio& Get();

	bool init(HWND window);
	void shutdown();
	bool ready() const noexcept { return ready_; }

	// Streams (server thread of the game = main thread)
	void createStream(uint32_t id, StreamKind kind, uint32_t color, const std::string& name, float distance, const game::Vec3& position, uint16_t target);
	void deleteStream(uint32_t id);
	void clearStreams();
	void setStreamDistance(uint32_t id, float distance);
	void setStreamPosition(uint32_t id, const game::Vec3& position);
	void setStreamFlags(uint32_t id, uint32_t flags);
	void setSourcePosition(uint32_t id, const game::Vec3& position);
	void setParameter(uint32_t id, uint32_t parameter, float value);
	void slideParameter(uint32_t id, uint32_t parameter, float from, float to, uint32_t timeMs);
	void createEffect(uint32_t stream, uint32_t effect, uint32_t number, int32_t priority, const uint8_t* params, std::size_t size);
	void deleteEffect(uint32_t stream, uint32_t effect);

	// Any thread
	void pushVoice(uint32_t stream, uint16_t sender, uint32_t packid, const uint8_t* data, std::size_t size);

	// Main thread, once per frame
	void update(const game::Listener& listener, bool serverPositions);
	std::vector<SpeakerInfo> speakers();

	void setPlayerVolume(uint16_t player, float volume);
	float playerVolume(uint16_t player) const;
	void setServerPlayerVolume(uint16_t player, float volume);
	void setPlayerMuted(uint16_t player, bool muted);
	bool playerMuted(uint16_t player) const;
	void clearPlayerSettings();

	// Capture
	std::vector<std::string> microphones();
	bool openMicrophone(const std::string& name);
	void closeMicrophone();
	bool hasMicrophone() const noexcept { return recordHandle_ != 0; }
	std::string microphoneName() const;
	void configureEncoder(uint32_t bitrate, uint32_t frameMs);
	uint32_t frameMs() const noexcept { return frameMs_; }
	// The recording runs for the whole voice session: bass.dll 2.4.7 keeps
	// capturing while a recording is paused and delivers that backlog on
	// resume, which sent old speech at the start of every push-to-talk.
	void setRecordingActive(bool active);
	void setTransmitting(bool transmitting);
	bool transmitting() const noexcept { return transmitting_; }
	void setMicTest(bool enabled);
	bool micTest() const noexcept { return micTest_; }
	void setVoiceActivation(bool enabled);
	bool voiceDetected() const noexcept { return voiceDetected_; }
	float micLevel() const noexcept { return micLevel_; }
	// Attribute changes BASS refused (used by the smoke test).
	int attributeErrors() const noexcept { return attributeErrors_; }
	// Late packets from an earlier utterance that were not played.
	int stalePackets() const noexcept { return stalePackets_; }
	// BASS handles of the voice channels (tests).
	std::vector<DWORD> channelHandles();
	// Microphone samples accepted since start (tests).
	uint64_t capturedSamples() const noexcept { return capturedSamples_; }

	using FrameSink = std::function<void(const uint8_t* data, std::size_t size, uint32_t packid)>;
	void setFrameSink(FrameSink sink);
	// Short local beep when push-to-talk starts/stops.
	void playCue(bool start);

private:
	struct EffectData
	{
		uint32_t number = 0;
		int32_t priority = 0;
		std::vector<uint8_t> params;
	};

	struct Parameter
	{
		float from = 0.f;
		float to = 0.f;
		uint64_t start = 0;
		uint32_t duration = 0;
		float value(uint64_t now) const;
	};

	// Per speaker state of the 3D renderer.
	struct Spatial
	{
		static constexpr int kDelaySize = 64;
		float delay[kDelaySize] {};
		int delayPos = 0;
		float nearState = 0.f;
		float farState = 0.f;
		float farGain = 1.f;
		float itd = 0.f;
		float roomSend = 0.f;
		float lowpass = 0.f;
		std::vector<float> comb[4];
		int combPos[4] {};
		float combFilter[4] {};
		std::vector<float> allpass[2];
		int allpassPos[2] {};
		Spatial();
	};

	// Where the speaker is, written by update() and read by the playback DSP
	// so the direction follows the speaker while they keep talking.
	struct Placement
	{
		std::atomic<float> side { 0.f };
		std::atomic<float> ratio { 0.f };
		std::atomic<float> brightness { 1.f };
		std::atomic<bool> positioned { false };
	};

	struct Channel
	{
		uint16_t speaker = 0;
		bass::HSTREAM handle = 0;
		OpusDecoder* decoder = nullptr;
		std::map<uint32_t, bass::HFX> effects;
		uint32_t expected = 0;
		// Start of the current utterance: packet ids restart at 0 on every
		// one, so a late packet from the previous utterance looks "new".
		uint64_t utteranceStart = 0;
		uint32_t utteranceFirst = 0;
		bool initialized = false;
		bool playing = false;
		DWORD queued = 0; // bytes pushed since the channel (re)started
		uint64_t lastPacket = 0;
		float level = 0.f;
		float appliedFrequency = -1.f;
		std::unique_ptr<Spatial> spatial;
		Placement placement;
		bool dsp = false; // spatialized at playback (false: old bass.dll, done when pushed)
	};

	struct Stream
	{
		uint32_t id = 0;
		StreamKind kind = StreamKind::Global;
		uint32_t color = 0;
		std::string name;
		float distance = 0.f;
		game::Vec3 position;
		uint16_t target = 0xFFFF;
		uint32_t flags = 0;
		bool sourceKnown = false;
		uint64_t sourceTime = 0;
		std::map<uint32_t, Parameter> parameters;
		std::map<uint32_t, EffectData> effects;
		std::vector<std::unique_ptr<Channel>> channels;
		float gain = 1.f;
		float pan = 0.f;
		// 1 = full bandwidth, 0 = strongly muffled (far away or behind).
		float brightness = 1.f;
		// Direction for the renderer: -1 left .. 1 right, facing < 0 = behind.
		float side = 0.f;
		float facing = 1.f;
		float ratio = 0.f; // distance / audible range
		bool positioned = false;
	};

	float playerVolumeLocked(uint16_t player) const;
	Channel* channelFor(Stream& stream, uint16_t speaker);
	void freeChannel(Channel& channel);
	void applyEffect(Channel& channel, uint32_t id, const EffectData& effect);
	bool resolveSource(Stream& stream, bool serverPositions, game::Vec3& out);
	void startRecording();
	static BOOL CALLBACK recordProc(DWORD handle, const void* buffer, DWORD length, void* user);
	void onCapture(const int16_t* samples, std::size_t count);
	static void spatialize(Channel& channel, int16_t* stereo, int count);
	static void CALLBACK dspProc(DWORD handle, DWORD channel, void* buffer, DWORD length, void* user);
	bool duplicate(uint16_t sender, uint32_t packid, const uint8_t* data, std::size_t size, uint64_t now);

	struct RecentPacket
	{
		uint16_t sender = 0xFFFF;
		uint32_t hash = 0;
		uint64_t time = 0;
	};
	std::array<RecentPacket, 256> recent_ {};
	// Stream each speaker is currently heard through (one at a time).
	struct SpeakerRoute
	{
		uint32_t stream = 0;
		uint64_t time = 0;
	};
	std::map<uint16_t, SpeakerRoute> routes_;
	std::size_t recentPos_ = 0;
	bass::HSTREAM cueStream_ = 0;

	bool ready_ = false;
	std::atomic<int> attributeErrors_ { 0 };
	std::atomic<int> stalePackets_ { 0 };
	bool firstPlayLogged_ = false;
	DWORD loggedStreamVolume_ = 0xFFFFFFFF;
	mutable std::mutex mutex_;
	std::map<uint32_t, Stream> streams_;
	std::map<uint16_t, float> playerVolume_;
	std::map<uint16_t, float> serverVolume_;
	std::bitset<1024> muted_;

	// Capture
	std::mutex captureMutex_;
	DWORD recordHandle_ = 0;
	bool recordActive_ = false;
	int recordDevice_ = -1;
	std::string recordName_;
	bass::HSTREAM testStream_ = 0;
	OpusEncoder* encoder_ = nullptr;
	uint32_t bitrate_ = 24000;
	uint32_t frameMs_ = 100;
	std::vector<int16_t> pending_;
	std::atomic<uint64_t> capturedSamples_ { 0 };
	uint32_t packid_ = 0;
	std::atomic<bool> transmitting_ { false };
	bool wasTransmitting_ = false;
	std::atomic<bool> micTest_ { false };
	std::atomic<bool> voiceActivation_ { false };
	std::atomic<bool> voiceDetected_ { false };
	std::atomic<float> micLevel_ { -90.f };
	uint64_t gateOpenUntil_ = 0;
	uint64_t voiceUntil_ = 0;
	FrameSink sink_;
};
}
