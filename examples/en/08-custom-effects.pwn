/*
 *  Voice Bridge example 08 - custom effects (without presets)
 *
 *  /voice <name>   changes how your voice sounds to the players near you:
 *                  normal, echo, hall, radio, robot, monster
 *
 *  How effects work:
 *  - VB_Create...Effect(priority, parameters...) creates an effect and returns its id.
 *  - VB_AttachEffect(effect, stream) applies it to everything heard in that stream.
 *  - Several effects can be attached to one stream: they are chained, and
 *    the one with the HIGHER priority is applied first.
 *  - One effect can be attached to many streams at the same time.
 *  - The parameters and their ranges are listed in the README (Effects).
 */

#include <open.mp>
#include <voice-bridge>

#define MAX_CHAIN (3)

enum
{
	VOICE_NORMAL,
	VOICE_ECHO,
	VOICE_HALL,
	VOICE_RADIO,
	VOICE_ROBOT,
	VOICE_MONSTER,
	VOICE_COUNT
}

new const gVoiceNames[VOICE_COUNT][] = { "normal", "echo", "hall", "radio", "robot", "monster" };

// Effects of each voice (VB_INVALID_EFFECT = empty slot)
new gVoiceEffects[VOICE_COUNT][MAX_CHAIN];

new gLocalStream[MAX_PLAYERS] = { VB_INVALID_STREAM, ... };
new gPlayerVoice[MAX_PLAYERS];

main() {}

CreateVoiceEffects()
{
	// Echo: 30% wet, 40% feedback, 300 ms delay on both sides, no ping-pong
	gVoiceEffects[VOICE_ECHO][0] = VB_CreateEchoEffect(0, 30.0, 40.0, 300.0, 300.0, false);

	// Hall: input gain 0 dB, reverb mix -6 dB, 1800 ms tail, high frequency ratio 0.5
	gVoiceEffects[VOICE_HALL][0] = VB_CreateReverbEffect(0, 0.0, -6.0, 1800.0, 0.5);

	// Old radio = 3 effects chained (priority 3 runs first)
	gVoiceEffects[VOICE_RADIO][0] = VB_CreateParamEqEffect(3, 1500.0, 12.0, 12.0);                     // boost the mids
	gVoiceEffects[VOICE_RADIO][1] = VB_CreateDistortionEffect(2, -20.0, 20.0, 2500.0, 2400.0, 4000.0); // crackle
	gVoiceEffects[VOICE_RADIO][2] = VB_CreateCompressorEffect(1, 8.0, 5.0, 200.0, -20.0, 4.0, 2.0);    // even volume

	// Robot: fast flanger (sine wave) + gargle at 40 Hz
	gVoiceEffects[VOICE_ROBOT][0] = VB_CreateFlangerEffect(2, 60.0, 80.0, 70.0, 6.0, 1, 2.0, 2);
	gVoiceEffects[VOICE_ROBOT][1] = VB_CreateGargleEffect(1, 40, 0);

	// Monster: heavy bass, cut highs, a bit of distortion
	gVoiceEffects[VOICE_MONSTER][0] = VB_CreateParamEqEffect(3, 120.0, 24.0, 15.0);
	gVoiceEffects[VOICE_MONSTER][1] = VB_CreateParamEqEffect(2, 5000.0, 36.0, -15.0);
	gVoiceEffects[VOICE_MONSTER][2] = VB_CreateDistortionEffect(1, -25.0, 35.0, 600.0, 800.0, 3000.0);
}

public OnGameModeInit()
{
	for (new v = 0; v < VOICE_COUNT; v++)
	{
		for (new i = 0; i < MAX_CHAIN; i++)
		{
			gVoiceEffects[v][i] = VB_INVALID_EFFECT;
		}
	}
	CreateVoiceEffects();
	return 1;
}

SetPlayerVoice(playerid, voice)
{
	new stream = gLocalStream[playerid];
	// Remove the effects of the previous voice...
	for (new i = 0; i < MAX_CHAIN; i++)
	{
		if (gVoiceEffects[gPlayerVoice[playerid]][i] != VB_INVALID_EFFECT)
		{
			VB_DetachEffect(gVoiceEffects[gPlayerVoice[playerid]][i], stream);
		}
	}
	// ...and attach the new ones. Only playerid talks in this stream, so
	// only their voice changes.
	for (new i = 0; i < MAX_CHAIN; i++)
	{
		if (gVoiceEffects[voice][i] != VB_INVALID_EFFECT)
		{
			VB_AttachEffect(gVoiceEffects[voice][i], stream);
		}
	}
	gPlayerVoice[playerid] = voice;
}

public VB_OnPlayerClientDetected(playerid, version, bool:extended, bool:hasmicro)
{
	if (gLocalStream[playerid] == VB_INVALID_STREAM)
	{
		gLocalStream[playerid] = VB_CreateDynamicStreamAtPlayer(35.0, VB_INFINITE, playerid, 0xFF66FF66, "Local");
		gPlayerVoice[playerid] = VOICE_NORMAL;
		VB_AddKey(playerid, VB_KEY_B);
	}
	return 1;
}

public OnPlayerDisconnect(playerid, reason)
{
	if (gLocalStream[playerid] != VB_INVALID_STREAM)
	{
		VB_DeleteStream(gLocalStream[playerid]); // the effects stay for the other players
		gLocalStream[playerid] = VB_INVALID_STREAM;
	}
	return 1;
}

public OnPlayerActivationKeyPress(playerid, keyid)
{
	if (keyid == VB_KEY_B) VB_AddSpeaker(gLocalStream[playerid], playerid);
	return 1;
}

public OnPlayerActivationKeyRelease(playerid, keyid)
{
	if (keyid == VB_KEY_B) VB_RemoveSpeaker(gLocalStream[playerid], playerid);
	return 1;
}

public OnPlayerCommandText(playerid, cmdtext[])
{
	if (!strcmp(cmdtext, "/voice", true, 6))
	{
		if (gLocalStream[playerid] == VB_INVALID_STREAM)
		{
			return SendClientMessage(playerid, 0xFF5555FF, "You need the voice client (voice-bridge.asi)."), 1;
		}
		for (new v = 0; v < VOICE_COUNT; v++)
		{
			if (cmdtext[6] == ' ' && !strcmp(cmdtext[7], gVoiceNames[v], true))
			{
				SetPlayerVoice(playerid, v);
				new message[64];
				format(message, sizeof message, "Voice: %s", gVoiceNames[v]);
				VB_Notify(playerid, message, 0x66CCFFFF, 2000);
				return 1;
			}
		}
		return SendClientMessage(playerid, 0xFFFFFFFF, "Usage: /voice normal | echo | hall | radio | robot | monster"), 1;
	}
	return 0;
}
