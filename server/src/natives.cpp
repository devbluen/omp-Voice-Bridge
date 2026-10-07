/*
 *  Voice Bridge for open.mp and SA-MP
 *
 *  Pawn natives.  The Sv* natives keep the exact names, parameters and
 *  return values of SampVoice 3.1, so gamemodes compiled against
 *  sampvoice.inc keep working without being recompiled.  The VB_* natives
 *  are the extended API.
 */

#include "pawn-host.hpp"
#include "log.hpp"
#include "version.hpp"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace vbs
{
namespace
{
VoiceServer& server()
{
	return VoiceServer::Get();
}

float toFloat(cell value)
{
	float result;
	std::memcpy(&result, &value, sizeof(result));
	return result;
}

cell fromFloat(float value)
{
	cell result;
	std::memcpy(&result, &value, sizeof(result));
	return result;
}

std::string getString(AMX* amx, cell address)
{
	cell* physical = nullptr;
	if (amx_GetAddr(amx, address, &physical) != AMX_ERR_NONE || !physical)
	{
		return {};
	}
	int length = 0;
	amx_StrLen(physical, &length);
	if (length <= 0)
	{
		return {};
	}
	std::vector<char> buffer(static_cast<std::size_t>(length) + 1, '\0');
	amx_GetString(buffer.data(), physical, 0, buffer.size());
	return std::string(buffer.data());
}

bool setReference(AMX* amx, cell address, cell value)
{
	cell* physical = nullptr;
	if (amx_GetAddr(amx, address, &physical) != AMX_ERR_NONE || !physical)
	{
		return false;
	}
	*physical = value;
	return true;
}

bool hasParams(const cell* params, int count, const char* name)
{
	if (params[0] >= count * static_cast<cell>(sizeof(cell)))
	{
		return true;
	}
	LogWarning("%s: expected %d parameters, got %d (outdated include?)", name, count, static_cast<int>(params[0] / sizeof(cell)));
	return false;
}

uint16_t playerId(cell value)
{
	return value < 0 || value >= kMaxPlayers ? vb::kNonePlayer : static_cast<uint16_t>(value);
}

uint32_t handle(cell value)
{
	return static_cast<uint32_t>(value);
}

bool vehicleExists(cell vehicle)
{
	Pose pose;
	return vehicle >= 0 && vehicle < 0xFFFF && server().world() && server().world()->vehiclePose(static_cast<uint16_t>(vehicle), pose);
}

bool objectExists(cell object)
{
	Pose pose;
	return object >= 0 && object < 0xFFFF && server().world() && server().world()->objectPose(static_cast<uint16_t>(object), pose);
}

cell createAt(AMX* amx, StreamType type, float distance, uint32_t maxListeners, cell target, uint32_t color, const std::string& name)
{
	switch (StreamTarget(type))
	{
	case TargetKind::Player:
	{
		// Any connected player, NPCs included (SampVoice allowed them).
		Pose pose;
		const uint16_t id = playerId(target);
		if (id == vb::kNonePlayer || (!server().isTracked(id) && !(server().world() && server().world()->playerPose(id, pose))))
		{
			return 0;
		}
	}
		break;
	case TargetKind::Vehicle:
		if (!vehicleExists(target))
		{
			return 0;
		}
		break;
	case TargetKind::Object:
		if (!objectExists(target))
		{
			return 0;
		}
		break;
	default:
		break;
	}
	return static_cast<cell>(server().createStream(type, distance, maxListeners, vb::Vec3 {}, static_cast<uint16_t>(target), color, name, amx));
}

template <typename T>
cell makeEffect(AMX* amx, uint32_t number, cell priority, const T& params)
{
	return static_cast<cell>(server().createEffect(number, static_cast<int32_t>(priority), &params, sizeof(params), amx));
}

template <typename T>
EffectItem item(uint32_t number, int32_t priority, const T& params)
{
	EffectItem result;
	result.number = number;
	result.priority = priority;
	const auto* bytes = reinterpret_cast<const uint8_t*>(&params);
	result.params.assign(bytes, bytes + sizeof(params));
	return result;
}

enum Preset
{
	PresetRadio = 1,
	PresetPhone,
	PresetMegaphone,
	PresetHall,
	PresetCave,
	PresetUnderwater,
	PresetRobot,
	PresetEcho,
	PresetWalkieTalkie,
};

std::vector<EffectItem> presetItems(cell preset, int32_t priority)
{
	using namespace vb;
	switch (preset)
	{
	case PresetRadio:
		return {
			item(effect::distortion, priority, DistortionParams { -18.f, 15.f, 2400.f, 2400.f, 3800.f }),
			item(effect::parameq, priority + 1, ParameqParams { 1200.f, 12.f, 8.f }),
			item(effect::compressor, priority + 2, CompressorParams { 6.f, 5.f, 200.f, -20.f, 4.f, 2.f }),
		};
	case PresetPhone:
		return {
			item(effect::parameq, priority, ParameqParams { 1800.f, 24.f, 12.f }),
			item(effect::parameq, priority + 1, ParameqParams { 250.f, 18.f, -15.f }),
			item(effect::distortion, priority + 2, DistortionParams { -30.f, 6.f, 1800.f, 1600.f, 3400.f }),
		};
	case PresetMegaphone:
		return {
			item(effect::distortion, priority, DistortionParams { -8.f, 45.f, 2000.f, 2400.f, 6000.f }),
			item(effect::parameq, priority + 1, ParameqParams { 2200.f, 16.f, 10.f }),
			item(effect::echo, priority + 2, EchoParams { 12.f, 20.f, 90.f, 110.f, 0 }),
		};
	case PresetHall:
		return { item(effect::reverb, priority, ReverbParams { 0.f, -4.f, 2200.f, 0.4f }) };
	case PresetCave:
		return {
			item(effect::i3dl2reverb, priority, I3dl2reverbParams { -1000, -100, 0.f, 2.91f, 1.3f, -602, 0.015f, -302, 0.022f, 100.f, 100.f, 5000.f }),
			item(effect::echo, priority + 1, EchoParams { 20.f, 35.f, 300.f, 340.f, 1 }),
		};
	case PresetUnderwater:
		return {
			item(effect::parameq, priority, ParameqParams { 350.f, 36.f, 15.f }),
			item(effect::parameq, priority + 1, ParameqParams { 4000.f, 36.f, -15.f }),
			item(effect::chorus, priority + 2, ChorusParams { 40.f, 20.f, 10.f, 0.8f, 1, 12.f, 2 }),
		};
	case PresetRobot:
		return {
			item(effect::flanger, priority, FlangerParams { 60.f, 80.f, 70.f, 6.f, 1, 2.f, 2 }),
			item(effect::gargle, priority + 1, GargleParams { 40, 0 }),
		};
	case PresetEcho:
		return { item(effect::echo, priority, EchoParams { 35.f, 40.f, 250.f, 250.f, 0 }) };
	case PresetWalkieTalkie:
		return {
			item(effect::distortion, priority, DistortionParams { -12.f, 30.f, 2200.f, 1800.f, 3200.f }),
			item(effect::parameq, priority + 1, ParameqParams { 2000.f, 20.f, 12.f }),
			item(effect::parameq, priority + 2, ParameqParams { 300.f, 24.f, -15.f }),
		};
	default:
		return {};
	}
}

#define NATIVE(name) cell AMX_NATIVE_CALL name(AMX* amx, cell* params)
#define REQUIRE(count) \
	do \
	{ \
		if (!hasParams(params, count, __func__)) \
			return 0; \
	} while (0)
#define UNUSED_AMX (void)amx

// ---------------------------------------------------------------------------
// SampVoice 3.1 compatible natives
// ---------------------------------------------------------------------------

NATIVE(n_SvDebug)
{
	UNUSED_AMX;
	REQUIRE(1);
	LogSetDebug(params[1] != 0);
	return 1;
}

NATIVE(n_SvInit)
{
	UNUSED_AMX;
	REQUIRE(1);
	server().setBitrate(static_cast<uint32_t>(params[1]));
	return 1;
}

NATIVE(n_SvGetVersion)
{
	UNUSED_AMX;
	REQUIRE(1);
	return server().clientVersion(playerId(params[1]));
}

NATIVE(n_SvHasMicro)
{
	UNUSED_AMX;
	REQUIRE(1);
	return server().hasMicro(playerId(params[1]));
}

NATIVE(n_SvStartRecord)
{
	UNUSED_AMX;
	REQUIRE(1);
	return server().startRecord(playerId(params[1]));
}

NATIVE(n_SvStopRecord)
{
	UNUSED_AMX;
	REQUIRE(1);
	return server().stopRecord(playerId(params[1]));
}

NATIVE(n_SvAddKey)
{
	UNUSED_AMX;
	REQUIRE(2);
	return server().addKey(playerId(params[1]), static_cast<uint8_t>(params[2]));
}

NATIVE(n_SvHasKey)
{
	UNUSED_AMX;
	REQUIRE(2);
	return server().hasKey(playerId(params[1]), static_cast<uint8_t>(params[2]));
}

NATIVE(n_SvRemoveKey)
{
	UNUSED_AMX;
	REQUIRE(2);
	return server().removeKey(playerId(params[1]), static_cast<uint8_t>(params[2]));
}

NATIVE(n_SvRemoveAllKeys)
{
	UNUSED_AMX;
	REQUIRE(1);
	server().removeAllKeys(playerId(params[1]));
	return 1;
}

NATIVE(n_SvMutePlayerStatus)
{
	UNUSED_AMX;
	REQUIRE(1);
	return server().isMuted(playerId(params[1]));
}

NATIVE(n_SvMutePlayerEnable)
{
	UNUSED_AMX;
	REQUIRE(1);
	server().setMuted(playerId(params[1]), true);
	return 1;
}

NATIVE(n_SvMutePlayerDisable)
{
	UNUSED_AMX;
	REQUIRE(1);
	server().setMuted(playerId(params[1]), false);
	return 1;
}

NATIVE(n_SvCreateGStream)
{
	REQUIRE(2);
	return static_cast<cell>(server().createStream(StreamType::Global, 0.f, kInfiniteListeners, vb::Vec3 {}, vb::kNonePlayer,
		static_cast<uint32_t>(params[1]), getString(amx, params[2]), amx));
}

NATIVE(n_SvCreateSLStreamAtPoint)
{
	REQUIRE(6);
	const vb::Vec3 position { toFloat(params[2]), toFloat(params[3]), toFloat(params[4]) };
	return static_cast<cell>(server().createStream(StreamType::StaticPoint, toFloat(params[1]), kInfiniteListeners, position, vb::kNonePlayer,
		static_cast<uint32_t>(params[5]), getString(amx, params[6]), amx));
}

NATIVE(n_SvCreateSLStreamAtVehicle)
{
	REQUIRE(4);
	return createAt(amx, StreamType::StaticVehicle, toFloat(params[1]), kInfiniteListeners, params[2], static_cast<uint32_t>(params[3]), getString(amx, params[4]));
}

NATIVE(n_SvCreateSLStreamAtPlayer)
{
	REQUIRE(4);
	return createAt(amx, StreamType::StaticPlayer, toFloat(params[1]), kInfiniteListeners, params[2], static_cast<uint32_t>(params[3]), getString(amx, params[4]));
}

NATIVE(n_SvCreateSLStreamAtObject)
{
	REQUIRE(4);
	return createAt(amx, StreamType::StaticObject, toFloat(params[1]), kInfiniteListeners, params[2], static_cast<uint32_t>(params[3]), getString(amx, params[4]));
}

NATIVE(n_SvCreateDLStreamAtPoint)
{
	REQUIRE(7);
	const vb::Vec3 position { toFloat(params[3]), toFloat(params[4]), toFloat(params[5]) };
	return static_cast<cell>(server().createStream(StreamType::DynamicPoint, toFloat(params[1]), static_cast<uint32_t>(params[2]), position,
		vb::kNonePlayer, static_cast<uint32_t>(params[6]), getString(amx, params[7]), amx));
}

NATIVE(n_SvCreateDLStreamAtVehicle)
{
	REQUIRE(5);
	return createAt(amx, StreamType::DynamicVehicle, toFloat(params[1]), static_cast<uint32_t>(params[2]), params[3], static_cast<uint32_t>(params[4]), getString(amx, params[5]));
}

NATIVE(n_SvCreateDLStreamAtPlayer)
{
	REQUIRE(5);
	return createAt(amx, StreamType::DynamicPlayer, toFloat(params[1]), static_cast<uint32_t>(params[2]), params[3], static_cast<uint32_t>(params[4]), getString(amx, params[5]));
}

NATIVE(n_SvCreateDLStreamAtObject)
{
	REQUIRE(5);
	return createAt(amx, StreamType::DynamicObject, toFloat(params[1]), static_cast<uint32_t>(params[2]), params[3], static_cast<uint32_t>(params[4]), getString(amx, params[5]));
}

NATIVE(n_SvUpdateDistanceForLStream)
{
	UNUSED_AMX;
	REQUIRE(2);
	server().setStreamDistance(handle(params[1]), toFloat(params[2]));
	return 1;
}

NATIVE(n_SvUpdatePositionForLPStream)
{
	UNUSED_AMX;
	REQUIRE(4);
	server().setStreamPosition(handle(params[1]), vb::Vec3 { toFloat(params[2]), toFloat(params[3]), toFloat(params[4]) });
	return 1;
}

NATIVE(n_SvAttachListenerToStream)
{
	UNUSED_AMX;
	REQUIRE(2);
	return server().attachListener(handle(params[1]), playerId(params[2]));
}

NATIVE(n_SvHasListenerInStream)
{
	UNUSED_AMX;
	REQUIRE(2);
	return server().hasListener(handle(params[1]), playerId(params[2]));
}

NATIVE(n_SvDetachListenerFromStream)
{
	UNUSED_AMX;
	REQUIRE(2);
	return server().detachListener(handle(params[1]), playerId(params[2]));
}

NATIVE(n_SvDetachAllListenersFromStream)
{
	UNUSED_AMX;
	REQUIRE(1);
	server().detachAllListeners(handle(params[1]));
	return 1;
}

NATIVE(n_SvAttachSpeakerToStream)
{
	UNUSED_AMX;
	REQUIRE(2);
	return server().attachSpeaker(handle(params[1]), playerId(params[2]));
}

NATIVE(n_SvHasSpeakerInStream)
{
	UNUSED_AMX;
	REQUIRE(2);
	return server().hasSpeaker(handle(params[1]), playerId(params[2]));
}

NATIVE(n_SvDetachSpeakerFromStream)
{
	UNUSED_AMX;
	REQUIRE(2);
	return server().detachSpeaker(handle(params[1]), playerId(params[2]));
}

NATIVE(n_SvDetachAllSpeakersFromStream)
{
	UNUSED_AMX;
	REQUIRE(1);
	server().detachAllSpeakers(handle(params[1]));
	return 1;
}

NATIVE(n_SvStreamParameterSet)
{
	UNUSED_AMX;
	REQUIRE(3);
	server().setParameter(handle(params[1]), static_cast<uint8_t>(params[2]), toFloat(params[3]));
	return 1;
}

NATIVE(n_SvStreamParameterReset)
{
	UNUSED_AMX;
	REQUIRE(2);
	server().resetParameter(handle(params[1]), static_cast<uint8_t>(params[2]));
	return 1;
}

NATIVE(n_SvStreamParameterHas)
{
	UNUSED_AMX;
	REQUIRE(2);
	return server().hasParameter(handle(params[1]), static_cast<uint8_t>(params[2]));
}

NATIVE(n_SvStreamParameterGet)
{
	UNUSED_AMX;
	REQUIRE(2);
	return fromFloat(server().parameter(handle(params[1]), static_cast<uint8_t>(params[2])));
}

NATIVE(n_SvStreamParameterSlideFromTo)
{
	UNUSED_AMX;
	REQUIRE(5);
	server().slideParameter(handle(params[1]), static_cast<uint8_t>(params[2]), toFloat(params[3]), toFloat(params[4]), static_cast<uint32_t>(params[5]));
	return 1;
}

NATIVE(n_SvStreamParameterSlideTo)
{
	UNUSED_AMX;
	REQUIRE(4);
	server().slideParameterTo(handle(params[1]), static_cast<uint8_t>(params[2]), toFloat(params[3]), static_cast<uint32_t>(params[4]));
	return 1;
}

NATIVE(n_SvStreamParameterSlide)
{
	UNUSED_AMX;
	REQUIRE(4);
	server().slideParameterBy(handle(params[1]), static_cast<uint8_t>(params[2]), toFloat(params[3]), static_cast<uint32_t>(params[4]));
	return 1;
}

NATIVE(n_SvDeleteStream)
{
	UNUSED_AMX;
	REQUIRE(1);
	server().deleteStream(handle(params[1]));
	return 1;
}

NATIVE(n_SvEffectCreateChorus)
{
	REQUIRE(8);
	return makeEffect(amx, vb::effect::chorus, params[1], vb::ChorusParams { toFloat(params[2]), toFloat(params[3]), toFloat(params[4]), toFloat(params[5]),
		static_cast<uint32_t>(params[6]), toFloat(params[7]), static_cast<uint32_t>(params[8]) });
}

NATIVE(n_SvEffectCreateCompressor)
{
	REQUIRE(7);
	return makeEffect(amx, vb::effect::compressor, params[1], vb::CompressorParams { toFloat(params[2]), toFloat(params[3]), toFloat(params[4]),
		toFloat(params[5]), toFloat(params[6]), toFloat(params[7]) });
}

NATIVE(n_SvEffectCreateDistortion)
{
	REQUIRE(6);
	return makeEffect(amx, vb::effect::distortion, params[1], vb::DistortionParams { toFloat(params[2]), toFloat(params[3]), toFloat(params[4]),
		toFloat(params[5]), toFloat(params[6]) });
}

NATIVE(n_SvEffectCreateEcho)
{
	REQUIRE(6);
	return makeEffect(amx, vb::effect::echo, params[1], vb::EchoParams { toFloat(params[2]), toFloat(params[3]), toFloat(params[4]), toFloat(params[5]),
		static_cast<uint32_t>(params[6] != 0) });
}

NATIVE(n_SvEffectCreateFlanger)
{
	REQUIRE(8);
	return makeEffect(amx, vb::effect::flanger, params[1], vb::FlangerParams { toFloat(params[2]), toFloat(params[3]), toFloat(params[4]), toFloat(params[5]),
		static_cast<uint32_t>(params[6]), toFloat(params[7]), static_cast<uint32_t>(params[8]) });
}

NATIVE(n_SvEffectCreateGargle)
{
	REQUIRE(3);
	return makeEffect(amx, vb::effect::gargle, params[1], vb::GargleParams { static_cast<uint32_t>(params[2]), static_cast<uint32_t>(params[3]) });
}

NATIVE(n_SvEffectCreateI3dl2reverb)
{
	REQUIRE(13);
	return makeEffect(amx, vb::effect::i3dl2reverb, params[1], vb::I3dl2reverbParams { static_cast<int32_t>(params[2]), static_cast<int32_t>(params[3]),
		toFloat(params[4]), toFloat(params[5]), toFloat(params[6]), static_cast<int32_t>(params[7]), toFloat(params[8]), static_cast<int32_t>(params[9]),
		toFloat(params[10]), toFloat(params[11]), toFloat(params[12]), toFloat(params[13]) });
}

NATIVE(n_SvEffectCreateParameq)
{
	REQUIRE(4);
	return makeEffect(amx, vb::effect::parameq, params[1], vb::ParameqParams { toFloat(params[2]), toFloat(params[3]), toFloat(params[4]) });
}

NATIVE(n_SvEffectCreateReverb)
{
	REQUIRE(5);
	return makeEffect(amx, vb::effect::reverb, params[1], vb::ReverbParams { toFloat(params[2]), toFloat(params[3]), toFloat(params[4]), toFloat(params[5]) });
}

NATIVE(n_SvEffectAttachStream)
{
	UNUSED_AMX;
	REQUIRE(2);
	server().attachEffect(handle(params[1]), handle(params[2]));
	return 1;
}

NATIVE(n_SvEffectDetachStream)
{
	UNUSED_AMX;
	REQUIRE(2);
	server().detachEffect(handle(params[1]), handle(params[2]));
	return 1;
}

NATIVE(n_SvEffectDelete)
{
	UNUSED_AMX;
	REQUIRE(1);
	server().deleteEffect(handle(params[1]));
	return 1;
}

// ---------------------------------------------------------------------------
// Voice Bridge natives
// ---------------------------------------------------------------------------

NATIVE(n_VB_GetPluginVersion)
{
	REQUIRE(2);
	cell* physical = nullptr;
	if (params[2] <= 0 || amx_GetAddr(amx, params[1], &physical) != AMX_ERR_NONE || !physical)
	{
		return 0;
	}
	amx_SetString(physical, VOICE_BRIDGE_VERSION, 0, 0, static_cast<size_t>(params[2]));
	return 1;
}

NATIVE(n_VB_GetVoicePort)
{
	UNUSED_AMX;
	(void)params;
	return server().port();
}

NATIVE(n_VB_SetDebug)
{
	UNUSED_AMX;
	REQUIRE(1);
	LogSetDebug(params[1] != 0);
	return 1;
}

NATIVE(n_VB_SetBitrate)
{
	UNUSED_AMX;
	REQUIRE(1);
	server().setBitrate(static_cast<uint32_t>(params[1]));
	return 1;
}

NATIVE(n_VB_GetBitrate)
{
	UNUSED_AMX;
	(void)params;
	return static_cast<cell>(server().bitrate());
}

NATIVE(n_VB_HasPlugin)
{
	UNUSED_AMX;
	REQUIRE(1);
	return server().hasPlugin(playerId(params[1]));
}

NATIVE(n_VB_GetClientVersion)
{
	UNUSED_AMX;
	REQUIRE(1);
	return server().clientVersion(playerId(params[1]));
}

NATIVE(n_VB_GetClientBuild)
{
	UNUSED_AMX;
	REQUIRE(1);
	return server().clientBuild(playerId(params[1]));
}

// Voice Bridge clients report MAJOR*10000 + MINOR*100 + PATCH; SampVoice
// clients only report their protocol (11 = SampVoice 3.1).
ClientType clientVersionParts(uint16_t player, int& major, int& minor, int& patch)
{
	major = minor = patch = 0;
	const ClientType type = server().clientType(player);
	if (type == ClientType::VoiceBridge)
	{
		const int build = server().clientBuild(player);
		major = build / 10000;
		minor = build / 100 % 100;
		patch = build % 100;
	}
	else if (type == ClientType::SampVoice)
	{
		const int protocol = server().clientVersion(player);
		major = protocol >= 10 ? 3 : 0;
		minor = protocol >= 10 ? protocol - 10 : protocol;
	}
	return type;
}

NATIVE(n_VB_GetClientVersionNumbers)
{
	REQUIRE(4);
	int major, minor, patch;
	const ClientType type = clientVersionParts(playerId(params[1]), major, minor, patch);
	setReference(amx, params[2], major);
	setReference(amx, params[3], minor);
	setReference(amx, params[4], patch);
	return static_cast<cell>(type);
}

NATIVE(n_VB_GetClientVersionString)
{
	REQUIRE(3);
	cell* physical = nullptr;
	if (params[3] <= 0 || amx_GetAddr(amx, params[2], &physical) != AMX_ERR_NONE || !physical)
	{
		return 0;
	}
	int major, minor, patch;
	const ClientType type = clientVersionParts(playerId(params[1]), major, minor, patch);
	std::string text;
	if (type == ClientType::VoiceBridge)
	{
		text = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
	}
	else if (type == ClientType::SampVoice)
	{
		text = std::to_string(major) + "." + std::to_string(minor);
	}
	amx_SetString(physical, text.c_str(), 0, 0, static_cast<size_t>(params[3]));
	return static_cast<cell>(type);
}

NATIVE(n_VB_GetClientType)
{
	UNUSED_AMX;
	REQUIRE(1);
	return static_cast<cell>(server().clientType(playerId(params[1])));
}

NATIVE(n_VB_AllowClientType)
{
	UNUSED_AMX;
	REQUIRE(2);
	if (params[1] != static_cast<cell>(ClientType::SampVoice) && params[1] != static_cast<cell>(ClientType::VoiceBridge))
	{
		return 0;
	}
	server().setClientTypeAllowed(static_cast<ClientType>(params[1]), params[2] != 0);
	return 1;
}

NATIVE(n_VB_IsClientTypeAllowed)
{
	UNUSED_AMX;
	REQUIRE(1);
	return server().isClientTypeAllowed(static_cast<ClientType>(params[1]));
}

NATIVE(n_VB_SetPlayerSpeakerList)
{
	UNUSED_AMX;
	REQUIRE(2);
	return server().setPlayerSpeakerList(playerId(params[1]), params[2] != 0);
}

NATIVE(n_VB_SetPlayerMicIcon)
{
	UNUSED_AMX;
	REQUIRE(2);
	return server().setPlayerMicIcon(playerId(params[1]), params[2] != 0);
}

NATIVE(n_VB_SetPlayerVoiceActivation)
{
	UNUSED_AMX;
	REQUIRE(2);
	return server().setPlayerVoiceActivation(playerId(params[1]), params[2] != 0);
}

NATIVE(n_VB_IsExtendedClient)
{
	UNUSED_AMX;
	REQUIRE(1);
	return server().isExtended(playerId(params[1]));
}

NATIVE(n_VB_HasMicrophone)
{
	UNUSED_AMX;
	REQUIRE(1);
	return server().hasMicro(playerId(params[1]));
}

NATIVE(n_VB_GetTransport)
{
	UNUSED_AMX;
	REQUIRE(1);
	return server().transportOf(playerId(params[1]));
}

NATIVE(n_VB_IsTalking)
{
	UNUSED_AMX;
	REQUIRE(1);
	return server().isTalking(playerId(params[1]));
}

NATIVE(n_VB_IsClientMicMuted)
{
	UNUSED_AMX;
	REQUIRE(1);
	return server().clientMicMuted(playerId(params[1]));
}

NATIVE(n_VB_IsClientSoundMuted)
{
	UNUSED_AMX;
	REQUIRE(1);
	return server().clientSoundMuted(playerId(params[1]));
}

NATIVE(n_VB_MutePlayer)
{
	UNUSED_AMX;
	REQUIRE(2);
	const uint16_t player = playerId(params[1]);
	if (!server().hasPlugin(player))
	{
		return 0;
	}
	server().setMuted(player, params[2] != 0);
	return 1;
}

NATIVE(n_VB_IsPlayerMuted)
{
	UNUSED_AMX;
	REQUIRE(1);
	return server().isMuted(playerId(params[1]));
}

NATIVE(n_VB_IsRecording)
{
	UNUSED_AMX;
	REQUIRE(1);
	return server().isRecording(playerId(params[1]));
}

NATIVE(n_VB_BlockSpeaker)
{
	UNUSED_AMX;
	REQUIRE(3);
	return server().setBlocked(playerId(params[1]), playerId(params[2]), params[3] != 0);
}

NATIVE(n_VB_IsSpeakerBlocked)
{
	UNUSED_AMX;
	REQUIRE(2);
	return server().isBlocked(playerId(params[1]), playerId(params[2]));
}

NATIVE(n_VB_SetSpeakerVolume)
{
	UNUSED_AMX;
	REQUIRE(3);
	return server().setPlayerVolume(playerId(params[1]), playerId(params[2]), toFloat(params[3]));
}

NATIVE(n_VB_Notify)
{
	REQUIRE(4);
	const cell duration = params[4] < 0 ? 0 : (params[4] > 60000 ? 60000 : params[4]);
	return server().notify(playerId(params[1]), getString(amx, params[2]), static_cast<uint32_t>(params[3]), static_cast<uint16_t>(duration));
}

NATIVE(n_VB_CreateGlobalStream)
{
	return n_SvCreateGStream(amx, params);
}

NATIVE(n_VB_CreateStaticStreamAtPoint)
{
	return n_SvCreateSLStreamAtPoint(amx, params);
}

NATIVE(n_VB_CreateStaticStreamAtPlayer)
{
	return n_SvCreateSLStreamAtPlayer(amx, params);
}

NATIVE(n_VB_CreateStaticStreamAtVehicle)
{
	return n_SvCreateSLStreamAtVehicle(amx, params);
}

NATIVE(n_VB_CreateStaticStreamAtObject)
{
	return n_SvCreateSLStreamAtObject(amx, params);
}

NATIVE(n_VB_CreateDynamicStreamAtPoint)
{
	return n_SvCreateDLStreamAtPoint(amx, params);
}

NATIVE(n_VB_CreateDynamicStreamAtPlayer)
{
	return n_SvCreateDLStreamAtPlayer(amx, params);
}

NATIVE(n_VB_CreateDynamicStreamAtVehicle)
{
	return n_SvCreateDLStreamAtVehicle(amx, params);
}

NATIVE(n_VB_CreateDynamicStreamAtObject)
{
	return n_SvCreateDLStreamAtObject(amx, params);
}

NATIVE(n_VB_DeleteStream)
{
	UNUSED_AMX;
	REQUIRE(1);
	return server().deleteStream(handle(params[1]));
}

NATIVE(n_VB_IsValidStream)
{
	UNUSED_AMX;
	REQUIRE(1);
	return server().isValidStream(handle(params[1]));
}

NATIVE(n_VB_GetStreamType)
{
	UNUSED_AMX;
	REQUIRE(1);
	return server().streamType(handle(params[1]));
}

NATIVE(n_VB_SetStreamDistance)
{
	UNUSED_AMX;
	REQUIRE(2);
	return server().setStreamDistance(handle(params[1]), toFloat(params[2]));
}

NATIVE(n_VB_GetStreamDistance)
{
	UNUSED_AMX;
	REQUIRE(1);
	return fromFloat(server().streamDistance(handle(params[1])));
}

NATIVE(n_VB_SetStreamPosition)
{
	UNUSED_AMX;
	REQUIRE(4);
	return server().setStreamPosition(handle(params[1]), vb::Vec3 { toFloat(params[2]), toFloat(params[3]), toFloat(params[4]) });
}

NATIVE(n_VB_GetStreamPosition)
{
	REQUIRE(4);
	vb::Vec3 position {};
	if (!server().streamPosition(handle(params[1]), position))
	{
		return 0;
	}
	setReference(amx, params[2], fromFloat(position.x));
	setReference(amx, params[3], fromFloat(position.y));
	setReference(amx, params[4], fromFloat(position.z));
	return 1;
}

NATIVE(n_VB_SetStreamMaxListeners)
{
	UNUSED_AMX;
	REQUIRE(2);
	return server().setStreamMaxListeners(handle(params[1]), static_cast<uint32_t>(params[2]));
}

NATIVE(n_VB_SetStreamWorld)
{
	UNUSED_AMX;
	REQUIRE(3);
	return server().setStreamWorld(handle(params[1]), static_cast<int>(params[2]), static_cast<int>(params[3]));
}

NATIVE(n_VB_SetStreamFlat)
{
	UNUSED_AMX;
	REQUIRE(2);
	return server().setStreamFlat(handle(params[1]), params[2] != 0);
}

NATIVE(n_VB_AddListener)
{
	return n_SvAttachListenerToStream(amx, params);
}

NATIVE(n_VB_RemoveListener)
{
	return n_SvDetachListenerFromStream(amx, params);
}

NATIVE(n_VB_HasListener)
{
	return n_SvHasListenerInStream(amx, params);
}

NATIVE(n_VB_RemoveAllListeners)
{
	return n_SvDetachAllListenersFromStream(amx, params);
}

NATIVE(n_VB_GetListenerCount)
{
	UNUSED_AMX;
	REQUIRE(1);
	return server().listenerCount(handle(params[1]));
}

NATIVE(n_VB_AddSpeaker)
{
	return n_SvAttachSpeakerToStream(amx, params);
}

NATIVE(n_VB_RemoveSpeaker)
{
	return n_SvDetachSpeakerFromStream(amx, params);
}

NATIVE(n_VB_HasSpeaker)
{
	return n_SvHasSpeakerInStream(amx, params);
}

NATIVE(n_VB_RemoveAllSpeakers)
{
	return n_SvDetachAllSpeakersFromStream(amx, params);
}

NATIVE(n_VB_GetSpeakerCount)
{
	UNUSED_AMX;
	REQUIRE(1);
	return server().speakerCount(handle(params[1]));
}

cell copyPlayers(AMX* amx, cell address, cell size, const std::vector<uint16_t>& players)
{
	cell* physical = nullptr;
	if (size <= 0 || amx_GetAddr(amx, address, &physical) != AMX_ERR_NONE || !physical)
	{
		return 0;
	}
	const cell count = std::min<cell>(size, static_cast<cell>(players.size()));
	for (cell i = 0; i < count; ++i)
	{
		physical[i] = players[static_cast<std::size_t>(i)];
	}
	return count;
}

NATIVE(n_VB_GetListeners)
{
	REQUIRE(3);
	std::vector<uint16_t> players;
	server().listenersOf(handle(params[1]), players);
	return copyPlayers(amx, params[2], params[3], players);
}

NATIVE(n_VB_GetSpeakers)
{
	REQUIRE(3);
	std::vector<uint16_t> players;
	server().speakersOf(handle(params[1]), players);
	return copyPlayers(amx, params[2], params[3], players);
}

NATIVE(n_VB_SetStreamParameter)
{
	UNUSED_AMX;
	REQUIRE(3);
	return server().setParameter(handle(params[1]), static_cast<uint8_t>(params[2]), toFloat(params[3]));
}

NATIVE(n_VB_GetStreamParameter)
{
	return n_SvStreamParameterGet(amx, params);
}

NATIVE(n_VB_HasStreamParameter)
{
	return n_SvStreamParameterHas(amx, params);
}

NATIVE(n_VB_ResetStreamParameter)
{
	UNUSED_AMX;
	REQUIRE(2);
	return server().resetParameter(handle(params[1]), static_cast<uint8_t>(params[2]));
}

NATIVE(n_VB_SlideStreamParameter)
{
	UNUSED_AMX;
	REQUIRE(5);
	return server().slideParameter(handle(params[1]), static_cast<uint8_t>(params[2]), toFloat(params[3]), toFloat(params[4]), static_cast<uint32_t>(params[5]));
}

NATIVE(n_VB_SlideStreamParameterTo)
{
	UNUSED_AMX;
	REQUIRE(4);
	return server().slideParameterTo(handle(params[1]), static_cast<uint8_t>(params[2]), toFloat(params[3]), static_cast<uint32_t>(params[4]));
}

NATIVE(n_VB_SetStreamVolume)
{
	UNUSED_AMX;
	REQUIRE(2);
	return server().setParameter(handle(params[1]), vb::param::volume, toFloat(params[2]));
}

NATIVE(n_VB_CreatePresetEffect)
{
	REQUIRE(2);
	const auto items = presetItems(params[1], static_cast<int32_t>(params[2]));
	return items.empty() ? 0 : static_cast<cell>(server().createEffectChain(items, amx));
}

NATIVE(n_VB_AttachEffect)
{
	UNUSED_AMX;
	REQUIRE(2);
	return server().attachEffect(handle(params[1]), handle(params[2]));
}

NATIVE(n_VB_DetachEffect)
{
	UNUSED_AMX;
	REQUIRE(2);
	return server().detachEffect(handle(params[1]), handle(params[2]));
}

NATIVE(n_VB_DeleteEffect)
{
	UNUSED_AMX;
	REQUIRE(1);
	return server().deleteEffect(handle(params[1]));
}

NATIVE(n_VB_IsValidEffect)
{
	UNUSED_AMX;
	REQUIRE(1);
	return server().isValidEffect(handle(params[1]));
}

const AMX_NATIVE_INFO kNatives[] = {
	// SampVoice 3.1
	{ "SvDebug", n_SvDebug },
	{ "SvInit", n_SvInit },
	{ "SvGetVersion", n_SvGetVersion },
	{ "SvHasMicro", n_SvHasMicro },
	{ "SvStartRecord", n_SvStartRecord },
	{ "SvStopRecord", n_SvStopRecord },
	{ "SvAddKey", n_SvAddKey },
	{ "SvHasKey", n_SvHasKey },
	{ "SvRemoveKey", n_SvRemoveKey },
	{ "SvRemoveAllKeys", n_SvRemoveAllKeys },
	{ "SvMutePlayerStatus", n_SvMutePlayerStatus },
	{ "SvMutePlayerEnable", n_SvMutePlayerEnable },
	{ "SvMutePlayerDisable", n_SvMutePlayerDisable },
	{ "SvCreateGStream", n_SvCreateGStream },
	{ "SvCreateSLStreamAtPoint", n_SvCreateSLStreamAtPoint },
	{ "SvCreateSLStreamAtVehicle", n_SvCreateSLStreamAtVehicle },
	{ "SvCreateSLStreamAtPlayer", n_SvCreateSLStreamAtPlayer },
	{ "SvCreateSLStreamAtObject", n_SvCreateSLStreamAtObject },
	{ "SvCreateDLStreamAtPoint", n_SvCreateDLStreamAtPoint },
	{ "SvCreateDLStreamAtVehicle", n_SvCreateDLStreamAtVehicle },
	{ "SvCreateDLStreamAtPlayer", n_SvCreateDLStreamAtPlayer },
	{ "SvCreateDLStreamAtObject", n_SvCreateDLStreamAtObject },
	{ "SvUpdateDistanceForLStream", n_SvUpdateDistanceForLStream },
	{ "SvUpdatePositionForLPStream", n_SvUpdatePositionForLPStream },
	{ "SvAttachListenerToStream", n_SvAttachListenerToStream },
	{ "SvHasListenerInStream", n_SvHasListenerInStream },
	{ "SvDetachListenerFromStream", n_SvDetachListenerFromStream },
	{ "SvDetachAllListenersFromStream", n_SvDetachAllListenersFromStream },
	{ "SvAttachSpeakerToStream", n_SvAttachSpeakerToStream },
	{ "SvHasSpeakerInStream", n_SvHasSpeakerInStream },
	{ "SvDetachSpeakerFromStream", n_SvDetachSpeakerFromStream },
	{ "SvDetachAllSpeakersFromStream", n_SvDetachAllSpeakersFromStream },
	{ "SvStreamParameterSet", n_SvStreamParameterSet },
	{ "SvStreamParameterReset", n_SvStreamParameterReset },
	{ "SvStreamParameterHas", n_SvStreamParameterHas },
	{ "SvStreamParameterGet", n_SvStreamParameterGet },
	{ "SvStreamParameterSlideFromTo", n_SvStreamParameterSlideFromTo },
	{ "SvStreamParameterSlideTo", n_SvStreamParameterSlideTo },
	{ "SvStreamParameterSlide", n_SvStreamParameterSlide },
	{ "SvDeleteStream", n_SvDeleteStream },
	{ "SvEffectCreateChorus", n_SvEffectCreateChorus },
	{ "SvEffectCreateCompressor", n_SvEffectCreateCompressor },
	{ "SvEffectCreateDistortion", n_SvEffectCreateDistortion },
	{ "SvEffectCreateEcho", n_SvEffectCreateEcho },
	{ "SvEffectCreateFlanger", n_SvEffectCreateFlanger },
	{ "SvEffectCreateGargle", n_SvEffectCreateGargle },
	{ "SvEffectCreateI3dl2reverb", n_SvEffectCreateI3dl2reverb },
	{ "SvEffectCreateParameq", n_SvEffectCreateParameq },
	{ "SvEffectCreateReverb", n_SvEffectCreateReverb },
	{ "SvEffectAttachStream", n_SvEffectAttachStream },
	{ "SvEffectDetachStream", n_SvEffectDetachStream },
	{ "SvEffectDelete", n_SvEffectDelete },

	// Voice Bridge
	{ "VB_GetPluginVersion", n_VB_GetPluginVersion },
	{ "VB_GetVoicePort", n_VB_GetVoicePort },
	{ "VB_SetDebug", n_VB_SetDebug },
	{ "VB_SetBitrate", n_VB_SetBitrate },
	{ "VB_GetBitrate", n_VB_GetBitrate },
	{ "VB_HasPlugin", n_VB_HasPlugin },
	{ "VB_GetClientVersion", n_VB_GetClientVersion },
	{ "VB_GetClientBuild", n_VB_GetClientBuild },
	{ "VB_IsExtendedClient", n_VB_IsExtendedClient },
	{ "VB_GetClientType", n_VB_GetClientType },
	{ "VB_GetClientVersionNumbers", n_VB_GetClientVersionNumbers },
	{ "VB_GetClientVersionString", n_VB_GetClientVersionString },
	{ "VB_AllowClientType", n_VB_AllowClientType },
	{ "VB_IsClientTypeAllowed", n_VB_IsClientTypeAllowed },
	{ "VB_SetPlayerSpeakerList", n_VB_SetPlayerSpeakerList },
	{ "VB_SetPlayerMicIcon", n_VB_SetPlayerMicIcon },
	{ "VB_SetPlayerVoiceActivation", n_VB_SetPlayerVoiceActivation },
	{ "VB_HasMicrophone", n_VB_HasMicrophone },
	{ "VB_GetTransport", n_VB_GetTransport },
	{ "VB_IsTalking", n_VB_IsTalking },
	{ "VB_IsClientMicMuted", n_VB_IsClientMicMuted },
	{ "VB_IsClientSoundMuted", n_VB_IsClientSoundMuted },
	{ "VB_MutePlayer", n_VB_MutePlayer },
	{ "VB_IsPlayerMuted", n_VB_IsPlayerMuted },
	{ "VB_StartRecord", n_SvStartRecord },
	{ "VB_StopRecord", n_SvStopRecord },
	{ "VB_IsRecording", n_VB_IsRecording },
	{ "VB_AddKey", n_SvAddKey },
	{ "VB_RemoveKey", n_SvRemoveKey },
	{ "VB_HasKey", n_SvHasKey },
	{ "VB_RemoveAllKeys", n_SvRemoveAllKeys },
	{ "VB_BlockSpeaker", n_VB_BlockSpeaker },
	{ "VB_IsSpeakerBlocked", n_VB_IsSpeakerBlocked },
	{ "VB_SetSpeakerVolume", n_VB_SetSpeakerVolume },
	{ "VB_Notify", n_VB_Notify },

	{ "VB_CreateGlobalStream", n_VB_CreateGlobalStream },
	{ "VB_CreateStaticStreamAtPoint", n_VB_CreateStaticStreamAtPoint },
	{ "VB_CreateStaticStreamAtPlayer", n_VB_CreateStaticStreamAtPlayer },
	{ "VB_CreateStaticStreamAtVehicle", n_VB_CreateStaticStreamAtVehicle },
	{ "VB_CreateStaticStreamAtObject", n_VB_CreateStaticStreamAtObject },
	{ "VB_CreateDynamicStreamAtPoint", n_VB_CreateDynamicStreamAtPoint },
	{ "VB_CreateDynamicStreamAtPlayer", n_VB_CreateDynamicStreamAtPlayer },
	{ "VB_CreateDynamicStreamAtVehicle", n_VB_CreateDynamicStreamAtVehicle },
	{ "VB_CreateDynamicStreamAtObject", n_VB_CreateDynamicStreamAtObject },
	{ "VB_DeleteStream", n_VB_DeleteStream },
	{ "VB_IsValidStream", n_VB_IsValidStream },
	{ "VB_GetStreamType", n_VB_GetStreamType },
	{ "VB_SetStreamDistance", n_VB_SetStreamDistance },
	{ "VB_GetStreamDistance", n_VB_GetStreamDistance },
	{ "VB_SetStreamPosition", n_VB_SetStreamPosition },
	{ "VB_GetStreamPosition", n_VB_GetStreamPosition },
	{ "VB_SetStreamMaxListeners", n_VB_SetStreamMaxListeners },
	{ "VB_SetStreamWorld", n_VB_SetStreamWorld },
	{ "VB_SetStreamFlat", n_VB_SetStreamFlat },
	{ "VB_AddListener", n_VB_AddListener },
	{ "VB_RemoveListener", n_VB_RemoveListener },
	{ "VB_HasListener", n_VB_HasListener },
	{ "VB_RemoveAllListeners", n_VB_RemoveAllListeners },
	{ "VB_GetListenerCount", n_VB_GetListenerCount },
	{ "VB_GetListeners", n_VB_GetListeners },
	{ "VB_AddSpeaker", n_VB_AddSpeaker },
	{ "VB_RemoveSpeaker", n_VB_RemoveSpeaker },
	{ "VB_HasSpeaker", n_VB_HasSpeaker },
	{ "VB_RemoveAllSpeakers", n_VB_RemoveAllSpeakers },
	{ "VB_GetSpeakerCount", n_VB_GetSpeakerCount },
	{ "VB_GetSpeakers", n_VB_GetSpeakers },
	{ "VB_SetStreamParameter", n_VB_SetStreamParameter },
	{ "VB_GetStreamParameter", n_VB_GetStreamParameter },
	{ "VB_HasStreamParameter", n_VB_HasStreamParameter },
	{ "VB_ResetStreamParameter", n_VB_ResetStreamParameter },
	{ "VB_SlideStreamParameter", n_VB_SlideStreamParameter },
	{ "VB_SlideStreamParameterTo", n_VB_SlideStreamParameterTo },
	{ "VB_SetStreamVolume", n_VB_SetStreamVolume },

	{ "VB_CreateChorusEffect", n_SvEffectCreateChorus },
	{ "VB_CreateCompressorEffect", n_SvEffectCreateCompressor },
	{ "VB_CreateDistortionEffect", n_SvEffectCreateDistortion },
	{ "VB_CreateEchoEffect", n_SvEffectCreateEcho },
	{ "VB_CreateFlangerEffect", n_SvEffectCreateFlanger },
	{ "VB_CreateGargleEffect", n_SvEffectCreateGargle },
	{ "VB_CreateI3dl2ReverbEffect", n_SvEffectCreateI3dl2reverb },
	{ "VB_CreateParamEqEffect", n_SvEffectCreateParameq },
	{ "VB_CreateReverbEffect", n_SvEffectCreateReverb },
	{ "VB_CreatePresetEffect", n_VB_CreatePresetEffect },
	{ "VB_AttachEffect", n_VB_AttachEffect },
	{ "VB_DetachEffect", n_VB_DetachEffect },
	{ "VB_DeleteEffect", n_VB_DeleteEffect },
	{ "VB_IsValidEffect", n_VB_IsValidEffect },
};
}

int RegisterNatives(AMX* amx)
{
	return amx_Register(amx, kNatives, static_cast<int>(sizeof(kNatives) / sizeof(kNatives[0])));
}
}
