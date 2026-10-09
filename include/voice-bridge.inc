/*
 *  Voice Bridge - voice chat for open.mp and SA-MP
 *  by devbluen - https://github.com/devbluen/omp-Voice-Bridge
 *  Include after <open.mp> or <a_samp>.  Docs: README.md
 */

#if defined _voice_bridge_included
	#endinput
#endif
#define _voice_bridge_included

#define VOICE_BRIDGE_INCLUDE_VERSION "1.0.0"

// ===========================================================================
//  Constants
// ===========================================================================

#define VB_INVALID_STREAM       (0)
#define VB_INVALID_EFFECT       (0)
#define VB_INFINITE             (-1)
#define VB_ANY_WORLD            (-1)

// Voice client of a player
#define VB_CLIENT_NONE          (0)
#define VB_CLIENT_SAMPVOICE     (1)
#define VB_CLIENT_VOICEBRIDGE   (2)

// Voice connection of a player
#define VB_TRANSPORT_NONE       (0)
#define VB_TRANSPORT_UDP        (1)
#define VB_TRANSPORT_TUNNEL     (2)

// VB_OnPlayerVoiceIgnored: why the player's voice was dropped
#define VB_VOICE_IGNORED_CLIENT_NOT_ALLOWED (1) // client type blocked (VB_AllowClientType / voice_allow_*)
#define VB_VOICE_IGNORED_MUTED              (2) // VB_MutePlayer / SvMutePlayerEnable
#define VB_VOICE_IGNORED_NO_KEY             (3) // no talk key (VB_AddKey) and not recording
#define VB_VOICE_IGNORED_NOT_SPEAKER        (4) // talking, but not a speaker of any stream (VB_AddSpeaker)

// Compare with VB_GetClientBuild: VB_GetClientBuild(playerid) < VB_VERSION(1, 2, 0)
#define VB_VERSION(%0,%1,%2)    ((%0) * 10000 + (%1) * 100 + (%2))

// Push-to-talk keys (Windows virtual key codes)
#define VB_KEY_B                (0x42)
#define VB_KEY_N                (0x4E)
#define VB_KEY_X                (0x58)
#define VB_KEY_Z                (0x5A)
#define VB_KEY_CAPS             (0x14)
#define VB_KEY_ALT              (0x12)
#define VB_KEY_LSHIFT           (0xA0)
#define VB_KEY_MOUSE4           (0x05)
#define VB_KEY_MOUSE5           (0x06)

enum VB_STREAM_TYPE
{
	VB_STREAM_GLOBAL = 0,
	VB_STREAM_STATIC_POINT,
	VB_STREAM_STATIC_PLAYER,
	VB_STREAM_STATIC_VEHICLE,
	VB_STREAM_STATIC_OBJECT,
	VB_STREAM_DYNAMIC_POINT,
	VB_STREAM_DYNAMIC_PLAYER,
	VB_STREAM_DYNAMIC_VEHICLE,
	VB_STREAM_DYNAMIC_OBJECT
}

enum VB_PARAM
{
	VB_PARAM_FREQUENCY = 1,
	VB_PARAM_VOLUME    = 2,
	VB_PARAM_PANNING   = 3,
	VB_PARAM_EAXMIX    = 4,
	VB_PARAM_SRC       = 8
}

enum VB_PRESET
{
	VB_PRESET_RADIO = 1,
	VB_PRESET_PHONE,
	VB_PRESET_MEGAPHONE,
	VB_PRESET_HALL,
	VB_PRESET_CAVE,
	VB_PRESET_UNDERWATER,
	VB_PRESET_ROBOT,
	VB_PRESET_ECHO,
	VB_PRESET_WALKIE_TALKIE
}

// ===========================================================================
//  Server
// ===========================================================================

native VB_GetPluginVersion(version[], size = sizeof version);
native VB_GetVoicePort();
native VB_SetDebug(bool:enabled);
native VB_SetBitrate(bitrate);
native VB_GetBitrate();

native bool:VB_AllowClientType(type, bool:allowed);
native bool:VB_IsClientTypeAllowed(type);

// ===========================================================================
//  Players - client information
// ===========================================================================

