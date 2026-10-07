/*
 *  Voice Bridge client
 */

#include "audio.hpp"
#include "log.hpp"
#include "samp.hpp"
#include "settings.hpp"
#include <vb-protocol.hpp>
#include <opus.h>
#include <algorithm>
#include <cmath>

namespace vbc
{
namespace
{
constexpr int kMaxFrameSamples = 5760; // 120 ms at 48 kHz
constexpr uint64_t kSpeakingWindowMs = 300;
constexpr uint64_t kChannelIdleMs = 60000;
constexpr uint64_t kRestartGapMs = 1000;
// A speaker stays on the stream that delivered their audio until it has
// been quiet this long; copies from other streams are dropped.
constexpr uint64_t kRouteHoldMs = 400;
// How far ahead of the clock a packet may claim to be before it is treated
// as a leftover of the previous utterance.
constexpr uint64_t kStaleToleranceMs = 600;
// Audio waiting to be played is capped so the delay (and the direction baked
// into old audio) never builds up while someone talks without pausing.
constexpr DWORD kMaxQueuedBytes = 300 * (vb::kFrequency / 1000) * 2 * sizeof(int16_t);
// After the script's effects (their priorities are small numbers).
constexpr int kSpatialDspPriority = -1000000;
constexpr uint64_t kGateHangoverMs = 350;
constexpr uint64_t kVoiceHangoverMs = 600;
// The bass.dll shipped with SA-MP (2.4.7) rejects BASS_ATTRIB_VOL above 1, so
// amplification is applied to the samples and the attribute stays in 0..1.
constexpr float kBaseGain = 1.6f;
constexpr float kMaxPreGain = 6.f;

uint64_t now()
{
	return GetTickCount64();
}

float levelDb(const int16_t* samples, std::size_t count)
{
	if (!count)
	{
		return -90.f;
	}
	double sum = 0.0;
	for (std::size_t i = 0; i < count; ++i)
	{
		const double sample = samples[i] / 32768.0;
		sum += sample * sample;
	}
	const double rms = std::sqrt(sum / static_cast<double>(count));
	return rms > 0.0 ? std::max(-90.f, static_cast<float>(20.0 * std::log10(rms))) : -90.f;
}

game::Vec3 sub(const game::Vec3& a, const game::Vec3& b)
{
	return { a.x - b.x, a.y - b.y, a.z - b.z };
}

float dot(const game::Vec3& a, const game::Vec3& b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

game::Vec3 cross(const game::Vec3& a, const game::Vec3& b)
{
	return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

float length(const game::Vec3& value)
{
	return std::sqrt(dot(value, value));
}

game::Vec3 normalize(const game::Vec3& value)
{
	const float size = length(value);
	return size > 0.0001f ? game::Vec3 { value.x / size, value.y / size, value.z / size } : game::Vec3 {};
}

uint64_t packetMs(const uint8_t* data, std::size_t size)
{
	const int samples = opus_packet_get_nb_samples(data, static_cast<opus_int32>(size), vb::kFrequency);
	return samples > 0 ? static_cast<uint64_t>(samples) / (vb::kFrequency / 1000) : 20;
}

// Gain with a soft knee instead of hard clipping.
void amplify(int16_t* samples, int count, float gain)
{
	if (gain > 0.999f && gain < 1.001f)
	{
		return;
	}
	for (int i = 0; i < count; ++i)
	{
		float value = static_cast<float>(samples[i]) / 32768.f * gain;
		const float magnitude = std::fabs(value);
		if (magnitude > 0.8f)
		{
			value = std::copysign(0.8f + 0.2f * std::tanh((magnitude - 0.8f) / 0.2f), value);
		}
		samples[i] = static_cast<int16_t>(std::clamp(value * 32767.f, -32768.f, 32767.f));
	}
}

float defaultParameter(uint32_t parameter)
{
	switch (parameter)
	{
	case vb::param::volume:
	case vb::param::src:
		return 1.f;
	case vb::param::eaxmix:
		return -1.f;
	default:
		return 0.f;
	}
}
}

float Audio::Parameter::value(uint64_t time) const
{
	if (duration == 0 || time >= start + duration)
	{
		return to;
	}
	const float progress = static_cast<float>(time - start) / static_cast<float>(duration);
	return from + (to - from) * progress;
}

Audio& Audio::Get()
{
	static Audio instance;
	return instance;
}

bool Audio::init(HWND window)
{
	if (ready_)
	{
		return true;
	}
	if (!bass::Load() || !bass::EnsureOutput(window))
	{
		return false;
	}
	testStream_ = bass::Get().StreamCreate(vb::kFrequency, 1, 0, bass::kStreamProcPush, nullptr);
	cueStream_ = bass::Get().StreamCreate(vb::kFrequency, 1, 0, bass::kStreamProcPush, nullptr);
	if (cueStream_)
	{
		bass::Get().ChannelSetAttribute(cueStream_, bass::kAttribVol, 0.35f);
		bass::Get().ChannelPlay(cueStream_, FALSE);
	}
	ready_ = true;
	return true;
}

void Audio::shutdown()
{
	closeMicrophone();
	clearStreams();
	std::lock_guard<std::mutex> lock(captureMutex_);
	if (encoder_)
	{
		opus_encoder_destroy(encoder_);
		encoder_ = nullptr;
	}
	if (testStream_)
	{
		bass::Get().StreamFree(testStream_);
		testStream_ = 0;
	}
	if (cueStream_)
	{
		bass::Get().StreamFree(cueStream_);
		cueStream_ = 0;
	}
	ready_ = false;
}

// ---------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------

void Audio::createStream(uint32_t id, StreamKind kind, uint32_t color, const std::string& name, float distance, const game::Vec3& position,
	uint16_t target)
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto existing = streams_.find(id);
	if (existing != streams_.end())
	{
		for (auto& channel : existing->second.channels)
		{
			freeChannel(*channel);
		}
		streams_.erase(existing);
	}
	Stream& stream = streams_[id];
	stream.id = id;
	stream.kind = kind;
	stream.color = color;
	stream.name = name;
	stream.distance = distance;
	stream.position = position;
	stream.target = target;
	stream.sourceKnown = kind == StreamKind::Point;
}

void Audio::deleteStream(uint32_t id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const auto it = streams_.find(id);
	if (it == streams_.end())
	{
		return;
	}
	for (auto& channel : it->second.channels)
	{
		freeChannel(*channel);
	}
	streams_.erase(it);
}

void Audio::clearStreams()
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto& entry : streams_)
	{
		for (auto& channel : entry.second.channels)
		{
			freeChannel(*channel);
		}
	}
	streams_.clear();
	routes_.clear();
}

