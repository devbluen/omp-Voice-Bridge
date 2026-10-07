/*
 *  Voice Bridge example 02 - radio frequencies
 *
 *  /freq <1-100>  tunes the radio (0 turns it off)
 *  N              talk on the radio (heard with a radio effect)
 */

#include <open.mp>
#include <voice-bridge>

#define MAX_FREQUENCIES (100)

new gFrequencyStream[MAX_FREQUENCIES + 1];
new gPlayerFrequency[MAX_PLAYERS];
new gRadioEffect = VB_INVALID_EFFECT;

main() {}

public OnGameModeInit()
{
	gRadioEffect = VB_CreatePresetEffect(VB_PRESET_RADIO);
	return 1;
}

GetFrequencyStream(frequency)
{
	if (gFrequencyStream[frequency] == VB_INVALID_STREAM)
	{
		new name[16];
		format(name, sizeof name, "Radio %d", frequency);
		gFrequencyStream[frequency] = VB_CreateGlobalStream(0xFF55AAFF, name);
		VB_AttachEffect(gRadioEffect, gFrequencyStream[frequency]);
	}
	return gFrequencyStream[frequency];
}

SetPlayerFrequency(playerid, frequency)
{
	new current = gPlayerFrequency[playerid];
	if (current)
	{
		VB_RemoveListener(gFrequencyStream[current], playerid);
		VB_RemoveSpeaker(gFrequencyStream[current], playerid);
	}
	gPlayerFrequency[playerid] = frequency;
	if (frequency)
	{
		VB_AddListener(GetFrequencyStream(frequency), playerid);
		VB_AddKey(playerid, VB_KEY_N);
	}
	else
	{
		VB_RemoveKey(playerid, VB_KEY_N);
	}
}

public OnPlayerConnect(playerid)
{
	gPlayerFrequency[playerid] = 0;
	return 1;
}

public OnPlayerDisconnect(playerid, reason)
{
	SetPlayerFrequency(playerid, 0);
	return 1;
}

public OnPlayerCommandText(playerid, cmdtext[])
{
	if (!strcmp(cmdtext, "/freq", true, 5))
	{
		if (!VB_HasPlugin(playerid))
		{
			return SendClientMessage(playerid, 0xFF5555FF, "You need the voice client (voice-bridge.asi)."), 1;
		}
		new frequency = strval(cmdtext[6]);
		if (frequency < 0 || frequency > MAX_FREQUENCIES)
		{
			return SendClientMessage(playerid, 0xFF5555FF, "Usage: /freq <1-100> (0 = off)"), 1;
		}
		SetPlayerFrequency(playerid, frequency);
		new message[64];
		if (frequency)
		{
			format(message, sizeof message, "Radio tuned to %d. Hold N to talk.", frequency);
		}
		else
		{
			message = "Radio off.";
		}
		VB_Notify(playerid, message, 0x55AAFFFF, 3000);
		return 1;
	}
	return 0;
}

public OnPlayerActivationKeyPress(playerid, keyid)
{
	if (keyid == VB_KEY_N && gPlayerFrequency[playerid])
	{
		VB_AddSpeaker(gFrequencyStream[gPlayerFrequency[playerid]], playerid);
	}
	return 1;
}

public OnPlayerActivationKeyRelease(playerid, keyid)
{
	if (keyid == VB_KEY_N && gPlayerFrequency[playerid])
	{
		VB_RemoveSpeaker(gFrequencyStream[gPlayerFrequency[playerid]], playerid);
	}
	return 1;
}
