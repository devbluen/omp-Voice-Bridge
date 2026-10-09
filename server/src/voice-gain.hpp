/*
 *  Voice Bridge for open.mp and SA-MP
 *
 *  Server side voice volume.  Clients cannot be asked for more than 100%
 *  (the bass.dll shipped with SA-MP rejects volumes above 1), so a gain is
 *  applied to the audio itself: the Opus packet is decoded, amplified with
 *  a soft limiter and encoded again with the same duration (SampVoice
 *  clients only play 100 ms packets).  Nothing is done at 100%.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

struct OpusDecoder;
struct OpusEncoder;

namespace vbs
{
class VoiceGain
{
public:
	static constexpr std::size_t kSpeakers = 1004;

	VoiceGain();
	~VoiceGain();

	// Writes the amplified packet to out (capacity bytes).  False when the
	// original packet must be used unchanged (gain 1, invalid packet...).
	bool apply(uint16_t speaker, uint32_t packid, float gain, uint32_t bitrate, const uint8_t* in, std::size_t size, uint8_t* out,
		std::size_t capacity, std::size_t& written);
	void reset(uint16_t speaker);

private:
	struct State
	{
		std::mutex mutex;
		OpusDecoder* decoder = nullptr;
		OpusEncoder* encoder = nullptr;
		uint32_t bitrate = 0;
		~State();
	};
	std::array<std::unique_ptr<State>, kSpeakers> states_;
	std::mutex createMutex_; // UDP and tunnel voice may arrive on two threads
};
}