void Audio::setStreamDistance(uint32_t id, float distance)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (const auto it = streams_.find(id); it != streams_.end())
	{
		it->second.distance = distance;
	}
}

void Audio::setStreamPosition(uint32_t id, const game::Vec3& position)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (const auto it = streams_.find(id); it != streams_.end())
	{
		it->second.position = position;
		it->second.sourceKnown = true;
	}
}

void Audio::setStreamFlags(uint32_t id, uint32_t flags)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (const auto it = streams_.find(id); it != streams_.end())
	{
		it->second.flags = flags;
	}
}

void Audio::setSourcePosition(uint32_t id, const game::Vec3& position)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (const auto it = streams_.find(id); it != streams_.end() && it->second.kind != StreamKind::Point)
	{
		it->second.position = position;
		it->second.sourceKnown = true;
		it->second.sourceTime = now();
	}
}

void Audio::setParameter(uint32_t id, uint32_t parameter, float value)
{
	slideParameter(id, parameter, value, value, 0);
}

void Audio::slideParameter(uint32_t id, uint32_t parameter, float from, float to, uint32_t timeMs)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (const auto it = streams_.find(id); it != streams_.end())
	{
		it->second.parameters[parameter] = Parameter { from, to, now(), timeMs };
	}
}

void Audio::createEffect(uint32_t streamId, uint32_t effect, uint32_t number, int32_t priority, const uint8_t* params, std::size_t size)
{
	// BASS reads a fixed size structure for each effect type.
	static const std::size_t sizes[vb::effect::count] = { sizeof(vb::ChorusParams), sizeof(vb::CompressorParams),
		sizeof(vb::DistortionParams), sizeof(vb::EchoParams), sizeof(vb::FlangerParams), sizeof(vb::GargleParams),
		sizeof(vb::I3dl2reverbParams), sizeof(vb::ParameqParams), sizeof(vb::ReverbParams) };
	if (number >= vb::effect::count || size < sizes[number])
	{
		Log("ignored effect %u with %u parameter bytes", number, static_cast<unsigned>(size));
		return;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	const auto it = streams_.find(streamId);
	if (it == streams_.end())
	{
		return;
	}
	Stream& stream = it->second;
	EffectData& data = stream.effects[effect];
	data.number = number;
	data.priority = priority;
	data.params.assign(params, params + size);
	for (auto& channel : stream.channels)
	{
		applyEffect(*channel, effect, data);
	}
}

void Audio::deleteEffect(uint32_t streamId, uint32_t effect)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const auto it = streams_.find(streamId);
	if (it == streams_.end())
	{
		return;
	}
	for (auto& channel : it->second.channels)
	{
		if (const auto fx = channel->effects.find(effect); fx != channel->effects.end())
		{
			bass::Get().ChannelRemoveFX(channel->handle, fx->second);
			channel->effects.erase(fx);
		}
	}
	it->second.effects.erase(effect);
}

void Audio::applyEffect(Channel& channel, uint32_t id, const EffectData& effect)
{
	const auto& api = bass::Get();
	if (const auto existing = channel.effects.find(id); existing != channel.effects.end())
	{
		api.ChannelRemoveFX(channel.handle, existing->second);
		channel.effects.erase(existing);
	}
	const bass::HFX fx = api.ChannelSetFX(channel.handle, effect.number, effect.priority);
	if (!fx)
	{
		Log("could not create effect %u (BASS error %d)", effect.number, api.ErrorGetCode());
		return;
	}
	if (!effect.params.empty())
	{
		api.FXSetParameters(fx, effect.params.data());
	}
	channel.effects[id] = fx;
}

