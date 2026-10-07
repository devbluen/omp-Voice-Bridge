/*
 *  Exemplo 05 do Voice Bridge - moderação
 *
 *  /vinfo <id>      client de voz, versão e microfone de um jogador
 *  /vmute <id>      bloqueia o microfone de um jogador
 *  /vunmute <id>
 *  /vblock <id>     você deixa de ouvir um jogador (só você)
 */

#include <open.mp>
#include <voice-bridge>

main() {}

public VB_OnPlayerStartTalking(playerid)
{
	SetPlayerChatBubble(playerid, "(falando)", 0xCCCCCCFF, 15.0, 1000);
	return 1;
}

public VB_OnPlayerClientStatus(playerid, bool:micavailable, bool:micmuted, bool:soundmuted)
{
	if (soundmuted)
	{
		SendClientMessage(playerid, 0xFFAA00FF, "Você desligou o chat de voz no menu de voz (F11).");
	}
	return 1;
}

MostrarInfoVoz(playerid, alvo)
{
	new client[16], versao[16], mensagem[144];
	switch (VB_GetClientType(alvo))
	{
		case VB_CLIENT_VOICEBRIDGE: client = "Voice Bridge";
		case VB_CLIENT_SAMPVOICE: client = "SampVoice";
		default: client = "nenhum";
	}
	VB_GetClientVersionString(alvo, versao);
	format(mensagem, sizeof mensagem, "Jogador %d: %s %s | microfone: %s | falando: %s | silenciado: %s",
		alvo, client, versao,
		VB_HasMicrophone(alvo) ? ("sim") : ("não"),
		VB_IsTalking(alvo) ? ("sim") : ("não"),
		VB_IsPlayerMuted(alvo) ? ("sim") : ("não"));
	SendClientMessage(playerid, 0xFFFFFFFF, mensagem);
}

// "/cmd 12" -> comando = "/cmd", alvo = 12
bool:LerComando(const texto[], comando[16], &alvo)
{
	new espaco = strfind(texto, " ");
	if (espaco == -1)
	{
		return false;
	}
	strmid(comando, texto, 0, espaco, 16);
	alvo = strval(texto[espaco + 1]);
	return bool:IsPlayerConnected(alvo);
}

public OnPlayerCommandText(playerid, cmdtext[])
{
	new comando[16], alvo;
	if (!LerComando(cmdtext, comando, alvo))
	{
		return 0;
	}
	if (!strcmp(comando, "/vinfo", true))
	{
		MostrarInfoVoz(playerid, alvo);
		return 1;
	}
	if (!strcmp(comando, "/vmute", true))
	{
		VB_MutePlayer(alvo, true);
		VB_Notify(alvo, "Você foi silenciado por um administrador", 0xFF5555FF, 5000);
		return 1;
	}
	if (!strcmp(comando, "/vunmute", true))
	{
		VB_MutePlayer(alvo, false);
		VB_Notify(alvo, "Você pode falar novamente", 0x66FF66FF);
		return 1;
	}
	if (!strcmp(comando, "/vblock", true))
	{
		new bool:bloqueado = !VB_IsSpeakerBlocked(playerid, alvo);
		VB_BlockSpeaker(playerid, alvo, bloqueado);
		SendClientMessage(playerid, 0xFFFFFFFF, bloqueado ? ("Jogador bloqueado.") : ("Jogador desbloqueado."));
		return 1;
	}
	return 0;
}