native bool:VB_HasPlugin(playerid);
native VB_GetClientType(playerid);
native bool:VB_IsExtendedClient(playerid);
native VB_GetClientVersionString(playerid, version[], size = sizeof version);
native VB_GetClientVersionNumbers(playerid, &major, &minor, &patch);
native VB_GetClientBuild(playerid);
native VB_GetClientVersion(playerid);
native VB_GetTransport(playerid);
native bool:VB_HasMicrophone(playerid);
native bool:VB_IsClientMicMuted(playerid);
native bool:VB_IsClientSoundMuted(playerid);
native bool:VB_IsTalking(playerid);

// ===========================================================================
//  Players - talking
// ===========================================================================

native bool:VB_AddKey(playerid, keyid);
native bool:VB_RemoveKey(playerid, keyid);
native bool:VB_HasKey(playerid, keyid);
native VB_RemoveAllKeys(playerid);

native bool:VB_StartRecord(playerid);
native bool:VB_StopRecord(playerid);
native bool:VB_IsRecording(playerid);

native bool:VB_MutePlayer(playerid, bool:mute);
native bool:VB_IsPlayerMuted(playerid);

// ===========================================================================
//  Players - listening and screen
// ===========================================================================

native bool:VB_BlockSpeaker(listenerid, speakerid, bool:block);
native bool:VB_IsSpeakerBlocked(listenerid, speakerid);
native bool:VB_SetSpeakerVolume(listenerid, speakerid, Float:volume);

native bool:VB_SetPlayerSpeakerList(playerid, bool:enabled);
native bool:VB_SetPlayerMicIcon(playerid, bool:enabled);
native bool:VB_SetPlayerVoiceActivation(playerid, bool:enabled);
native bool:VB_Notify(playerid, const text[], color = 0xFFFFFFFF, duration = 4000);

// ===========================================================================
//  Streams
// ===========================================================================

native VB_CreateGlobalStream(color = 0, const name[] = "");
native VB_CreateStaticStreamAtPoint(Float:distance, Float:x, Float:y, Float:z, color = 0, const name[] = "");
native VB_CreateStaticStreamAtPlayer(Float:distance, playerid, color = 0, const name[] = "");
native VB_CreateStaticStreamAtVehicle(Float:distance, vehicleid, color = 0, const name[] = "");
native VB_CreateStaticStreamAtObject(Float:distance, objectid, color = 0, const name[] = "");
native VB_CreateDynamicStreamAtPoint(Float:distance, maxlisteners, Float:x, Float:y, Float:z, color = 0, const name[] = "");
native VB_CreateDynamicStreamAtPlayer(Float:distance, maxlisteners, playerid, color = 0, const name[] = "");
native VB_CreateDynamicStreamAtVehicle(Float:distance, maxlisteners, vehicleid, color = 0, const name[] = "");
native VB_CreateDynamicStreamAtObject(Float:distance, maxlisteners, objectid, color = 0, const name[] = "");
native bool:VB_DeleteStream(stream);
native bool:VB_IsValidStream(stream);
native VB_STREAM_TYPE:VB_GetStreamType(stream);

native bool:VB_SetStreamDistance(stream, Float:distance);
native Float:VB_GetStreamDistance(stream);
native bool:VB_SetStreamPosition(stream, Float:x, Float:y, Float:z);
native bool:VB_GetStreamPosition(stream, &Float:x, &Float:y, &Float:z);
native bool:VB_SetStreamMaxListeners(stream, maxlisteners);
native bool:VB_SetStreamWorld(stream, worldid = VB_ANY_WORLD, interiorid = VB_ANY_WORLD);
native bool:VB_SetStreamFlat(stream, bool:flat);
native bool:VB_SetStreamVolume(stream, Float:volume);

native bool:VB_AddListener(stream, playerid);
native bool:VB_RemoveListener(stream, playerid);
native bool:VB_RemoveAllListeners(stream);
native bool:VB_HasListener(stream, playerid);
native VB_GetListenerCount(stream);
native VB_GetListeners(stream, players[], size = sizeof players);