Audio::Channel* Audio::channelFor(Stream& stream, uint16_t speaker)
{
	for (auto& channel : stream.channels)
	{
		if (channel->speaker == speaker)
		{
			return channel.get();
		}
	}
	const auto& api = bass::Get();
	auto channel = std::make_unique<Channel>();
	channel->speaker = speaker;
	// Keeps BASS's default playback buffer: with a shorter one, bass.dll 2.4.7
	// never resumes a push stream that ran dry (the next utterance is lost).
	channel->handle = api.StreamCreate(vb::kFrequency, 2, 0, bass::kStreamProcPush, nullptr);
	channel->spatial = std::make_unique<Spatial>();
	if (channel->handle && api.ChannelSetDSP)
	{
		channel->dsp = api.ChannelSetDSP(channel->handle, &Audio::dspProc, channel.get(), kSpatialDspPriority) != 0;
	}
	int error = 0;
	channel->decoder = opus_decoder_create(vb::kFrequency, 1, &error);
	if (!channel->handle || !channel->decoder)
	{
		Log("could not create a voice channel (BASS error %d, Opus error %d)", api.ErrorGetCode(), error);
		freeChannel(*channel);
		return nullptr;
	}
	api.ChannelSetAttribute(channel->handle, bass::kAttribVol, 0.f);
	for (const auto& effect : stream.effects)
	{
		applyEffect(*channel, effect.first, effect.second);
	}
	stream.channels.push_back(std::move(channel));
	return stream.channels.back().get();
}

void Audio::freeChannel(Channel& channel)
{
	if (channel.handle)
	{
		bass::Get().StreamFree(channel.handle);
		channel.handle = 0;
	}
	if (channel.decoder)
	{
		opus_decoder_destroy(channel.decoder);
		channel.decoder = nullptr;
	}
	channel.effects.clear();
}

Audio::Spatial::Spatial()
{
	// Freeverb style comb and all-pass sizes, scaled to 48 kHz.  Left and
	// right differ slightly so the room sounds wide.
	static const int combSizes[4] = { 1214, 1293, 1239, 1318 };
	static const int allpassSizes[2] = { 605, 630 };
	for (int i = 0; i < 4; ++i)
	{
		comb[i].assign(static_cast<std::size_t>(combSizes[i]), 0.f);
	}
	for (int i = 0; i < 2; ++i)
	{
		allpass[i].assign(static_cast<std::size_t>(allpassSizes[i]), 0.f);
	}
}

bool Audio::duplicate(uint16_t sender, uint32_t packid, const uint8_t* data, std::size_t size, uint64_t time)
{
	// The same frame may arrive through two streams (the speaker talks in a
	// local and a global stream the listener hears): play it once.
	uint32_t hash = 2166136261u ^ packid;
	for (std::size_t i = 0; i < size; ++i)
	{
		hash = (hash ^ data[i]) * 16777619u;
	}
	for (const RecentPacket& recent : recent_)
	{
		if (recent.sender == sender && recent.hash == hash && time - recent.time < 1000)
		{
			return true;
		}
	}
	recent_[recentPos_] = { sender, hash, time };
	recentPos_ = (recentPos_ + 1) % recent_.size();
	return false;
}

void CALLBACK Audio::dspProc(DWORD, DWORD, void* buffer, DWORD length, void* user)
{
	spatialize(*static_cast<Channel*>(user), static_cast<int16_t*>(buffer), static_cast<int>(length / (2 * sizeof(int16_t))));
}

