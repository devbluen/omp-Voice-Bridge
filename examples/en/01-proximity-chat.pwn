/*
 *  Voice Bridge example 01 - proximity chat + global chat
 *
 *  B   talk to players near you (heard in 3D)
 *  Z   talk to everyone
 */

#include <open.mp>      // or <a_samp>
#include <voice-bridge>

#define LOCAL_DISTANCE  (35.0)

new gGlobalStream = VB_INVALID_STREAM;
new gLocalStream[MAX_PLAYERS] = { VB_INVALID_STREAM, ... };

main() {}

public OnGameModeInit()
{
	gGlobalStream = VB_CreateGlobalStream(0xFFFFAA00, "Global");
	return 1;
}

SetupVoice(playerid)
{
	if (gLocalStream[playerid] != VB_INVALID_STREAM || !VB_HasPlugin(playerid))
	{
		return;
	}
	// Dynamic stream: players near playerid are added automatically.
	gLocalStream[playerid] = VB_CreateDynamicStreamAtPlayer(LOCAL_DISTANCE, VB_INFINITE, playerid, 0xFF66FF66, "Local");
	VB_AddListener(gGlobalStream, playerid);
	VB_AddKey(playerid, VB_KEY_B);
	VB_AddKey(playerid, VB_KEY_Z);
}

public VB_OnPlayerClientDetected(playerid, version, bool:extended, bool:hasmicro)
{
	SetupVoice(playerid);
	return 1;
}

public OnPlayerConnect(playerid)
{
	SetupVoice(playerid); // also covers a GMX, when players stay connected

	if (!VB_HasPlugin(playerid))
	{
		SendClientMessage(playerid, 0xFF5555FF, "Voice chat: install voice-bridge.asi to talk with other players.");
	}
	else if (!VB_HasMicrophone(playerid))
	{
		SendClientMessage(playerid, 0xFFAA00FF, "Voice chat: no microphone found, you can only listen.");
	}
	else
	{
		SendClientMessage(playerid, 0x66FF66FF, "Voice chat: hold B to talk nearby, Z to talk to everyone. F11 opens the voice menu.");
	}
	return 1;
}

public OnPlayerDisconnect(playerid, reason)
{
	if (gLocalStream[playerid] != VB_INVALID_STREAM)
	{
		VB_DeleteStream(gLocalStream[playerid]);
		gLocalStream[playerid] = VB_INVALID_STREAM;
	}
	return 1;
}

public OnPlayerActivationKeyPress(playerid, keyid)
{
	switch (keyid)
	{
		case VB_KEY_B: VB_AddSpeaker(gLocalStream[playerid], playerid);
		case VB_KEY_Z: VB_AddSpeaker(gGlobalStream, playerid);
	}
	return 1;
}

public OnPlayerActivationKeyRelease(playerid, keyid)
{
	switch (keyid)
	{
		case VB_KEY_B: VB_RemoveSpeaker(gLocalStream[playerid], playerid);
		case VB_KEY_Z: VB_RemoveSpeaker(gGlobalStream, playerid);
	}
	return 1;
}
