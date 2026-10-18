/*
 *  Voice Bridge client - repeated audio check (manual, needs an audio device)
 *
 *  One continuous utterance where every 100 ms frame has its own tone, sent
 *  through a bad network (jitter, a freeze followed by a burst, reordering,
 *  loss).  Everything that reaches the playback buffer is recorded and the
 *  tone sequence is checked: a frame heard twice is a repeat.
 *
 *  usage: voice-bridge-repeat-sim.exe [streams 0|1|2] ["C:\path\to\GTA San Andreas"]
 *  1: the speaker is also in a second stream the listener hears (the server
 *  then sends every packet twice, once per stream).
 *  2: the first copy always comes from a local stream out of the listener's
 *  range, the second from a global radio: only the radio is audible, so only
 *  its channel counts (the radio used to be silenced by the local copy).
 */

#include "audio.hpp"
#include "log.hpp"
#include "settings.hpp"
#include <vb-protocol.hpp>
#include <opus.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

using namespace vbc;

namespace
{
constexpr int kFrames = 40;

using DspProc = void(CALLBACK*)(DWORD, DWORD, void*, DWORD, void*);
using SetDsp = DWORD(WINAPI*)(DWORD, DspProc, void*, int);

std::mutex g_mutex;
// Per voice channel: mono samples that entered the playback buffer.
std::vector<int16_t> g_heard[4];

void CALLBACK capture(DWORD, DWORD, void* buffer, DWORD length, void* user)
{
	const auto* samples = static_cast<const int16_t*>(buffer);
	auto& heard = g_heard[reinterpret_cast<intptr_t>(user)];
	std::lock_guard<std::mutex> lock(g_mutex);
	for (DWORD i = 0; i + 1 < length / 2; i += 2)
	{
		heard.push_back(static_cast<int16_t>((samples[i] + samples[i + 1]) / 2));
	}
}

double frameFrequency(int frame)
{
	return 300.0 + 45.0 * frame;
}

std::vector<std::vector<uint8_t>> encodeFrames()
{
	int error = 0;
	OpusEncoder* encoder = opus_encoder_create(vb::kFrequency, 1, OPUS_APPLICATION_VOIP, &error);
	opus_encoder_ctl(encoder, OPUS_SET_BITRATE(32000));
	opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(1));
	opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(10));
	std::vector<int16_t> pcm(4800);
	std::vector<std::vector<uint8_t>> packets;
	double phase = 0.0;
	for (int f = 0; f < kFrames; ++f)
	{
		for (auto& sample : pcm)
		{
			phase += 2.0 * 3.14159265 * frameFrequency(f) / vb::kFrequency;
			sample = static_cast<int16_t>(9000.0 * std::sin(phase));
		}
		uint8_t buffer[1500];
		const int size = opus_encode(encoder, pcm.data(), 4800, buffer, sizeof(buffer));
		packets.emplace_back(buffer, buffer + (size > 0 ? size : 0));
	}
	opus_encoder_destroy(encoder);
	return packets;
}

// Which frame a 20 ms window of output belongs to (-1 = silence/unclear).
int windowFrame(const int16_t* samples, int count)
{
	int crossings = 0;
	int loud = 0;
	for (int i = 1; i < count; ++i)
	{
		if ((samples[i - 1] < 0) != (samples[i] < 0))
		{
			++crossings;
		}
		if (std::abs(samples[i]) > 1500)
		{
			++loud;
		}
	}
	if (loud < count / 4)
	{
		return -1;
	}
	const double hz = crossings * static_cast<double>(vb::kFrequency) / (2.0 * count);
	const int frame = static_cast<int>(std::lround((hz - 300.0) / 45.0));
	return frame >= 0 && frame < kFrames && std::fabs(hz - frameFrequency(frame)) < 15.0 ? frame : -1;
}

struct Arrival
{
	uint64_t at;
	uint32_t packid;
	uint32_t stream;
};
}

