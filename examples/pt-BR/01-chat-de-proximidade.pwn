/*
 *  Exemplo 01 do Voice Bridge - chat de proximidade + chat global
 *
 *  B   fala com os jogadores perto de você (som em 3D)
 *  Z   fala com todos
 */

#include <open.mp>      // ou <a_samp>
#include <voice-bridge>

#define DISTANCIA_LOCAL  (35.0)

new gStreamGlobal = VB_INVALID_STREAM;
new gStreamLocal[MAX_PLAYERS] = { VB_INVALID_STREAM, ... };

main() {}

public OnGameModeInit()
{
	gStreamGlobal = VB_CreateGlobalStream(0xFFFFAA00, "Global");
	return 1;
}

ConfigurarVoz(playerid)
{
	if (gStreamLocal[playerid] != VB_INVALID_STREAM || !VB_HasPlugin(playerid))
	{
		return;
	}
	// Stream dinâmico: quem estiver perto de playerid entra sozinho.
	gStreamLocal[playerid] = VB_CreateDynamicStreamAtPlayer(DISTANCIA_LOCAL, VB_INFINITE, playerid, 0xFF66FF66, "Local");
	VB_AddListener(gStreamGlobal, playerid);
	VB_AddKey(playerid, VB_KEY_B);
	VB_AddKey(playerid, VB_KEY_Z);
}

public VB_OnPlayerClientDetected(playerid, version, bool:extended, bool:hasmicro)
{
	ConfigurarVoz(playerid);
	return 1;
}

public OnPlayerConnect(playerid)
{
	ConfigurarVoz(playerid); // também cobre o GMX, quando os jogadores continuam conectados

	if (!VB_HasPlugin(playerid))
	{
		SendClientMessage(playerid, 0xFF5555FF, "Chat de voz: instale o voice-bridge.asi para falar com os outros jogadores.");
	}
	else if (!VB_HasMicrophone(playerid))
	{
		SendClientMessage(playerid, 0xFFAA00FF, "Chat de voz: nenhum microfone encontrado, você só pode ouvir.");
	}
	else
	{
		SendClientMessage(playerid, 0x66FF66FF, "Chat de voz: segure B para falar perto, Z para falar com todos. F11 abre o menu de voz.");
	}
	return 1;
}

public OnPlayerDisconnect(playerid, reason)
{
	if (gStreamLocal[playerid] != VB_INVALID_STREAM)
	{
		VB_DeleteStream(gStreamLocal[playerid]);
		gStreamLocal[playerid] = VB_INVALID_STREAM;
	}
	return 1;
}

public OnPlayerActivationKeyPress(playerid, keyid)
{
	switch (keyid)
	{
		case VB_KEY_B: VB_AddSpeaker(gStreamLocal[playerid], playerid);
		case VB_KEY_Z: VB_AddSpeaker(gStreamGlobal, playerid);
	}
	return 1;
}

public OnPlayerActivationKeyRelease(playerid, keyid)
{
	switch (keyid)
	{
		case VB_KEY_B: VB_RemoveSpeaker(gStreamLocal[playerid], playerid);
		case VB_KEY_Z: VB_RemoveSpeaker(gStreamGlobal, playerid);
	}
	return 1;
}
