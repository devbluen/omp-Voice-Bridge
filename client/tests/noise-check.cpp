/*
 *  Voice Bridge client - noise suppression check (no audio device needed)
 *
 *  4 s of microphone input at 48 kHz in 20 ms blocks (BASS's recording
 *  period): a fan (hiss + 120 Hz hum) alone, then a voice-like signal
 *  (harmonics of a gliding pitch, syllables at 4 Hz) over the same fan, then
 *  the fan alone again.  The suppressor must cut the fan without eating the
 *  voice, keep every sample (only less than one 10 ms frame may wait), and
 *  pass the input untouched when disabled.
 *
 *  usage: voice-bridge-noise-check.exe
 */

#include "noise.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace vbc;

namespace
{
constexpr int kRate = 48000;
constexpr double kPi = 3.14159265358979;

double rms(const std::vector<float>& signal, int from, int to)
{
	double sum = 0.0;
	for (int i = from; i < to && i < static_cast<int>(signal.size()); ++i)
	{
		sum += signal[i] * signal[i];
	}
	return std::sqrt(sum / std::max(1, to - from));
}

double db(double ratio)
{
	return 20.0 * std::log10(std::max(ratio, 1e-9));
}
}

int main()
{
	const int total = kRate * 4;
	std::vector<float> fan(total);
	std::vector<float> voice(total, 0.f);
	std::srand(3);
	float lowpass = 0.f;
	for (int i = 0; i < total; ++i)
	{
		const float white = static_cast<float>(std::rand()) / RAND_MAX * 2.f - 1.f;
		lowpass += 0.3f * (white - lowpass);
		fan[i] = 0.05f * lowpass + 0.01f * static_cast<float>(std::sin(2.0 * kPi * 120.0 * i / kRate));
	}
	double phase = 0.0;
	for (int i = kRate; i < 3 * kRate; ++i)
	{
		const double t = static_cast<double>(i) / kRate;
		const double pitch = 140.0 + 30.0 * std::sin(2.0 * kPi * 0.7 * t);
		phase += 2.0 * kPi * pitch / kRate;
		double sample = 0.0;
		for (int k = 1; k <= 25; ++k)
		{
			// Rough vowel formants around 700 and 1200 Hz.
			const double f = k * pitch;
			const double formant = std::exp(-std::pow((f - 700.0) / 300.0, 2)) + 0.6 * std::exp(-std::pow((f - 1200.0) / 400.0, 2)) + 0.05;
			sample += formant * std::sin(k * phase) / k;
		}
		const double syllable = 0.5 - 0.5 * std::cos(2.0 * kPi * 4.0 * t);
		voice[i] = static_cast<float>(0.25 * sample * syllable);
	}

	std::vector<int16_t> input(total);
	for (int i = 0; i < total; ++i)
	{
		input[i] = static_cast<int16_t>(std::lround(std::max(-1.f, std::min(1.f, fan[i] + voice[i])) * 32767.f));
	}

	const auto run = [&](bool enabled)
	{
		NoiseSuppressor suppressor;
		std::vector<int16_t> out;
		for (int at = 0; at < total; at += 960)
		{
			suppressor.process(input.data() + at, 960, enabled, out);
		}
		return out;
	};

	int failures = 0;
	const std::vector<int16_t> bypass = run(false);
	if (bypass != input)
	{
		std::printf("FAILED: disabled, the input must pass unchanged\n");
		++failures;
	}

	const auto started = std::chrono::steady_clock::now();
	const std::vector<int16_t> cleaned = run(true);
	const double cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
	if (static_cast<int>(cleaned.size()) + static_cast<int>(NoiseSuppressor::kFrameSamples) <= total)
	{
		std::printf("FAILED: %zu of %d samples came out\n", cleaned.size(), total);
		++failures;
	}
	std::vector<float> in(total);
	std::vector<float> out(total, 0.f);
	for (int i = 0; i < total; ++i)
	{
		in[i] = input[i] / 32768.f;
		out[i] = i < static_cast<int>(cleaned.size()) ? cleaned[i] / 32768.f : 0.f;
	}
	// Fan alone: after the first 300 ms (the model adapts) and in the last second.
	const double fanBefore = (rms(in, kRate * 3 / 10, kRate) + rms(in, 3 * kRate + kRate / 10, total)) / 2.0;
	const double fanAfter = (rms(out, kRate * 3 / 10, kRate) + rms(out, 3 * kRate + kRate / 10, total)) / 2.0;
	const double voiceIn = rms(voice, kRate, 3 * kRate);
	const double voiceOut = rms(out, kRate, 3 * kRate);
	const double fanCut = db(fanAfter / fanBefore);
	const double voiceKept = db(voiceOut / voiceIn);
	std::printf("fan alone: %.1f dB (%.1f -> %.1f dBFS), voice over the fan: %.1f dB, %.0f ms of CPU for 4 s of audio (%.1f%% of one core)\n",
		fanCut, db(fanBefore), db(fanAfter), voiceKept, cpuMs, cpuMs / 40.0);
	if (fanCut > -10.0)
	{
		std::printf("FAILED: the fan must drop by 10 dB or more\n");
		++failures;
	}
	if (voiceKept < -6.0)
	{
		std::printf("FAILED: the voice must not lose more than 6 dB\n");
		++failures;
	}
	std::printf(failures ? "%d check(s) failed\n" : "all checks passed\n", failures);
	return failures ? 1 : 0;
}
