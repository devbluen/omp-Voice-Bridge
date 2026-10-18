/*
 *  Voice Bridge client
 *
 *  Microphone noise suppression (RNNoise): fans, keyboards, traffic and hiss
 *  are removed before the voice is encoded.  RNNoise works on 10 ms frames
 *  at 48 kHz; samples that do not fill a frame wait for the next call.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct DenoiseState;

namespace vbc
{
class NoiseSuppressor
{
public:
	static constexpr std::size_t kFrameSamples = 480; // 10 ms at 48 kHz

	NoiseSuppressor() = default;
	~NoiseSuppressor();
	NoiseSuppressor(const NoiseSuppressor&) = delete;
	NoiseSuppressor& operator=(const NoiseSuppressor&) = delete;

	// Appends the cleaned samples to `out`.  Disabled, the input is appended
	// unchanged (after whatever was still waiting for a full frame).
	void process(const int16_t* samples, std::size_t count, bool enabled, std::vector<int16_t>& out);
	// Forgets the state (new microphone, recording restarted).
	void reset();

private:
	DenoiseState* state_ = nullptr;
	std::vector<float> input_;
};
}
