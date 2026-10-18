/*
 *  Voice Bridge client
 */

#include "noise.hpp"
#include <rnnoise.h>
#include <algorithm>

namespace vbc
{
NoiseSuppressor::~NoiseSuppressor()
{
	reset();
}

void NoiseSuppressor::reset()
{
	if (state_)
	{
		rnnoise_destroy(state_);
		state_ = nullptr;
	}
	input_.clear();
}

void NoiseSuppressor::process(const int16_t* samples, std::size_t count, bool enabled, std::vector<int16_t>& out)
{
	if (!enabled || (!state_ && !(state_ = rnnoise_create(nullptr))))
	{
		// Switched off mid-speech: what was waiting goes out first, in order.
		for (float sample : input_)
		{
			out.push_back(static_cast<int16_t>(sample));
		}
		if (state_)
		{
			rnnoise_destroy(state_);
			state_ = nullptr;
		}
		input_.clear();
		out.insert(out.end(), samples, samples + count);
		return;
	}

	// RNNoise takes and returns samples in the 16 bit range, as floats.
	input_.insert(input_.end(), samples, samples + count);
	float cleaned[kFrameSamples];
	std::size_t used = 0;
	while (input_.size() - used >= kFrameSamples)
	{
		rnnoise_process_frame(state_, cleaned, input_.data() + used);
		for (float sample : cleaned)
		{
			out.push_back(static_cast<int16_t>(std::clamp(sample, -32768.f, 32767.f)));
		}
		used += kFrameSamples;
	}
	input_.erase(input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(used));
}
}
