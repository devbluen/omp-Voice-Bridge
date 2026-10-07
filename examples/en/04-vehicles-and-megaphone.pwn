/*
 *  Voice Bridge example 04 - vehicle intercom and megaphone
 *
 *  X            inside a vehicle: talk only to the people in it
 *  /megaphone   toggles a loud voice heard 80 m around you
 */

#include <open.mp>
#include <voice-bridge>

new gVehicleStream[MAX_VEHICLES] = { VB_INVALID_STREAM, ... };
new gPlayerVehicleStream[MAX_PLAYERS] = { VB_INVALID_STREAM, ... };
new gMegaphoneStream[MAX_PLAYERS] = { VB_INVALID_STREAM, ... };
new gMegaphoneEffect = VB_INVALID_EFFECT;

main() {}

public OnGameModeInit()
{
	gMegaphoneEffect = VB_CreatePresetEffect(VB_PRESET_MEGAPHONE);
	return 1;
}

public VB_OnPlayerClientDetected(playerid, version, bool:extended, bool:hasmicro)
{
	VB_AddKey(playerid, VB_KEY_X);
	return 1;
}

LeaveVehicleStream(playerid)
{
	new stream = gPlayerVehicleStream[playerid];
	if (stream != VB_INVALID_STREAM)
	{
		VB_RemoveListener(stream, playerid);
		VB_RemoveSpeaker(stream, playerid);
		gPlayerVehicleStream[playerid] = VB_INVALID_STREAM;
	}
}

public OnPlayerStateChange(playerid, PLAYER_STATE:newstate, PLAYER_STATE:oldstate)
{
	if (newstate == PLAYER_STATE_DRIVER || newstate == PLAYER_STATE_PASSENGER)
	{
		new vehicleid = GetPlayerVehicleID(playerid);
		if (gVehicleStream[vehicleid] == VB_INVALID_STREAM)
		{
			// Static stream: listeners are chosen by the script.
			gVehicleStream[vehicleid] = VB_CreateStaticStreamAtVehicle(10.0, vehicleid, 0xFFAAAAAA, "Vehicle");
			VB_SetStreamFlat(gVehicleStream[vehicleid], true);
		}
		VB_AddListener(gVehicleStream[vehicleid], playerid);
		gPlayerVehicleStream[playerid] = gVehicleStream[vehicleid];
	}
	else if (oldstate == PLAYER_STATE_DRIVER || oldstate == PLAYER_STATE_PASSENGER)
	{
		LeaveVehicleStream(playerid);
	}
	return 1;
}

public OnPlayerActivationKeyPress(playerid, keyid)
{
	if (keyid == VB_KEY_X && gPlayerVehicleStream[playerid] != VB_INVALID_STREAM)
	{
		VB_AddSpeaker(gPlayerVehicleStream[playerid], playerid);
	}
	return 1;
}

public OnPlayerActivationKeyRelease(playerid, keyid)
{
	if (keyid == VB_KEY_X && gPlayerVehicleStream[playerid] != VB_INVALID_STREAM)
	{
		VB_RemoveSpeaker(gPlayerVehicleStream[playerid], playerid);
	}
	return 1;
}

ToggleMegaphone(playerid)
{
	if (gMegaphoneStream[playerid] != VB_INVALID_STREAM)
	{
		VB_DeleteStream(gMegaphoneStream[playerid]);
		gMegaphoneStream[playerid] = VB_INVALID_STREAM;
		VB_StopRecord(playerid);
		VB_Notify(playerid, "Megaphone off");
		return;
	}
	new stream = VB_CreateDynamicStreamAtPlayer(80.0, VB_INFINITE, playerid, 0xFF3366FF, "Megaphone");
	VB_AttachEffect(gMegaphoneEffect, stream);
	VB_AddSpeaker(stream, playerid);
	VB_StartRecord(playerid); // open microphone, no key needed
	gMegaphoneStream[playerid] = stream;
	VB_Notify(playerid, "Megaphone on: you are heard 80 m around you", 0xFF3366FF);
}

public OnPlayerDisconnect(playerid, reason)
{
	LeaveVehicleStream(playerid);
	if (gMegaphoneStream[playerid] != VB_INVALID_STREAM)
	{
		ToggleMegaphone(playerid);
	}
	return 1;
}

public OnPlayerCommandText(playerid, cmdtext[])
{
	if (!strcmp(cmdtext, "/megaphone", true))
	{
		ToggleMegaphone(playerid);
		return 1;
	}
	return 0;
}
