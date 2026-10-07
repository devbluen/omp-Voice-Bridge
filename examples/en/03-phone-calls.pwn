/*
 *  Voice Bridge example 03 - phone calls
 *
 *  /call <playerid>  starts a call (both players talk with an open mic)
 *  /hangup           ends it
 */

#include <open.mp>
#include <voice-bridge>

new gCallStream[MAX_PLAYERS] = { VB_INVALID_STREAM, ... };
new gCallPartner[MAX_PLAYERS] = { INVALID_PLAYER_ID, ... };
new gPhoneEffect = VB_INVALID_EFFECT;

main() {}

public OnGameModeInit()
{
	gPhoneEffect = VB_CreatePresetEffect(VB_PRESET_PHONE);
	return 1;
}

StartCall(caller, target)
{
	new stream = VB_CreateGlobalStream(0xFF66CCFF, "Phone");
	VB_AttachEffect(gPhoneEffect, stream);
	VB_SetStreamFlat(stream, true);

	VB_AddListener(stream, caller);
	VB_AddSpeaker(stream, caller);
	VB_AddListener(stream, target);
	VB_AddSpeaker(stream, target);

	// Both microphones stay open while the call lasts.
	VB_StartRecord(caller);
	VB_StartRecord(target);

	gCallStream[caller] = stream;
	gCallStream[target] = stream;
	gCallPartner[caller] = target;
	gCallPartner[target] = caller;

	VB_Notify(caller, "Call started", 0x66CCFFFF);
	VB_Notify(target, "Incoming call answered", 0x66CCFFFF);
}

EndCall(playerid)
{
	new stream = gCallStream[playerid];
	if (stream == VB_INVALID_STREAM)
	{
		return;
	}
	new partner = gCallPartner[playerid];
	VB_DeleteStream(stream);
	VB_StopRecord(playerid);
	gCallStream[playerid] = VB_INVALID_STREAM;
	gCallPartner[playerid] = INVALID_PLAYER_ID;
	if (partner != INVALID_PLAYER_ID)
	{
		VB_StopRecord(partner);
		gCallStream[partner] = VB_INVALID_STREAM;
		gCallPartner[partner] = INVALID_PLAYER_ID;
		VB_Notify(partner, "Call ended", 0xFF6666FF);
	}
	VB_Notify(playerid, "Call ended", 0xFF6666FF);
}

public OnPlayerDisconnect(playerid, reason)
{
	EndCall(playerid);
	return 1;
}

public OnPlayerCommandText(playerid, cmdtext[])
{
	if (!strcmp(cmdtext, "/call", true, 5))
	{
		new target = strval(cmdtext[6]);
		if (!IsPlayerConnected(target) || target == playerid)
		{
			return SendClientMessage(playerid, 0xFF5555FF, "Usage: /call <playerid>"), 1;
		}
		if (!VB_HasPlugin(playerid) || !VB_HasPlugin(target))
		{
			return SendClientMessage(playerid, 0xFF5555FF, "Both players need the voice client."), 1;
		}
		if (gCallStream[playerid] != VB_INVALID_STREAM || gCallStream[target] != VB_INVALID_STREAM)
		{
			return SendClientMessage(playerid, 0xFF5555FF, "Someone is already in a call."), 1;
		}
		StartCall(playerid, target);
		return 1;
	}
	if (!strcmp(cmdtext, "/hangup", true))
	{
		EndCall(playerid);
		return 1;
	}
	return 0;
}
