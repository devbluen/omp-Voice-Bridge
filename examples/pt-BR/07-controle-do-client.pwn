/*
 *  Exemplo 07 do Voice Bridge - controle do client de voz
 *
 *  - Só o client Voice Bridge é aceito (SampVoice fica sem voz)
 *  - Quem tiver um client mais antigo que VERSAO_MINIMA é kickado
 *  - Ninguém vê quem está falando (bom para roleplay)
 *
 *  /vversao <id>    mostra o client de voz de um jogador
 */

#include <open.mp>
#include <voice-bridge>

#define VERSAO_MINIMA         VB_VERSION(1, 0, 0)
#define VERSAO_MINIMA_TEXTO   "1.0.0"

new bool:gVerificado[MAX_PLAYERS];

main() {}

public OnGameModeInit()
{
	// Mesmo que voice_allow_sampvoice false na configuração.
	VB_AllowClientType(VB_CLIENT_SAMPVOICE, false);
	return 1;
}

forward KickarJogador(playerid);
public KickarJogador(playerid)
{
	Kick(playerid);
	return 1;
}

VerificarClientVoz(playerid)
{
	new tipo = VB_GetClientType(playerid);
	if (gVerificado[playerid] || tipo == VB_CLIENT_NONE || !IsPlayerConnected(playerid))
	{
		return;
	}
	gVerificado[playerid] = true;

	if (tipo == VB_CLIENT_SAMPVOICE)
	{
		SendClientMessage(playerid, 0xFFAA00FF, "Este servidor usa o Voice Bridge: troque o sampvoice.asi pelo voice-bridge.asi para falar.");
		return;
	}

	if (VB_GetClientBuild(playerid) < VERSAO_MINIMA)
	{
		new versao[16], mensagem[144];
		VB_GetClientVersionString(playerid, versao);
		format(mensagem, sizeof mensagem, "Seu client de voz (%s) está desatualizado. Instale o Voice Bridge %s ou mais novo.", versao, VERSAO_MINIMA_TEXTO);
		SendClientMessage(playerid, 0xFF5555FF, mensagem);
		SetTimerEx("KickarJogador", 500, false, "i", playerid); // dá tempo da mensagem chegar
		return;
	}

	VB_SetPlayerSpeakerList(playerid, false);
}

// Normalmente é chamado antes do OnPlayerConnect; os dois casos são tratados.
public VB_OnPlayerClientDetected(playerid, version, bool:extended, bool:hasmicro)
{
	VerificarClientVoz(playerid);
	return 1;
}

public OnPlayerConnect(playerid)
{
	VerificarClientVoz(playerid);
	return 1;
}

public OnPlayerDisconnect(playerid, reason)
{
	gVerificado[playerid] = false;
	return 1;
}

public OnPlayerCommandText(playerid, cmdtext[])
{
	if (!strcmp(cmdtext, "/vversao", true, 8))
	{
		new alvo = strval(cmdtext[9]);
		if (!IsPlayerConnected(alvo))
		{
			return SendClientMessage(playerid, 0xFF5555FF, "Uso: /vversao <playerid>"), 1;
		}
		new versao[16], major, minor, patch, mensagem[144];
		VB_GetClientVersionString(alvo, versao);
		switch (VB_GetClientVersionNumbers(alvo, major, minor, patch))
		{
			case VB_CLIENT_VOICEBRIDGE: format(mensagem, sizeof mensagem, "Jogador %d usa Voice Bridge %s (major %d, minor %d, patch %d)", alvo, versao, major, minor, patch);
			case VB_CLIENT_SAMPVOICE: format(mensagem, sizeof mensagem, "Jogador %d usa SampVoice %s", alvo, versao);
			default: format(mensagem, sizeof mensagem, "Jogador %d não tem client de voz", alvo);
		}
		SendClientMessage(playerid, 0xFFFFFFFF, mensagem);
		return 1;
	}
	return 0;
}
