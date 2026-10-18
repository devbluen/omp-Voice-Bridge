/*
 *  Voice Bridge client - noise suppression on received voices (manual,
 *  needs an audio device)
 *
 *  The same 4 s of audio, a fan alone for 1 s and then a voice-like signal
 *  over it, arrives from two speakers in real time (100 ms Opus frames):
 *  one heard with "noise suppression on the voices I hear", one without.
 *  The fan must be much quieter on the first and the voice must stay.
 *
 *  usage: voice-bridge-incoming-noise-sim.exe ["C:\path\to\GTA San Andreas"]
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
constexpr int kRate = 48000;
constexpr int kFrames = 40; // 4 s
constexpr double kPi = 3.14159265358979;

using DspProc = void(CALLBACK*)(DWORD, DWORD, void*, DWORD, void*);
using SetDsp = DWORD(WINAPI*)(DWORD, DspProc, void*, int);

std::mutex g_mutex;
std::vector<float> g_heard[2]; // [0] cleaned speaker, [1] speaker heard as is

void CALLBACK capture(DWORD, DWORD, void* buffer, DWORD length, void* user)
{
	const auto* samples = static_cast<const int16_t*>(buffer);
	auto& heard = g_heard[reinterpret_cast<intptr_t>(user)];
	std::lock_guard<std::mutex> lock(g_mutex);
	for (DWORD i = 0; i + 1 < length / 2; i += 2)
	{
		heard.push_back(samples[i] / 32768.f);
	}
}

// Fan (hiss + 120 Hz hum) all the time; from 1 s a voice-like signal.
std::vector<int16_t> makeAudio()
{
	const int total = kRate * kFrames / 10;
	std::vector<int16_t> pcm(total);
	std::srand(5);
	float lowpass = 0.f;
	double phase = 0.0;
	for (int i = 0; i < total; ++i)
	{
		const float white = static_cast<float>(std::rand()) / RAND_MAX * 2.f - 1.f;
		lowpass += 0.3f * (white - lowpass);
		double sample = 0.05 * lowpass + 0.01 * std::sin(2.0 * kPi * 120.0 * i / kRate);
		const double t = static_cast<double>(i) / kRate;
		if (t >= 1.0)
		{
			const double pitch = 140.0 + 30.0 * std::sin(2.0 * kPi * 0.7 * t);
			phase += 2.0 * kPi * pitch / kRate;
			double voice = 0.0;
			for (int k = 1; k <= 25; ++k)
			{
				const double f = k * pitch;
				const double formant = std::exp(-std::pow((f - 700.0) / 300.0, 2)) + 0.6 * std::exp(-std::pow((f - 1200.0) / 400.0, 2)) + 0.05;
				voice += formant * std::sin(k * phase) / k;
			}
			sample += 0.25 * voice * (0.5 - 0.5 * std::cos(2.0 * kPi * 4.0 * t));
		}
		pcm[i] = static_cast<int16_t>(std::lround(std::max(-1.0, std::min(1.0, sample)) * 32767.0));
	}
	return pcm;
}

double rms(const std::vector<float>& signal, std::size_t from, std::size_t to)
{
	double sum = 0.0;
	to = std::min(to, signal.size());
	for (std::size_t i = from; i < to; ++i)
	{
		sum += signal[i] * signal[i];
	}
	return to > from ? std::sqrt(sum / static_cast<double>(to - from)) : 0.0;
}

double db(double ratio)
{
	return 20.0 * std::log10(std::max(ratio, 1e-9));
}
}

int main(int argc, char** argv)
{
	const std::string gta = argc > 1 ? argv[1] : "C:\\Program Files (x86)\\Gtas\\SanAndreas";
	LogOpen("incoming-noise-sim.log");
	HMODULE bass = LoadLibraryA((gta + "\\bass.dll").c_str());
	if (!bass)
	{
		std::printf("bass.dll not found in %s\n", gta.c_str());
		return 1;
	}
	const auto setDsp = reinterpret_cast<SetDsp>(GetProcAddress(bass, "BASS_ChannelSetDSP"));
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

	const std::vector<int16_t> pcm = makeAudio();
	int error = 0;
	OpusEncoder* encoder = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &error);
	opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
	std::vector<std::vector<uint8_t>> packets;
	for (int f = 0; f < kFrames; ++f)
	{
		uint8_t buffer[1500];
		const int size = opus_encode(encoder, pcm.data() + f * 4800, 4800, buffer, sizeof(buffer));
		packets.emplace_back(buffer, buffer + (size > 0 ? size : 0));
	}
	opus_encoder_destroy(encoder);

	// Speaker 7 is cleaned, speaker 8 is not: the setting is read per packet.
	const uint64_t start = GetTickCount64();
	std::vector<DWORD> hooked;
	int next = 0;
	while (GetTickCount64() - start < kFrames * 100 + 1500)
	{
		if (next < kFrames && GetTickCount64() - start >= static_cast<uint64_t>(next) * 100)
		{
			GetSettings().incomingNoiseSuppression = true;
			audio.pushVoice(1, 7, next, packets[next].data(), packets[next].size());
			GetSettings().incomingNoiseSuppression = false;
			audio.pushVoice(1, 8, next, packets[next].data(), packets[next].size());
			++next;
			for (DWORD handle : audio.channelHandles())
			{
				if (hooked.size() < 2 && std::find(hooked.begin(), hooked.end(), handle) == hooked.end()
					&& setDsp(handle, &capture, reinterpret_cast<void*>(static_cast<intptr_t>(hooked.size())), -2000000000))
				{
					hooked.push_back(handle);
				}
			}
		}
		audio.update(listener, false);
		Sleep(5);
	}

	double fan[2] = {};
	double voice[2] = {};
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		for (int c = 0; c < 2; ++c)
		{
			// Playback starts at the first frame: 0.5-0.95 s is the fan alone
			// (the suppressor has adapted), 1.3-3.7 s the voice over it.
			fan[c] = rms(g_heard[c], kRate / 2, kRate * 95 / 100);
			voice[c] = rms(g_heard[c], kRate * 13 / 10, kRate * 37 / 10);
		}
	}
	const double fanCut = db(fan[0] / fan[1]);
	const double voiceKept = db(voice[0] / voice[1]);
	std::printf("fan alone: %.1f dBFS as is, %.1f dBFS cleaned (%.1f dB); voice over the fan: %.1f dB\n", db(fan[1]), db(fan[0]), fanCut,
		voiceKept);
	audio.shutdown();
	const bool ok = fanCut <= -8.0 && voiceKept >= -6.0;
	std::printf(ok ? "incoming noise suppression ok\n" : "FAILED: the fan must drop by 8 dB or more and the voice lose 6 dB at most\n");
	return ok ? 0 : 1;
}
