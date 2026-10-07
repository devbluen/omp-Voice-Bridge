/*
 *  Voice Bridge example 07 - controlling the voice client
 *
 *  - Only the Voice Bridge client is accepted (SampVoice gets no voice)
 *  - Players with a client older than MIN_VERSION are kicked
 *  - Nobody sees who is talking (good for roleplay)
 *
 *  /vversion <id>   shows the voice client of a player
 */

#include <open.mp>
#include <voice-bridge>

#define MIN_VERSION         VB_VERSION(1, 0, 0)
#define MIN_VERSION_TEXT    "1.0.0"

new bool:gChecked[MAX_PLAYERS];

main() {}

public OnGameModeInit()
{
	// Same as voice_allow_sampvoice false in the configuration.
	VB_AllowClientType(VB_CLIENT_SAMPVOICE, false);
	return 1;
}

forward KickPlayer(playerid);
public KickPlayer(playerid)
{
	Kick(playerid);
	return 1;
}

CheckVoiceClient(playerid)
{
	new type = VB_GetClientType(playerid);
	if (gChecked[playerid] || type == VB_CLIENT_NONE || !IsPlayerConnected(playerid))
	{
		return;
	}
	gChecked[playerid] = true;

	if (type == VB_CLIENT_SAMPVOICE)
	{
		SendClientMessage(playerid, 0xFFAA00FF, "This server uses Voice Bridge: replace sampvoice.asi with voice-bridge.asi to talk.");
		return;
	}

	if (VB_GetClientBuild(playerid) < MIN_VERSION)
	{
		new version[16], message[144];
		VB_GetClientVersionString(playerid, version);
		format(message, sizeof message, "Your voice client (%s) is outdated. Install Voice Bridge %s or newer.", version, MIN_VERSION_TEXT);
		SendClientMessage(playerid, 0xFF5555FF, message);
		SetTimerEx("KickPlayer", 500, false, "i", playerid); // let the message arrive first
		return;
	}

	VB_SetPlayerSpeakerList(playerid, false);
}

// Usually called before OnPlayerConnect; both are handled.
public VB_OnPlayerClientDetected(playerid, version, bool:extended, bool:hasmicro)
{
	CheckVoiceClient(playerid);
	return 1;
}

public OnPlayerConnect(playerid)
{
	CheckVoiceClient(playerid);
	return 1;
}

public OnPlayerDisconnect(playerid, reason)
{
	gChecked[playerid] = false;
	return 1;
}

public OnPlayerCommandText(playerid, cmdtext[])
{
	if (!strcmp(cmdtext, "/vversion", true, 9))
	{
		new target = strval(cmdtext[10]);
		if (!IsPlayerConnected(target))
		{
			return SendClientMessage(playerid, 0xFF5555FF, "Usage: /vversion <playerid>"), 1;
		}
		new version[16], major, minor, patch, message[144];
		VB_GetClientVersionString(target, version);
		switch (VB_GetClientVersionNumbers(target, major, minor, patch))
		{
			case VB_CLIENT_VOICEBRIDGE: format(message, sizeof message, "Player %d uses Voice Bridge %s (major %d, minor %d, patch %d)", target, version, major, minor, patch);
			case VB_CLIENT_SAMPVOICE: format(message, sizeof message, "Player %d uses SampVoice %s", target, version);
			default: format(message, sizeof message, "Player %d has no voice client", target);
		}
		SendClientMessage(playerid, 0xFFFFFFFF, message);
		return 1;
	}
	return 0;
}