// Turns the (identical) left/right samples into a positioned stereo voice,
// in place.  Runs on BASS's playback thread with the latest placement.
void Audio::spatialize(Channel& channel, int16_t* stereo, int count)
{
	const Settings& settings = GetSettings();
	Spatial& state = *channel.spatial;
	const bool positioned = channel.placement.positioned.load(std::memory_order_relaxed);
	const int mode = positioned ? settings.spatialMode : 2;
	const float side = std::clamp(channel.placement.side.load(std::memory_order_relaxed), -1.f, 1.f);
	const float ratio = channel.placement.ratio.load(std::memory_order_relaxed);
	const float brightness = std::clamp(channel.placement.brightness.load(std::memory_order_relaxed), 0.f, 1.f);
	const float amount = std::fabs(side);

	// Distance: one pole low-pass, cutoff from 16 kHz down to about 1.8 kHz.
	const bool muffle = brightness < 0.98f;
	const float muffleCutoff = 1800.f + (16000.f - 1800.f) * brightness * brightness;
	const float muffleAlpha = 1.f - std::exp(-2.f * 3.14159265f * muffleCutoff / static_cast<float>(vb::kFrequency));

	// Targets for this block; the renderer glides to them sample by sample
	// so moving sources never click.
	const float targetFar = 1.f - 0.55f * amount;
	const float targetItd = amount * 26.f; // up to ~0.55 ms between the ears
	const float room = static_cast<float>(settings.roomAmount) / 100.f;
	const float targetRoom = mode == 0 ? room * (0.06f + 0.55f * std::pow(ratio, 1.2f)) : 0.f;
	const float shadowCutoff = 16000.f - 12500.f * amount;
	const float shadowAlpha = 1.f - std::exp(-2.f * 3.14159265f * shadowCutoff / static_cast<float>(vb::kFrequency));
	const float step = 1.f / static_cast<float>(std::max(count, 1));
	const float startFar = state.farGain;
	const float startItd = state.itd;
	const float startRoom = state.roomSend;

	// Equal power gains for the simple mode.
	const float angle = (side * 0.8f + 1.f) * 0.785398f;
	const float simpleLeft = std::min(1.f, std::cos(angle) * 1.4142f);
	const float simpleRight = std::min(1.f, std::sin(angle) * 1.4142f);

	for (int i = 0; i < count; ++i)
	{
		float x = (static_cast<float>(stereo[i * 2]) + static_cast<float>(stereo[i * 2 + 1])) * 0.5f;
		if (muffle)
		{
			state.lowpass += muffleAlpha * (x - state.lowpass);
			x = state.lowpass;
		}
		else
		{
			state.lowpass = x;
		}
		float left = x;
		float right = x;
		const float progress = static_cast<float>(i + 1) * step;

		if (mode == 1)
		{
			left = x * simpleLeft;
			right = x * simpleRight;
		}
		else if (mode == 0)
		{
			const float farGain = startFar + (targetFar - startFar) * progress;
			const float itd = startItd + (targetItd - startItd) * progress;
			const float roomSend = startRoom + (targetRoom - startRoom) * progress;

			state.delay[state.delayPos] = x;
			// The far ear hears the voice later, quieter and duller.
			float read = static_cast<float>(state.delayPos) - itd;
			while (read < 0.f)
			{
				read += Spatial::kDelaySize;
			}
			const int index = static_cast<int>(read);
			const float fraction = read - static_cast<float>(index);
			const float delayed = state.delay[index % Spatial::kDelaySize] * (1.f - fraction)
				+ state.delay[(index + 1) % Spatial::kDelaySize] * fraction;
			state.delayPos = (state.delayPos + 1) % Spatial::kDelaySize;
			state.farState += shadowAlpha * (delayed - state.farState);
			const float farSignal = state.farState * farGain;

			left = side >= 0.f ? farSignal : x;
			right = side >= 0.f ? x : farSignal;

			if (roomSend > 0.001f)
			{
				// Small room: four damped combs and an all-pass per ear.
				const float input = x * roomSend;
				float wet[2] = { 0.f, 0.f };
				for (int c = 0; c < 4; ++c)
				{
					std::vector<float>& buffer = state.comb[c];
					const float output = buffer[static_cast<std::size_t>(state.combPos[c])];
					state.combFilter[c] = output * 0.65f + state.combFilter[c] * 0.35f;
					buffer[static_cast<std::size_t>(state.combPos[c])] = input + state.combFilter[c] * 0.74f;
					state.combPos[c] = (state.combPos[c] + 1) % static_cast<int>(buffer.size());
					wet[c / 2] += output * 0.25f;
				}
				for (int a = 0; a < 2; ++a)
				{
					std::vector<float>& buffer = state.allpass[a];
					const float stored = buffer[static_cast<std::size_t>(state.allpassPos[a])];
					const float output = stored - wet[a];
					buffer[static_cast<std::size_t>(state.allpassPos[a])] = wet[a] + stored * 0.5f;
					state.allpassPos[a] = (state.allpassPos[a] + 1) % static_cast<int>(buffer.size());
					wet[a] = output;
				}
				const float dry = 1.f - 0.4f * roomSend;
				left = left * dry + wet[0];
				right = right * dry + wet[1];
			}
		}

		if (settings.swapChannels)
		{
			std::swap(left, right);
		}
		stereo[i * 2] = static_cast<int16_t>(std::clamp(left, -32768.f, 32767.f));
		stereo[i * 2 + 1] = static_cast<int16_t>(std::clamp(right, -32768.f, 32767.f));
	}
	state.farGain = targetFar;
	state.itd = targetItd;
	state.roomSend = targetRoom;
}