native bool:VB_AddSpeaker(stream, playerid);
native bool:VB_RemoveSpeaker(stream, playerid);
native bool:VB_RemoveAllSpeakers(stream);
native bool:VB_HasSpeaker(stream, playerid);
native VB_GetSpeakerCount(stream);
native VB_GetSpeakers(stream, players[], size = sizeof players);

native bool:VB_SetStreamParameter(stream, VB_PARAM:parameter, Float:value);
native Float:VB_GetStreamParameter(stream, VB_PARAM:parameter);
native bool:VB_HasStreamParameter(stream, VB_PARAM:parameter);
native bool:VB_ResetStreamParameter(stream, VB_PARAM:parameter);
native bool:VB_SlideStreamParameter(stream, VB_PARAM:parameter, Float:from, Float:to, time);
native bool:VB_SlideStreamParameterTo(stream, VB_PARAM:parameter, Float:to, time);

// ===========================================================================
//  Effects
// ===========================================================================

native VB_CreatePresetEffect(VB_PRESET:preset, priority = 0);
native VB_CreateChorusEffect(priority, Float:wetdrymix, Float:depth, Float:feedback, Float:frequency, waveform, Float:delay, phase);
native VB_CreateCompressorEffect(priority, Float:gain, Float:attack, Float:release, Float:threshold, Float:ratio, Float:predelay);
native VB_CreateDistortionEffect(priority, Float:gain, Float:edge, Float:posteqcenterfrequency, Float:posteqbandwidth, Float:prelowpasscutoff);
native VB_CreateEchoEffect(priority, Float:wetdrymix, Float:feedback, Float:leftdelay, Float:rightdelay, bool:pandelay);
native VB_CreateFlangerEffect(priority, Float:wetdrymix, Float:depth, Float:feedback, Float:frequency, waveform, Float:delay, phase);
native VB_CreateGargleEffect(priority, ratehz, waveshape);
native VB_CreateI3dl2ReverbEffect(priority, room, roomhf, Float:roomrollofffactor, Float:decaytime, Float:decayhfratio, reflections, Float:reflectionsdelay, reverb, Float:reverbdelay, Float:diffusion, Float:density, Float:hfreference);
native VB_CreateParamEqEffect(priority, Float:center, Float:bandwidth, Float:gain);
native VB_CreateReverbEffect(priority, Float:ingain, Float:reverbmix, Float:reverbtime, Float:highfreqrtratio);
native bool:VB_AttachEffect(effect, stream);
native bool:VB_DetachEffect(effect, stream);
native bool:VB_DeleteEffect(effect);
native bool:VB_IsValidEffect(effect);

// ===========================================================================
//  Callbacks
// ===========================================================================

#if !defined _sampvoice_included
	forward OnPlayerActivationKeyPress(playerid, keyid);
	forward OnPlayerActivationKeyRelease(playerid, keyid);
#endif
forward VB_OnPlayerClientDetected(playerid, version, bool:extended, bool:hasmicro);
forward VB_OnPlayerTransportChange(playerid, transport);
forward VB_OnPlayerStartTalking(playerid);
forward VB_OnPlayerStopTalking(playerid);
forward VB_OnPlayerClientStatus(playerid, bool:micavailable, bool:micmuted, bool:soundmuted);
// The player talks but the voice is dropped (at most once every 10 s per player).
forward VB_OnPlayerVoiceIgnored(playerid, reason);

// ===========================================================================
//  Internal - SA-MP: the plugin reads positions through these natives
// ===========================================================================

#if !defined _vb_world_natives
	#define _vb_world_natives
	forward __VB_WorldNatives();
	public __VB_WorldNatives()
	{
		new Float:x, Float:y, Float:z, name[2];
		IsPlayerConnected(0);
		IsPlayerNPC(0);
		GetMaxPlayers();
		GetPlayerPos(0, x, y, z);
		GetPlayerVirtualWorld(0);
		GetPlayerInterior(0);
		GetPlayerName(0, name, sizeof name);
		GetVehiclePos(0, x, y, z);
		GetVehicleVirtualWorld(0);
		GetObjectPos(0, x, y, z);
		IsValidObject(0);
		return 1;
	}
#endif