int main(int argc, char** argv)
{
	const int mode = argc > 1 ? std::clamp(argv[1][0] - '0', 0, 2) : 0;
	const bool twoStreams = mode != 0;
	const std::string gta = argc > 2 ? argv[2] :"C:\\Program Files (x86)\\Gtas\\SanAndreas";
	LogOpen("repeat-sim.log");
	HMODULE bass = LoadLibraryA((gta + "\\bass.dll").c_str());
	if (!bass)
	{
		std::printf("bass.dll not found in %s\n", gta.c_str());
		return 1;
	}
	const auto setDsp = reinterpret_cast<SetDsp>(GetProcAddress(bass, "BASS_ChannelSetDSP"));
	GetSettings().volume = 0;
	GetSettings().spatialMode = 2; // flat, so the tone is not altered
	// Tests the network path: pure tones are what noise suppression removes.
	GetSettings().incomingNoiseSuppression = false;
	Audio& audio = Audio::Get();
	if (!audio.init(GetConsoleWindow()))
	{
		std::printf("audio init failed\n");
		return 1;
	}
	if (mode == 2)
	{
		audio.createStream(1, StreamKind::Point, 0xFFFFFFFF, "Local", 15.f, { 500.f, 0.f, 0.f }, 0xFFFF);
	}
	else
	{
		audio.createStream(1, StreamKind::Global, 0xFFFFFFFF, "Global", 0.f, {}, 0xFFFF);
	}
	audio.createStream(2, StreamKind::Global, 0xFFFFFFFF, "Radio", 0.f, {}, 0xFFFF);
	game::Listener listener;
	listener.valid = true;
	listener.front = { 0.f, 1.f, 0.f };
	listener.up = { 0.f, 0.f, 1.f };
	audio.update(listener, false);

	// Network: frame i is sent at i*100 ms, with 0-80 ms jitter.  Frames
	// 12-18 are held by a 700 ms freeze and arrive together; 25 and 26 swap;
	// 31 is lost.
	const auto packets = encodeFrames();
	std::vector<Arrival> arrivals;
	std::srand(7);
	for (uint32_t i = 0; i < kFrames; ++i)
	{
		uint64_t at = i * 100 + std::rand() % 81;
		if (i >= 12 && i <= 18)
		{
			at = 18 * 100 + 700;
		}
		if (i == 25)
		{
			at = 26 * 100 + 90;
		}
		if (i == 31)
		{
			continue;
		}
		arrivals.push_back({ at, i, 1 });
		if (twoStreams)
		{
			// The copy of the second stream: up to 150 ms earlier or later.
			const int shift = mode == 2 ? 1 + std::rand() % 5 : std::rand() % 301 - 150;
			arrivals.push_back({ static_cast<uint64_t>(std::max<int64_t>(0, static_cast<int64_t>(at) + shift)), i, 2 });
		}
	}
	std::stable_sort(arrivals.begin(), arrivals.end(), [](const Arrival& a, const Arrival& b) { return a.at < b.at; });

	const uint64_t start = GetTickCount64();
	std::vector<DWORD> hooked;
	std::vector<uint32_t> hookedStream;
	std::size_t next = 0;
	while (GetTickCount64() - start < kFrames * 100 + 2500)
	{
		const uint64_t elapsed = GetTickCount64() - start;
		while (next < arrivals.size() && arrivals[next].at <= elapsed)
		{
			const auto& packet = packets[arrivals[next].packid];
			audio.pushVoice(arrivals[next].stream, 3, arrivals[next].packid, packet.data(), packet.size());
			++next;
			// Record every voice channel (one per stream the speaker is heard in).
			for (DWORD handle : audio.channelHandles())
			{
				if (hooked.size() < 4 && std::find(hooked.begin(), hooked.end(), handle) == hooked.end()
					&& setDsp(handle, &capture, reinterpret_cast<void*>(static_cast<intptr_t>(hooked.size())), -2000000000))
				{
					hooked.push_back(handle);
					hookedStream.push_back(arrivals[next - 1].stream);
				}
			}
		}
		audio.update(listener, false);
		Sleep(5);
	}

	// How many times each frame was heard, over every channel.
	int times[kFrames] = {};
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		for (std::size_t c = 0; c < hooked.size(); ++c)
		{
			const bool audible = mode != 2 || hookedStream[c] == 2;
			std::printf("channel %zu (stream %u%s) heard:", c + 1, hookedStream[c], audible ? "" : ", out of range");
			int last = -1;
			const auto& heard = g_heard[c];
			for (std::size_t i = 0; i + 960 <= heard.size(); i += 960)
			{
				const int frame = windowFrame(heard.data() + i, 960);
				if (frame >= 0 && frame != last)
				{
					std::printf(" %d", frame);
					times[frame] += audible ? 1 : 0;
					last = frame;
				}
			}
			std::printf("\n");
		}
	}
	int repeats = 0;
	int missing = 0;
	for (int f = 0; f < kFrames; ++f)
	{
		repeats += times[f] > 1 ? 1 : 0;
		missing += times[f] == 0 ? 1 : 0;
	}
	std::printf("%s: repeated frames: %d, missing frames: %d of %d (sent network loses 1 and holds 7 in a freeze), stale packets dropped: %d\n",
		mode == 2 ? "local out of range + radio" : twoStreams ? "two streams" : "one stream", repeats, missing, kFrames, audio.stalePackets());
	audio.shutdown();
	return repeats || (mode == 2 && missing > 12) ? 1 : 0;
}
