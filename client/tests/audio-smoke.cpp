/*
 *  Voice Bridge client - audio smoke test (manual, needs an audio device)
 *
 *  Loads the bass.dll shipped with SA-MP, plays Opus voice through the
 *  playback engine with effects and packet loss, and lists microphones.
 *  The master volume is set to 0 so nothing is heard.
 *
 *  usage: voice-bridge-audio-smoke.exe ["C:\path\to\GTA San Andreas"]
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
int g_failures = 0;

void check(const char* name, bool condition)
{
	std::printf("%-40s %s\n", name, condition ? "ok" : "FAILED");
	if (!condition)
	{
		++g_failures;
	}
}

std::vector<std::vector<uint8_t>> encodeTone(int frames, int frameMs, double amplitude = 8000.0)
{
	int error = 0;
	OpusEncoder* encoder = opus_encoder_create(vb::kFrequency, 1, OPUS_APPLICATION_VOIP, &error);
	opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
	opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(1));
	opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(10));
	const int samples = frameMs * 48;
	std::vector<int16_t> pcm(samples);
	std::vector<std::vector<uint8_t>> packets;
	int phase = 0;
	for (int f = 0; f < frames; ++f)
	{
		for (int i = 0; i < samples; ++i, ++phase)
		{
			pcm[i] = static_cast<int16_t>(amplitude * std::sin(2.0 * 3.14159265 * 440.0 * phase / vb::kFrequency));
		}
		uint8_t buffer[1500];
		const int size = opus_encode(encoder, pcm.data(), samples, buffer, sizeof(buffer));
		packets.emplace_back(buffer, buffer + (size > 0 ? size : 0));
	}
	opus_encoder_destroy(encoder);
	return packets;
}
}

int main(int argc, char** argv)
{
	const std::string gta = argc > 1 ? argv[1] : "C:\\Program Files (x86)\\Gtas\\SanAndreas";
	LogOpen("audio-smoke.log");
	if (!LoadLibraryA((gta + "\\bass.dll").c_str()))
	{
		std::printf("bass.dll not found in %s; skipping\n", gta.c_str());
		return 0;
	}
	GetSettings().volume = 0; // silent
	Audio& audio = Audio::Get();
	check("audio init (bass.dll + output)", audio.init(GetConsoleWindow()));
	if (!audio.ready())
	{
		return 1;
	}

	const auto microphones = audio.microphones();
	std::printf("microphones: %zu\n", microphones.size());
	for (const auto& name : microphones)
	{
		std::printf("  - %s\n", name.c_str());
	}

	audio.createStream(1, StreamKind::Global, 0xFFFF0000, "Global", 0.f, {}, 0xFFFF);
	audio.createStream(2, StreamKind::Point, 0xFF00FF00, "Local", 50.f, { 10.f, 0.f, 0.f }, 0xFFFF);
	const vb::ReverbParams reverb { 0.f, -4.f, 1000.f, 0.5f };
	audio.createEffect(1, 77, vb::effect::reverb, 0, reinterpret_cast<const uint8_t*>(&reverb), sizeof(reverb));
	const vb::EchoParams echo { 30.f, 40.f, 200.f, 200.f, 0 };
	audio.createEffect(2, 78, vb::effect::echo, 0, reinterpret_cast<const uint8_t*>(&echo), sizeof(echo));
	audio.setParameter(1, vb::param::volume, 0.8f);
	audio.slideParameter(2, vb::param::frequency, 44100.f, 52000.f, 500);

	// 100 ms frames like SampVoice, then 20 ms frames like a low latency server.
	const auto legacy = encodeTone(10, 100);
	const auto fast = encodeTone(20, 20);
	check("opus encode", legacy.size() == 10 && !legacy[0].empty() && !fast[0].empty());

	game::Listener listener;
	listener.valid = true;
	listener.front = { 0.f, 1.f, 0.f };
	listener.up = { 0.f, 0.f, 1.f };
	for (uint32_t i = 0; i < legacy.size(); ++i)
	{
		if (i == 4)
		{
			continue; // lost packet: recovered with FEC
		}
		audio.pushVoice(1, 3, i, legacy[i].data(), legacy[i].size());
	}
	for (uint32_t i = 0; i < fast.size(); ++i)
	{
		audio.pushVoice(2, 4, i, fast[i].data(), fast[i].size());
	}
	audio.pushVoice(1, 3, 2, legacy[2].data(), legacy[2].size()); // late duplicate is ignored
	audio.pushVoice(99, 5, 0, legacy[0].data(), legacy[0].size()); // unknown stream is ignored
	audio.update(listener, false);

	const auto speakers = audio.speakers();
	check("two speakers are active", speakers.size() == 2);

	// Speaker 6 starts a new utterance (ids restart at 0); a packet from
	// the previous one arrives late with a high id and must not be played.
	const int staleBefore = audio.stalePackets();
	audio.pushVoice(1, 6, 0, legacy[0].data(), legacy[0].size());
	audio.pushVoice(1, 6, 1, legacy[1].data(), legacy[1].size());
	audio.pushVoice(1, 6, 45, legacy[5].data(), legacy[5].size());
	audio.pushVoice(1, 6, 2, legacy[2].data(), legacy[2].size());
	check("late packet of an earlier utterance is dropped", audio.stalePackets() == staleBefore + 1);

	// Real volume with silent audio: every attribute must be accepted by the
	// bass.dll that ships with SA-MP (it rejects volumes above 1).
	GetSettings().volume = 200;
	audio.setPlayerVolume(3, 3.f);
	const auto silence = encodeTone(5, 100, 0.0);
	for (uint32_t i = 0; i < silence.size(); ++i)
	{
		audio.pushVoice(1, 3, 100 + i, silence[i].data(), silence[i].size());
	}
	audio.update(listener, false);
	check("BASS accepted every volume/pan", audio.attributeErrors() == 0);
	GetSettings().volume = 0;

	audio.setPlayerMuted(3, true);
	audio.pushVoice(1, 3, 0, legacy[0].data(), legacy[0].size());
	check("muted player is reported", audio.playerMuted(3));
	audio.setPlayerVolume(4, 2.5f);
	check("player volume", std::fabs(audio.playerVolume(4) - 2.5f) < 0.001f);

	for (int i = 0; i < 20; ++i)
	{
		audio.update(listener, false);
		Sleep(20);
	}
	audio.deleteEffect(1, 77);
	audio.deleteStream(1);
	audio.deleteStream(2);
	check("streams deleted", audio.speakers().empty());

	audio.configureEncoder(24000, 100);
	check("encoder frame length", audio.frameMs() == 100);
	audio.configureEncoder(32000, 20);
	check("low latency frames", audio.frameMs() == 20);
	audio.configureEncoder(32000, 33);
	check("invalid frame length falls back to 100", audio.frameMs() == 100);

	if (!microphones.empty())
	{
		check("open microphone", audio.openMicrophone(""));
		// The client keeps the recording running for the whole session (a
		// paused bass.dll recording delivers stale audio on resume), so it
		// must deliver all of the audio, in real time, with nothing removed.
		audio.setRecordingActive(true);
		Sleep(500); // device start
		const uint64_t before = audio.capturedSamples();
		Sleep(3000);
		const uint64_t ms = (audio.capturedSamples() - before) / 48;
		audio.setRecordingActive(false);
		std::printf("  %llu ms of microphone audio in 3000 ms of recording\n",static_cast<unsigned long long>(ms));
		check("microphone audio is complete", ms >= 2900 && ms <= 3100);

		// Talking for 3 s (noise gate off): one encoded frame every 100 ms,
		// numbered without gaps.
		audio.configureEncoder(24000, 100);
		GetSettings().noiseGate = -90;
		std::vector<uint32_t> sent;
		std::mutex sentMutex;
		audio.setFrameSink([&](const uint8_t*, std::size_t, uint32_t packid)
		{
			std::lock_guard<std::mutex> lock(sentMutex);
			sent.push_back(packid);
		});
		audio.setRecordingActive(true);
		Sleep(100); // talking right after the microphone started: no old audio
		audio.setTransmitting(true);
		Sleep(3000);
		audio.setTransmitting(false);
		Sleep(200);
		audio.setRecordingActive(false);
		{
			std::lock_guard<std::mutex> lock(sentMutex);
			bool consecutive = !sent.empty() && sent.front() == 0;
			for (std::size_t i = 1; i < sent.size(); ++i)
			{
				consecutive = consecutive && sent[i] == sent[i - 1] + 1;
			}
			std::printf("  %zu frames sent in 3 s of talking\n", sent.size());
			check("continuous speech is sent without gaps", consecutive && sent.size() >= 29 && sent.size() <= 31);
		}
		audio.setFrameSink(nullptr);
		audio.closeMicrophone();
	}
	audio.shutdown();
	std::printf("%s\n", g_failures ? "audio smoke test FAILED" : "audio smoke test passed");
	return g_failures ? 1 : 0;
}