void Audio::pushVoice(uint32_t streamId, uint16_t sender, uint32_t packid, const uint8_t* data, std::size_t size)
{
	if (!ready_ || !size || size > vb::kMaxVoicePacketSize)
	{
		return;
	}
	const auto& api = bass::Get();
	std::lock_guard<std::mutex> lock(mutex_);
	if (sender < muted_.size() && muted_.test(sender))
	{
		return;
	}
	const auto it = streams_.find(streamId);
	if (it == streams_.end())
	{
		return;
	}
	const uint64_t t = now();
	// The same voice in two streams (e.g. local + radio) would play twice,
	// and split between both when packets arrive in a different order.
	// Checked before the duplicate filter: a rejected copy must not mark the
	// packet as already played, or the copy of the right stream is dropped.
	SpeakerRoute& route = routes_[sender];
	if (route.stream != streamId && route.time && t - route.time < kRouteHoldMs && streams_.count(route.stream))
	{
		return;
	}
	if (duplicate(sender, packid, data, size, t))
	{
		return;
	}
	route.stream = streamId;
	route.time = t;
	Channel* channel = channelFor(it->second, sender);
	if (!channel)
	{
		return;
	}

	// Only the part above 100% is applied here; update() handles the rest
	// through the channel volume.
	const float master = static_cast<float>(GetSettings().volume) / 100.f;
	const float preGain = std::min(kBaseGain * std::max(1.f, master) * std::max(1.f, playerVolumeLocked(sender)), kMaxPreGain);
	int16_t pcm[kMaxFrameSamples];
	int16_t stereo[kMaxFrameSamples * 2];
	const auto put = [&](int samples)
	{
		if (channel->playing)
		{
			// Too much waiting to be played (network burst, clock drift):
			// skip this frame instead of letting the delay grow.
			const DWORD waiting = api.StreamPutData(channel->handle, nullptr, 0);
			if (waiting != static_cast<DWORD>(-1) && waiting > kMaxQueuedBytes)
			{
				return;
			}
		}
		amplify(pcm, samples, preGain);
		for (int i = 0; i < samples; ++i)
		{
			stereo[i * 2] = stereo[i * 2 + 1] = pcm[i];
		}
		if (!channel->dsp)
		{
			spatialize(*channel, stereo, samples);
		}
		const DWORD bytes = static_cast<DWORD>(samples * 2 * sizeof(int16_t));
		api.StreamPutData(channel->handle, stereo, bytes);
		channel->queued += bytes;
	};

	const bool restart = !channel->initialized || packid == 0 || t - channel->lastPacket > kRestartGapMs;
	if (restart)
	{
		// Never rewind the stream (that replayed the last words on some BASS
		// builds): keep what is still playing and only re-buffer when the
		// channel ran dry.
		opus_decoder_ctl(channel->decoder, OPUS_RESET_STATE);
		channel->initialized = true;
		channel->utteranceStart = t;
		channel->utteranceFirst = packid;
		if (!channel->playing || api.ChannelIsActive(channel->handle) != bass::kActivePlaying)
		{
			if (channel->playing)
			{
				api.ChannelPause(channel->handle);
			}
			channel->playing = false;
			channel->queued = 0;
		}
	}
	else if (packid < channel->expected)
	{
		return; // late or duplicated
	}
	else if (packid > channel->expected
		&& static_cast<uint64_t>(packid - channel->utteranceFirst) * packetMs(data, size) > t - channel->utteranceStart + kStaleToleranceMs)
	{
		// From an earlier utterance, delayed by the network: playing it
		// would mix old and new speech.
		if (stalePackets_++ == 0)
		{
			Log("dropping late voice packets from an earlier utterance");
		}
		return;
	}
	else if (packid > channel->expected && packid - channel->expected <= 3)
	{
		// One packet was lost: rebuild it from the in-band FEC of this one.
		const int lost = opus_packet_get_nb_samples(data, static_cast<opus_int32>(size), vb::kFrequency);
		if (lost > 0 && lost <= kMaxFrameSamples)
		{
			const int decoded = opus_decode(channel->decoder, data, static_cast<opus_int32>(size), pcm, lost, 1);
			if (decoded > 0)
			{
				put(decoded);
			}
		}
	}

	const int decoded = opus_decode(channel->decoder, data, static_cast<opus_int32>(size), pcm, kMaxFrameSamples, 0);
	if (decoded <= 0)
	{
		return;
	}
	channel->level = levelDb(pcm, static_cast<std::size_t>(decoded));
	put(decoded);
	channel->expected = packid + 1;
	channel->lastPacket = t;

	if (!channel->playing)
	{
		// 1.5 frames (at least 60 ms) of buffer absorbs normal network jitter.
		const DWORD frameMs = static_cast<DWORD>(decoded / (vb::kFrequency / 1000));
		const DWORD prebufferMs = std::max<DWORD>(60, frameMs + frameMs / 2);
		// Counted locally: BASS_DATA_AVAILABLE does not report the queue of a
		// push stream that is not playing on every BASS version.
		if (channel->queued >= prebufferMs * (vb::kFrequency / 1000) * 2 * sizeof(int16_t))
		{
			if (api.ChannelPlay(channel->handle, FALSE))
			{
				channel->playing = true;
				if (!firstPlayLogged_)
				{
					firstPlayLogged_ = true;
					Log("first voice channel playing");
				}
			}
			else
			{
				Log("could not play a voice channel (BASS error %d)", api.ErrorGetCode());
			}
		}
	}
}

void Audio::playCue(bool start)
{
	if (!cueStream_)
	{
		return;
	}
	// 45 ms tone: rising when the microphone opens, lower when it closes.
	constexpr int kSamples = 48 * 45;
	int16_t tone[kSamples];
	const float frequency = start ? 1180.f : 760.f;
	for (int i = 0; i < kSamples; ++i)
	{
		const float envelope = std::min(1.f, std::min(i, kSamples - i) / 240.f);
		tone[i] = static_cast<int16_t>(9000.f * envelope * std::sin(2.f * 3.14159265f * frequency * static_cast<float>(i) / 48000.f));
	}
	bass::Get().StreamPutData(cueStream_, tone, sizeof(tone));
}

bool Audio::resolveSource(Stream& stream, bool serverPositions, game::Vec3& out)
{
	switch (stream.kind)
	{
	case StreamKind::Point:
		out = stream.position;
		return true;
	case StreamKind::Global:
		return false;
	default:
		break;
	}
	if (serverPositions)
	{
		out = stream.position;
		return stream.sourceKnown;
	}
	float position[3];
	bool found = false;
	if (stream.kind == StreamKind::Player)
	{
		found = samp::PlayerPosition(stream.target, position);
	}
	else if (stream.kind == StreamKind::Vehicle)
	{
		found = samp::VehiclePosition(stream.target, position);
	}
	else
	{
		found = samp::ObjectPosition(stream.target, position);
	}
	if (found)
	{
		stream.position = { position[0], position[1], position[2] };
		stream.sourceKnown = true;
	}
	out = stream.position;
	return stream.sourceKnown;
}

