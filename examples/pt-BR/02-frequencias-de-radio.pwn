/*
 *  Exemplo 02 do Voice Bridge - frequências de rádio
 *
 *  /freq <1-100>  sintoniza o rádio (0 desliga)
 *  N              fala no rádio (ouvido com efeito de rádio)
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
			return SendClientMessage(playerid, 0xFF5555FF, "Você precisa do client de voz (voice-bridge.asi)."), 1;
		}
		new frequency = strval(cmdtext[6]);
		if (frequency < 0 || frequency > MAX_FREQUENCIES)
		{
			return SendClientMessage(playerid, 0xFF5555FF, "Uso: /freq <1-100> (0 = desligar)"), 1;
		}
		SetPlayerFrequency(playerid, frequency);
		new message[64];
		if (frequency)
		{
			format(message, sizeof message, "Rádio na frequência %d. Segure N para falar.", frequency);
		}
		else
		{
			message = "Rádio desligado.";
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
