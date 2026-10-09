/*
 *  Voice Bridge for open.mp and SA-MP
 */

#include "voice-gain.hpp"
#include <opus.h>
#include <algorithm>
#include <cmath>

namespace vbs
{
namespace
{
constexpr int kFrequency = 48000;
constexpr int kMaxSamples = 5760; // 120 ms

// Gain with a soft knee above 60% of full scale, so loud voices are not
// clipped harshly.  The ceiling stays under full scale: Opus overshoots a
// little when encoding a limited signal, which would clip on the clients.
int16_t amplify(int16_t sample, float gain)
{
	const float value = static_cast<float>(sample) * gain;
	const float magnitude = std::fabs(value);
	constexpr float knee = 0.6f * 32767.f;
	constexpr float ceiling = 0.9f * 32767.f;
	if (magnitude <= knee)
	{
		return static_cast<int16_t>(value);
	}
	const float over = (magnitude - knee) / (ceiling - knee);
	const float limited = knee + (ceiling - knee) * std::tanh(over);
	return static_cast<int16_t>(value < 0.f ? -limited : limited);
}
}

VoiceGain::State::~State()
{
	if (decoder)
	{
		opus_decoder_destroy(decoder);
	}
	if (encoder)
	{
		opus_encoder_destroy(encoder);
	}
}

VoiceGain::VoiceGain() = default;
VoiceGain::~VoiceGain() = default;

void VoiceGain::reset(uint16_t speaker)
{
	if (speaker < kSpeakers)
	{
		// Called with the voice routes locked exclusively: no packet of this
		// speaker is being amplified.
		std::lock_guard<std::mutex> create(createMutex_);
		states_[speaker].reset();
	}
}

bool VoiceGain::apply(uint16_t speaker, uint32_t packid, float gain, uint32_t bitrate, const uint8_t* in, std::size_t size, uint8_t* out,
	std::size_t capacity, std::size_t& written)
{
	if (speaker >= kSpeakers || std::fabs(gain - 1.f) < 0.01f || !size)
	{
		return false;
	}
	const int samples = opus_packet_get_nb_samples(in, static_cast<opus_int32>(size), kFrequency);
	if (samples <= 0 || samples > kMaxSamples)
	{
		return false;
	}

	State* existing = nullptr;
	{
		std::lock_guard<std::mutex> create(createMutex_);
		auto& slot = states_[speaker];
		if (!slot)
		{
			slot = std::make_unique<State>();
		}
		existing = slot.get();
	}
	State& state = *existing;
	std::lock_guard<std::mutex> lock(state.mutex);
	int error = 0;
	if (!state.decoder)
	{
		state.decoder = opus_decoder_create(kFrequency, 1, &error);
	}
	if (!state.encoder)
	{
		state.encoder = opus_encoder_create(kFrequency, 1, OPUS_APPLICATION_VOIP, &error);
		if (state.encoder)
		{
			opus_encoder_ctl(state.encoder, OPUS_SET_INBAND_FEC(1));
			opus_encoder_ctl(state.encoder, OPUS_SET_PACKET_LOSS_PERC(10));
			opus_encoder_ctl(state.encoder, OPUS_SET_COMPLEXITY(5));
		}
	}
	if (!state.decoder || !state.encoder)
	{
		return false;
	}
	if (packid == 0)
	{
		// A new utterance: start clean, like the clients do.
		opus_decoder_ctl(state.decoder, OPUS_RESET_STATE);
		opus_encoder_ctl(state.encoder, OPUS_RESET_STATE);
	}
	if (state.bitrate != bitrate)
	{
		state.bitrate = bitrate;
		opus_encoder_ctl(state.encoder, OPUS_SET_BITRATE(static_cast<opus_int32>(bitrate)));
	}

	int16_t pcm[kMaxSamples];
	const int decoded = opus_decode(state.decoder, in, static_cast<opus_int32>(size), pcm, kMaxSamples, 0);
	if (decoded != samples)
	{
		return false;
	}
	for (int i = 0; i < decoded; ++i)
	{
		pcm[i] = amplify(pcm[i], gain);
	}
	const int encoded = opus_encode(state.encoder, pcm, decoded, out, static_cast<opus_int32>(capacity));
	if (encoded <= 0)
	{
		return false;
	}
	written = static_cast<std::size_t>(encoded);
	return true;
}
}