void Audio::update(const game::Listener& listener, bool serverPositions)
{
	if (!ready_)
	{
		return;
	}
	const auto& api = bass::Get();
	const Settings& settings = GetSettings();
	const float master = settings.soundEnabled ? static_cast<float>(settings.volume) / 100.f : 0.f;
	const uint64_t t = now();
	const game::Vec3 right = normalize(cross(listener.front, listener.up));
	const game::Vec3 front = normalize(listener.front);

	// SA-MP ties BASS's global stream volume to the GTA radio volume; at 0 the
	// voice would be silent too.  Voice chat keeps it at the maximum.
	const DWORD streamVolume = api.GetConfig(bass::kConfigGlobalStreamVolume);
	if (streamVolume != static_cast<DWORD>(-1) && streamVolume < 10000)
	{
		if (streamVolume != loggedStreamVolume_)
		{
			loggedStreamVolume_ = streamVolume;
			Log("BASS stream volume was %u/10000 (GTA radio volume); raised for voice chat", streamVolume);
		}
		api.SetConfig(bass::kConfigGlobalStreamVolume, 10000);
	}

	std::lock_guard<std::mutex> lock(mutex_);
	for (auto& entry : streams_)
	{
		Stream& stream = entry.second;

		float spatialGain = 1.f;
		float spatialPan = 0.f;
		float brightness = 1.f;
		game::Vec3 source;
		float sourceRatio = 0.f;
		const bool flat = (stream.flags & (vb::streamflag::forceFlat | vb::streamflag::sourceIsListener)) != 0;
		const bool resolved = !flat && listener.valid && resolveSource(stream, serverPositions, source);
		if (resolved)
		{
			const game::Vec3 offset = sub(source, listener.position);
			const float distance = length(offset);
			const float maxDistance = std::max(stream.distance, 0.1f);
			const float ratio = std::clamp(distance / maxDistance, 0.f, 1.f);
			sourceRatio = ratio;
			if (distance >= maxDistance)
			{
				spatialGain = 0.f;
			}
			else
			{
				// Inverse distance (about -6 dB per doubling) from a 1.5 m
				// reference, plus a smooth fade over the last 40% of the range
				// so voices disappear instead of being cut.
				const float reference = std::min(1.5f, maxDistance * 0.25f);
				const float rolloff = 0.8f * static_cast<float>(settings.distanceStrength) / 100.f;
				spatialGain = distance <= reference ? 1.f : reference / (reference + rolloff * (distance - reference));
				const float fade = std::clamp((ratio - 0.6f) / 0.4f, 0.f, 1.f);
				spatialGain *= 1.f - fade * fade * (3.f - 2.f * fade);
				// Distant voices lose their high frequencies.
				brightness = 1.f - ratio * ratio * 0.85f;
			}
			if (distance > 0.3f)
			{
				// Close voices come clearly from one side; far ones are more
				// diffuse.  Never fully on one ear.
				const game::Vec3 direction = normalize(offset);
				spatialPan = dot(direction, right) * (1.f - 0.35f * ratio);
				const float facing = dot(direction, front);
				stream.facing = facing;
				if (facing < 0.f)
				{
					// Behind the listener: quieter and muffled (head shadow).
					spatialGain *= 1.f + 0.25f * facing;
					brightness *= 1.f + 0.3f * facing;
				}
			}
		}
		stream.brightness = settings.spatialMode == 2 ? 1.f : brightness;
		stream.positioned = resolved && spatialGain > 0.f;
		stream.side = spatialPan;
		stream.ratio = sourceRatio;
		// Panning is rendered into the samples (ears, room); the channel
		// balance only carries the script's panning parameter.
		spatialPan = 0.f;

		const auto parameter = [&](uint32_t id)
		{
			const auto it = stream.parameters.find(id);
			return it == stream.parameters.end() ? defaultParameter(id) : it->second.value(t);
		};
		const float volume = parameter(vb::param::volume);
		const float pan = std::clamp(parameter(vb::param::panning) + spatialPan, -1.f, 1.f);
		const float frequency = parameter(vb::param::frequency);
		stream.gain = spatialGain;
		stream.pan = pan;

		for (auto it = stream.channels.begin(); it != stream.channels.end();)
		{
			Channel& channel = **it;
			if (t - channel.lastPacket > kChannelIdleMs)
			{
				freeChannel(channel);
				it = stream.channels.erase(it);
				continue;
			}
			channel.placement.side.store(stream.side, std::memory_order_relaxed);
			channel.placement.ratio.store(stream.ratio, std::memory_order_relaxed);
			channel.placement.brightness.store(stream.brightness, std::memory_order_relaxed);
			channel.placement.positioned.store(stream.positioned, std::memory_order_relaxed);
			// Volumes above 100% are applied to the samples in pushVoice.
			const float personal = std::min(1.f, playerVolumeLocked(channel.speaker));
			const float gain = std::clamp(volume * spatialGain * std::min(master, 1.f) * personal, 0.f, 1.f);
			if (!api.ChannelSetAttribute(channel.handle, bass::kAttribVol, gain) || !api.ChannelSetAttribute(channel.handle, bass::kAttribPan, pan))
			{
				++attributeErrors_;
			}
			if (frequency != channel.appliedFrequency)
			{
				api.ChannelSetAttribute(channel.handle, bass::kAttribFreq, frequency > 0.f ? frequency : 0.f);
				channel.appliedFrequency = frequency;
			}
			++it;
		}
	}
}

std::vector<DWORD> Audio::channelHandles()
{
	std::vector<DWORD> handles;
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto& entry : streams_)
	{
		for (const auto& channel : entry.second.channels)
		{
			handles.push_back(channel->handle);
		}
	}
	return handles;
}

