/*
 *  Exemplo 04 do Voice Bridge - rádio do veículo e megafone
 *
 *  X            dentro de um veículo: fala só com quem está nele
 *  /megafone    liga/desliga uma voz alta ouvida a 80 m de você
 */

#include <open.mp>
#include <voice-bridge>

new gStreamVeiculo[MAX_VEHICLES] = { VB_INVALID_STREAM, ... };
new gStreamVeiculoJogador[MAX_PLAYERS] = { VB_INVALID_STREAM, ... };
new gStreamMegafone[MAX_PLAYERS] = { VB_INVALID_STREAM, ... };
new gEfeitoMegafone = VB_INVALID_EFFECT;

main() {}

public OnGameModeInit()
{
	gEfeitoMegafone = VB_CreatePresetEffect(VB_PRESET_MEGAPHONE);
	return 1;
}

public VB_OnPlayerClientDetected(playerid, version, bool:extended, bool:hasmicro)
{
	VB_AddKey(playerid, VB_KEY_X);
	return 1;
}

SairStreamVeiculo(playerid)
{
	new stream = gStreamVeiculoJogador[playerid];
	if (stream != VB_INVALID_STREAM)
	{
		VB_RemoveListener(stream, playerid);
		VB_RemoveSpeaker(stream, playerid);
		gStreamVeiculoJogador[playerid] = VB_INVALID_STREAM;
	}
}

public OnPlayerStateChange(playerid, PLAYER_STATE:newstate, PLAYER_STATE:oldstate)
{
	if (newstate == PLAYER_STATE_DRIVER || newstate == PLAYER_STATE_PASSENGER)
	{
		new vehicleid = GetPlayerVehicleID(playerid);
		if (gStreamVeiculo[vehicleid] == VB_INVALID_STREAM)
		{
			// Stream estático: o script escolhe quem ouve.
			gStreamVeiculo[vehicleid] = VB_CreateStaticStreamAtVehicle(10.0, vehicleid, 0xFFAAAAAA, "Veículo");
			VB_SetStreamFlat(gStreamVeiculo[vehicleid], true);
		}
		VB_AddListener(gStreamVeiculo[vehicleid], playerid);
		gStreamVeiculoJogador[playerid] = gStreamVeiculo[vehicleid];
	}
	else if (oldstate == PLAYER_STATE_DRIVER || oldstate == PLAYER_STATE_PASSENGER)
	{
		SairStreamVeiculo(playerid);
	}
	return 1;
}

public OnPlayerActivationKeyPress(playerid, keyid)
{
	if (keyid == VB_KEY_X && gStreamVeiculoJogador[playerid] != VB_INVALID_STREAM)
	{
		VB_AddSpeaker(gStreamVeiculoJogador[playerid], playerid);
	}
	return 1;
}

public OnPlayerActivationKeyRelease(playerid, keyid)
{
	if (keyid == VB_KEY_X && gStreamVeiculoJogador[playerid] != VB_INVALID_STREAM)
	{
		VB_RemoveSpeaker(gStreamVeiculoJogador[playerid], playerid);
	}
	return 1;
}

AlternarMegafone(playerid)
{
	if (gStreamMegafone[playerid] != VB_INVALID_STREAM)
	{
		VB_DeleteStream(gStreamMegafone[playerid]);
		gStreamMegafone[playerid] = VB_INVALID_STREAM;
		VB_StopRecord(playerid);
		VB_Notify(playerid, "Megafone desligado");
		return;
	}
	new stream = VB_CreateDynamicStreamAtPlayer(80.0, VB_INFINITE, playerid, 0xFF3366FF, "Megafone");
	VB_AttachEffect(gEfeitoMegafone, stream);
	VB_AddSpeaker(stream, playerid);
	VB_StartRecord(playerid); // microfone aberto, sem precisar de tecla
	gStreamMegafone[playerid] = stream;
	VB_Notify(playerid, "Megafone ligado: você é ouvido a 80 m", 0xFF3366FF);
}

public OnPlayerDisconnect(playerid, reason)
{
	SairStreamVeiculo(playerid);
	if (gStreamMegafone[playerid] != VB_INVALID_STREAM)
	{
		AlternarMegafone(playerid);
	}
	return 1;
}

public OnPlayerCommandText(playerid, cmdtext[])
{
	if (!strcmp(cmdtext, "/megafone", true))
	{
		AlternarMegafone(playerid);
		return 1;
	}
	return 0;
}
