/*
 *  Voice Bridge client - mobile network check (manual, needs an audio device)
 *
 *  A SampVoice mobile client talking for 6 s through a phone network: 30-200
 *  ms of delay per packet, in order (a radio link queues, it does not
 *  reorder), and every 1.5 s a 480 ms freeze released as one burst.  Nothing
 *  is lost on the way, so 6 s of voice must come out; the time the channel
 *  spends stalled while the speaker still talks is the choppiness.  Players
 *  reported mobile voices "twisted / garbled" on the Voice Bridge client.
 *
 *  "hot": the voice is as loud as a phone with automatic gain sends it (peaks
 *  near full scale); the distortion added on the way to the speakers is
 *  measured (THD+N of the 440 Hz tone).
 *
 *  usage: voice-bridge-mobile-sim.exe [frame_ms 20|40|60|100] [hot] ["C:\path\to\GTA San Andreas"]
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
constexpr uint64_t kTalkMs = 6000;

using DspProc = void(CALLBACK*)(DWORD, DWORD, void*, DWORD, void*);
using SetDsp = DWORD(WINAPI*)(DWORD, DspProc, void*, int);
using IsActive = DWORD(WINAPI*)(DWORD);

std::mutex g_mutex;
// Stereo frames (sample pairs) above the silence level that reached playback.
uint64_t g_loud = 0;
// Left channel of everything that reached playback (for the distortion check).
std::vector<float> g_signal;

void CALLBACK capture(DWORD, DWORD, void* buffer, DWORD length, void*)
{
	const auto* samples = static_cast<const int16_t*>(buffer);
	std::lock_guard<std::mutex> lock(g_mutex);
	// Counted per 1 ms block: a block of the tone has peaks far above 1500.
	for (DWORD i = 0; i + 96 <= length / 2; i += 96)
	{
		int peak = 0;
		for (DWORD j = 0; j < 96; ++j)
		{
			peak = std::max(peak, std::abs(static_cast<int>(samples[i + j])));
		}
		g_loud += peak > 1500 ? 48 : 0;
	}
	for (DWORD i = 0; i + 1 < length / 2; i += 2)
	{
		g_signal.push_back(static_cast<float>(samples[i]) / 32768.f);
	}
}

std::vector<std::vector<uint8_t>> encodeSpeech(int frameMs, int frames, double amplitude)
{
	int error = 0;
	OpusEncoder* encoder = opus_encoder_create(vb::kFrequency, 1, OPUS_APPLICATION_VOIP, &error);
	opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
	std::vector<int16_t> pcm(static_cast<std::size_t>(frameMs) * 48);
	std::vector<std::vector<uint8_t>> packets;
	double phase = 0.0;
	for (int f = 0; f < frames; ++f)
	{
		for (auto& sample : pcm)
		{
			phase += 2.0 * 3.14159265 * 440.0 / vb::kFrequency;
			sample = static_cast<int16_t>(amplitude * std::sin(phase));
		}
		uint8_t buffer[1500];
		const int size = opus_encode(encoder, pcm.data(), static_cast<int>(pcm.size()), buffer, sizeof(buffer));
		packets.emplace_back(buffer, buffer + (size > 0 ? size : 0));
	}
	opus_encoder_destroy(encoder);
	return packets;
}

// THD+N in %: everything that is not the 440 Hz tone, over voiced 100 ms
// blocks (Goertzel for the tone power).
double distortion(const std::vector<float>& signal)
{
	const std::size_t block = 4800;
	double tone = 0.0;
	double total = 0.0;
	for (std::size_t start = 0; start + block <= signal.size(); start += block)
	{
		double energy = 0.0;
		for (std::size_t i = 0; i < block; ++i)
		{
			energy += signal[start + i] * signal[start + i];
		}
		if (energy / block < 0.001)
		{
			continue; // silence or a gap
		}
		const double w = 2.0 * 3.14159265358979 * 440.0 / vb::kFrequency;
		double s1 = 0.0;
		double s2 = 0.0;
		for (std::size_t i = 0; i < block; ++i)
		{
			const double s0 = signal[start + i] + 2.0 * std::cos(w) * s1 - s2;
			s2 = s1;
			s1 = s0;
		}
		const double power = s1 * s1 + s2 * s2 - 2.0 * std::cos(w) * s1 * s2;
		tone += 2.0 * power / block;
		total += energy;
	}
	return total > 0.0 ? 100.0 * std::sqrt(std::max(0.0, total - tone) / total) : 0.0;
}

struct Arrival
{
	uint64_t at;
	uint32_t packid;
};
}

int main(int argc, char** argv)
{
	int frameMs = argc > 1 ? std::atoi(argv[1]) : 100;
	if (frameMs != 20 && frameMs != 40 && frameMs != 60)
	{
		frameMs = 100;
	}
	const int frames = static_cast<int>(kTalkMs) / frameMs;
	const bool hot = argc > 2 && std::string(argv[2]) == "hot";
	const int gtaArg = hot ? 3 : 2;
	const std::string gta = argc > gtaArg ? argv[gtaArg] : "C:\\Program Files (x86)\\Gtas\\SanAndreas";
	LogOpen("mobile-sim.log");
	HMODULE bass = LoadLibraryA((gta + "\\bass.dll").c_str());
	if (!bass)
	{
		std::printf("bass.dll not found in %s\n", gta.c_str());
		return 1;
	}
	const auto setDsp = reinterpret_cast<SetDsp>(GetProcAddress(bass, "BASS_ChannelSetDSP"));
	const auto isActive = reinterpret_cast<IsActive>(GetProcAddress(bass, "BASS_ChannelIsActive"));
	GetSettings().volume = 0;
	GetSettings().spatialMode = 2;
	Audio& audio = Audio::Get();
	if (!audio.init(GetConsoleWindow()))
	{
		std::printf("audio init failed\n");
		return 1;
	}
	audio.createStream(1, StreamKind::Global, 0xFFFFFFFF, "Radio", 0.f, {}, 0xFFFF);
	game::Listener listener;
	listener.valid = true;
	listener.front = { 0.f, 1.f, 0.f };
	listener.up = { 0.f, 0.f, 1.f };

	// 0.28 of full scale is a typical PC microphone; 0.9 a phone with AGC.
	const auto packets = encodeSpeech(frameMs, frames, hot ? 29500.0 : 9000.0);
	std::vector<Arrival> arrivals;
	std::srand(11);
	uint64_t previous = 0;
	for (int i = 0; i < frames; ++i)
	{
		const uint64_t sent = static_cast<uint64_t>(i) * frameMs;
		uint64_t at = std::max(previous, sent + 30 + std::rand() % 171);
		const uint64_t freezeStart = sent / 1500 * 1500 + 300;
		const uint64_t freezeEnd = freezeStart + 480;
		if (sent >= 1500 && at >= freezeStart && at < freezeEnd)
		{
			at = freezeEnd;
		}
		arrivals.push_back({ at, static_cast<uint32_t>(i) });
		previous = at;
	}

	const uint64_t start = GetTickCount64();
	DWORD handle = 0;
	std::size_t next = 0;
	uint64_t firstPlay = 0;
	uint64_t stalledMs = 0;
	int stalls = 0;
	bool wasStalled = false;
	uint64_t lastPoll = start;
	const uint64_t talkEnd = kTalkMs + 200;
	while (GetTickCount64() - start < kTalkMs + 2500)
	{
		const uint64_t now = GetTickCount64();
		const uint64_t elapsed = now - start;
		while (next < arrivals.size() && arrivals[next].at <= elapsed)
		{
			const auto& packet = packets[arrivals[next].packid];
			audio.pushVoice(1, 7, arrivals[next].packid, packet.data(), packet.size());
			++next;
			if (!handle)
			{
				const auto handles = audio.channelHandles();
				if (!handles.empty() && setDsp(handles[0], &capture, nullptr, -2000000000))
				{
					handle = handles[0];
				}
			}
		}
		if (handle)
		{
			const DWORD state = isActive(handle);
			if (!firstPlay && state == 1)
			{
				firstPlay = elapsed;
			}
			// Stalled (or paused to re-buffer) while the speaker still talks.
			const bool stalled = firstPlay && elapsed < talkEnd && state != 1;
			if (stalled)
			{
				stalledMs += now - lastPoll;
				stalls += wasStalled ? 0 : 1;
			}
			wasStalled = stalled;
		}
		lastPoll = now;
		audio.update(listener, false);
		Sleep(5);
	}

	uint64_t heardMs = 0;
	double thd = 0.0;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		heardMs = g_loud / 48;
		thd = distortion(g_signal);
	}
	const uint64_t missingMs = heardMs < kTalkMs ? kTalkMs - heardMs : 0;
	std::printf("%d ms frames: first sound after %llu ms, %llu ms of the %llu ms heard (%llu ms missing, none lost on the way), "
				"stalled %d time(s) for %llu ms while talking, distortion %.1f%%%s\n",
		frameMs, static_cast<unsigned long long>(firstPlay), static_cast<unsigned long long>(heardMs),
		static_cast<unsigned long long>(kTalkMs), static_cast<unsigned long long>(missingMs), stalls,
		static_cast<unsigned long long>(stalledMs), thd, hot ? " (loud phone voice)" : "");
	audio.shutdown();
	return missingMs > 150 || stalledMs > 300 || thd > 5.0 ? 1 : 0;
}