std::vector<SpeakerInfo> Audio::speakers()
{
	std::vector<SpeakerInfo> result;
	const uint64_t t = now();
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto& entry : streams_)
	{
		const Stream& stream = entry.second;
		for (const auto& channel : stream.channels)
		{
			if (t - channel->lastPacket > kSpeakingWindowMs || stream.gain <= 0.001f)
			{
				continue;
			}
			const bool known = std::any_of(result.begin(), result.end(), [&](const SpeakerInfo& info) { return info.player == channel->speaker; });
			if (!known)
			{
				result.push_back({ channel->speaker, stream.color, stream.name, channel->level });
			}
		}
	}
	return result;
}

void Audio::setPlayerVolume(uint16_t player, float volume)
{
	std::lock_guard<std::mutex> lock(mutex_);
	playerVolume_[player] = std::clamp(volume, 0.f, 3.f);
}

float Audio::playerVolumeLocked(uint16_t player) const
{
	float volume = 1.f;
	if (const auto own = playerVolume_.find(player); own != playerVolume_.end())
	{
		volume *= own->second;
	}
	if (const auto server = serverVolume_.find(player); server != serverVolume_.end())
	{
		volume *= server->second;
	}
	return volume;
}

float Audio::playerVolume(uint16_t player) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	const auto it = playerVolume_.find(player);
	return it == playerVolume_.end() ? 1.f : it->second;
}

void Audio::setServerPlayerVolume(uint16_t player, float volume)
{
	std::lock_guard<std::mutex> lock(mutex_);
	serverVolume_[player] = std::clamp(volume, 0.f, 4.f);
}

void Audio::setPlayerMuted(uint16_t player, bool muted)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (player < muted_.size())
	{
		muted_.set(player, muted);
	}
}

bool Audio::playerMuted(uint16_t player) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return player < muted_.size() && muted_.test(player);
}

void Audio::clearPlayerSettings()
{
	std::lock_guard<std::mutex> lock(mutex_);
	playerVolume_.clear();
	serverVolume_.clear();
	muted_.reset();
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

std::vector<std::string> Audio::microphones()
{
	std::vector<std::string> names;
	if (!bass::Loaded())
	{
		return names;
	}
	bass::DeviceInfo info {};
	for (DWORD device = 0; bass::Get().RecordGetDeviceInfo(device, &info); ++device)
	{
		if ((info.flags & bass::kDeviceEnabled) && !(info.flags & bass::kDeviceLoopback) && info.name)
		{
			names.emplace_back(info.name);
		}
	}
	return names;
}

std::string Audio::microphoneName() const
{
	return recordName_;
}

bool Audio::openMicrophone(const std::string& wanted)
{
	if (!bass::Loaded())
	{
		return false;
	}
	closeMicrophone();
	const auto& api = bass::Get();

	int chosen = -1;
	int fallback = -1;
	std::string chosenName;
	bass::DeviceInfo info {};
	for (DWORD device = 0; api.RecordGetDeviceInfo(device, &info); ++device)
	{
		if (!(info.flags & bass::kDeviceEnabled) || (info.flags & bass::kDeviceLoopback) || !info.name)
		{
			continue;
		}
		if (!wanted.empty() && wanted == info.name)
		{
			chosen = static_cast<int>(device);
			chosenName = info.name;
			break;
		}
		if (fallback < 0 || (info.flags & bass::kDeviceDefault))
		{
			fallback = static_cast<int>(device);
			chosenName = info.name;
		}
	}
	if (chosen < 0)
	{
		chosen = fallback;
	}
	if (chosen < 0)
	{
		Log("no microphone found");
		return false;
	}
	if (!api.RecordInit(chosen) && api.ErrorGetCode() != bass::kErrorAlready)
	{
		Log("could not open microphone '%s' (BASS error %d)", chosenName.c_str(), api.ErrorGetCode());
		return false;
	}
	api.RecordSetDevice(static_cast<DWORD>(chosen));
	recordDevice_ = chosen;
	recordName_ = chosenName;
	startRecording();
	if (!recordHandle_)
	{
		api.RecordFree();
		recordDevice_ = -1;
		return false;
	}
	Log("microphone: %s", recordName_.c_str());
	return true;
}

void Audio::startRecording()
{
	const auto& api = bass::Get();
	{
		std::lock_guard<std::mutex> lock(captureMutex_);
		pending_.clear();
	}
	// 20 ms recording period; frames are assembled in onCapture.
	recordHandle_ = api.RecordStart(vb::kFrequency, 1, MAKELONG(bass::kRecordPause, 20), &Audio::recordProc, this);
	recordActive_ = false;
	if (!recordHandle_)
	{
		Log("could not start recording (BASS error %d)", api.ErrorGetCode());
	}
}

void Audio::closeMicrophone()
{
	if (!bass::Loaded() || recordDevice_ < 0)
	{
		return;
	}
	const auto& api = bass::Get();
	if (recordHandle_)
	{
		api.ChannelStop(recordHandle_);
		recordHandle_ = 0;
		recordActive_ = false;
	}
	api.RecordSetDevice(static_cast<DWORD>(recordDevice_));
	api.RecordFree();
	recordDevice_ = -1;
	recordName_.clear();
}

void Audio::configureEncoder(uint32_t bitrate, uint32_t frameMs)
{
	std::lock_guard<std::mutex> lock(captureMutex_);
	bitrate_ = std::clamp<uint32_t>(bitrate, 6000, 128000);
	frameMs_ = (frameMs == 20 || frameMs == 40 || frameMs == 60 || frameMs == 100) ? frameMs : 100;
	if (!encoder_)
	{
		int error = 0;
		encoder_ = opus_encoder_create(vb::kFrequency, 1, OPUS_APPLICATION_VOIP, &error);
		if (!encoder_)
		{
			Log("could not create the Opus encoder (%d)", error);
			return;
		}
	}
	opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(static_cast<opus_int32>(bitrate_)));
	opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
	opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(8));
	opus_encoder_ctl(encoder_, OPUS_SET_INBAND_FEC(1));
	opus_encoder_ctl(encoder_, OPUS_SET_PACKET_LOSS_PERC(10));
	opus_encoder_ctl(encoder_, OPUS_SET_DTX(0));
	opus_encoder_ctl(encoder_, OPUS_RESET_STATE);
	pending_.clear();
	packid_ = 0;
}

