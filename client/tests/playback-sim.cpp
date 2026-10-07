/*
 *  Voice Bridge client - playback simulation (manual, needs an audio device)
 *
 *  Sends two utterances in real time (100 ms frames, a pause between them)
 *  through the playback engine and prints the channel output level every
 *  100 ms.  The level must follow the speech and drop to 0 after it ends.
 *
 *  usage: voice-bridge-playback-sim.exe [jitter_ms] ["C:\path\to\GTA San Andreas"]
 */

#include "audio.hpp"
#include "log.hpp"
#include "settings.hpp"
#include <vb-protocol.hpp>
#include <opus.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace vbc;

namespace
{
using GetLevel = DWORD(WINAPI*)(DWORD);
using IsActive = DWORD(WINAPI*)(DWORD);
GetLevel g_level = nullptr;
IsActive g_active = nullptr;
uint64_t g_start = 0;

std::vector<std::vector<uint8_t>> encodeTone(int frames, double frequency)
{
	int error = 0;
	OpusEncoder* encoder = opus_encoder_create(vb::kFrequency, 1, OPUS_APPLICATION_VOIP, &error);
	opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
	opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(1));
	std::vector<int16_t> pcm(4800);
	std::vector<std::vector<uint8_t>> packets;
	int phase = 0;
	for (int f = 0; f < frames; ++f)
	{
		for (auto& sample : pcm)
		{
			sample = static_cast<int16_t>(9000.0 * std::sin(2.0 * 3.14159265 * frequency * phase++ / vb::kFrequency));
		}
		uint8_t buffer[1500];
		const int size = opus_encode(encoder, pcm.data(), 4800, buffer, sizeof(buffer));
		packets.emplace_back(buffer, buffer + (size > 0 ? size : 0));
	}
	opus_encoder_destroy(encoder);
	return packets;
}

void report(const char* phase, const game::Listener& listener)
{
	Audio& audio = Audio::Get();
	audio.update(listener, false);
	const auto handles = audio.channelHandles();
	std::printf("%6llu ms  %-10s", static_cast<unsigned long long>(GetTickCount64() - g_start), phase);
	for (DWORD handle : handles)
	{
		const DWORD level = g_level(handle);
		std::printf("  state %u  L %5u R %5u", g_active(handle), LOWORD(level), HIWORD(level));
	}
	std::printf("\n");
}

// Waits `ms` while the "game" runs at ~60 fps, printing every 100 ms.
void run(const char* phase, int ms, const game::Listener& listener)
{
	const uint64_t end = GetTickCount64() + ms;
	uint64_t nextReport = GetTickCount64();
	while (GetTickCount64() < end)
	{
		if (GetTickCount64() >= nextReport)
		{
			report(phase, listener);
			nextReport += 100;
		}
		Audio::Get().update(listener, false);
		Sleep(16);
	}
}
}

int main(int argc, char** argv)
{
	const int jitter = argc > 1 ? std::atoi(argv[1]) : 0;
	const std::string gta = argc > 2 ? argv[2] : "C:\\Program Files (x86)\\Gtas\\SanAndreas";
	LogOpen("playback-sim.log");
	HMODULE bass = LoadLibraryA((gta + "\\bass.dll").c_str());
	if (!bass)
	{
		std::printf("bass.dll not found in %s\n", gta.c_str());
		return 1;
	}
	g_level = reinterpret_cast<GetLevel>(GetProcAddress(bass, "BASS_ChannelGetLevel"));
	g_active = reinterpret_cast<IsActive>(GetProcAddress(bass, "BASS_ChannelIsActive"));
	GetSettings().volume = 0; // silent; the level is measured before the volume
	Audio& audio = Audio::Get();
	if (!audio.init(GetConsoleWindow()))
	{
		std::printf("audio init failed\n");
		return 1;
	}
	audio.createStream(1, StreamKind::Point, 0xFFFFFFFF, "Local", 50.f, { 5.f, 0.f, 0.f }, 0xFFFF);

	game::Listener listener;
	listener.valid = true;
	listener.front = { 0.f, 1.f, 0.f };
	listener.up = { 0.f, 0.f, 1.f };

	const auto first = encodeTone(20, 440.0);
	const auto second = encodeTone(10, 660.0);
	g_start = GetTickCount64();
	std::srand(1);
	const auto talk = [&](const char* phase, const std::vector<std::vector<uint8_t>>& packets)
	{
		for (uint32_t i = 0; i < packets.size(); ++i)
		{
			const int delay = jitter ? std::rand() % (jitter + 1) : 0;
			if (&packets == &first && i == 10)
			{
				// The speaker walks to the left while still talking.
				audio.setStreamPosition(1, { -5.f, 0.f, 0.f });
				std::printf("------ speaker moved to the left ------\n");
			}
			audio.pushVoice(1, 3, i, packets[i].data(), packets[i].size());
			run(phase, 100 - std::min(delay, 99), listener);
			if (delay)
			{
				Sleep(delay);
			}
		}
	};
	talk("talk 1", first);
	run("pause", 1500, listener);
	talk("talk 2", second);
	run("after", 2500, listener);
	audio.setStreamPosition(1, { 5.f, 0.f, 0.f });
	std::printf("stale packets dropped: %d\n", audio.stalePackets());
	audio.shutdown();
	return 0;
}