void Audio::setRecordingActive(bool active)
{
	if (!recordHandle_ || active == recordActive_)
	{
		return;
	}
	recordActive_ = active;
	if (active)
	{
		// A fresh recording instead of resuming the paused one: a resumed
		// bass.dll recording delivers what it captured while paused, about a
		// second of old audio.  This only happens when a session starts or
		// after a mute, so its few ms of start-up are not noticed.
		const auto& api = bass::Get();
		api.ChannelStop(recordHandle_);
		if (recordDevice_ >= 0)
		{
			api.RecordSetDevice(static_cast<DWORD>(recordDevice_));
		}
		{
			std::lock_guard<std::mutex> lock(captureMutex_);
			pending_.clear();
		}
		recordHandle_ = api.RecordStart(vb::kFrequency, 1, MAKELONG(0, 20), &Audio::recordProc, this);
		if (!recordHandle_)
		{
			Log("could not restart recording (BASS error %d)", api.ErrorGetCode());
			startRecording(); // keep a paused handle so the microphone stays known
		}
	}
	else
	{
		bass::Get().ChannelPause(recordHandle_);
		micLevel_ = -90.f;
		voiceDetected_ = false;
	}
}

void Audio::setTransmitting(bool transmitting)
{
	transmitting_ = transmitting;
}

void Audio::setMicTest(bool enabled)
{
	micTest_ = enabled;
	if (testStream_)
	{
		const auto& api = bass::Get();
		if (enabled)
		{
			api.ChannelSetPosition(testStream_, 0, bass::kPosByte);
			api.ChannelSetAttribute(testStream_, bass::kAttribVol, 1.f);
			api.ChannelPlay(testStream_, TRUE);
		}
		else
		{
			api.ChannelStop(testStream_);
		}
	}
}

void Audio::setVoiceActivation(bool enabled)
{
	voiceActivation_ = enabled;
	if (!enabled)
	{
		voiceDetected_ = false;
	}
}

void Audio::setFrameSink(FrameSink sink)
{
	std::lock_guard<std::mutex> lock(captureMutex_);
	sink_ = std::move(sink);
}

BOOL CALLBACK Audio::recordProc(DWORD, const void* buffer, DWORD length, void* user)
{
	auto* audio = static_cast<Audio*>(user);
	audio->onCapture(static_cast<const int16_t*>(buffer), length / sizeof(int16_t));
	return TRUE;
}

void Audio::onCapture(const int16_t* samples, std::size_t count)
{
	const Settings& settings = GetSettings();
	std::lock_guard<std::mutex> lock(captureMutex_);
	if (!encoder_)
	{
		return;
	}
	const std::size_t frameSamples = frameMs_ * (vb::kFrequency / 1000);
	const float gain = static_cast<float>(settings.micGain) / 100.f;
	capturedSamples_ += count;
	for (std::size_t i = 0; i < count; ++i)
	{
		const float value = static_cast<float>(samples[i]) * gain;
		pending_.push_back(static_cast<int16_t>(std::clamp(value, -32768.f, 32767.f)));
	}

	while (pending_.size() >= frameSamples)
	{
		const uint64_t t = now();
		const float level = levelDb(pending_.data(), frameSamples);
		micLevel_ = level;

		if (micTest_ && testStream_)
		{
			bass::Get().StreamPutData(testStream_, pending_.data(), static_cast<DWORD>(frameSamples * sizeof(int16_t)));
		}
		if (voiceActivation_)
		{
			if (level >= static_cast<float>(settings.voiceActivationLevel))
			{
				voiceUntil_ = t + kVoiceHangoverMs;
			}
			voiceDetected_ = t < voiceUntil_;
		}

		const bool transmit = transmitting_ && settings.micEnabled && !micTest_;
		if (!transmit)
		{
			if (wasTransmitting_)
			{
				opus_encoder_ctl(encoder_, OPUS_RESET_STATE);
				packid_ = 0;
				wasTransmitting_ = false;
			}
		}
		else
		{
			wasTransmitting_ = true;
			if (level >= static_cast<float>(settings.noiseGate))
			{
				gateOpenUntil_ = t + kGateHangoverMs;
			}
			if (settings.noiseGate <= -90 || t < gateOpenUntil_)
			{
				uint8_t packet[vb::kMaxVoicePacketSize - sizeof(vb::VoiceHeader)];
				const int size = opus_encode(encoder_, pending_.data(), static_cast<int>(frameSamples), packet, static_cast<opus_int32>(sizeof(packet)));
				if (size > 0 && sink_)
				{
					sink_(packet, static_cast<std::size_t>(size), packid_++);
				}
			}
		}
		pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(frameSamples));
	}
}
}
