/*
 *  Exemplo 08 do Voice Bridge - efeitos personalizados (sem preset)
 *
 *  /voz <nome>   muda como sua voz soa para quem está perto:
 *                normal, eco, salao, radio, robo, monstro
 *
 *  Como os efeitos funcionam:
 *  - VB_Create...Effect(prioridade, parâmetros...) cria um efeito e retorna o id dele.
 *  - VB_AttachEffect(efeito, stream) aplica o efeito em tudo que se ouve nesse stream.
 *  - Vários efeitos podem ser colocados no mesmo stream: eles formam uma
 *    corrente, e o de MAIOR prioridade é aplicado primeiro.
 *  - Um efeito pode estar em vários streams ao mesmo tempo.
 *  - Os parâmetros e seus limites estão no README (Efeitos).
 */

#include <open.mp>
#include <voice-bridge>

#define MAX_CORRENTE (3)

enum
{
	VOZ_NORMAL,
	VOZ_ECO,
	VOZ_SALAO,
	VOZ_RADIO,
	VOZ_ROBO,
	VOZ_MONSTRO,
	TOTAL_VOZES
}

new const gNomesVoz[TOTAL_VOZES][] = { "normal", "eco", "salao", "radio", "robo", "monstro" };

// Efeitos de cada voz (VB_INVALID_EFFECT = posição vazia)
new gEfeitosVoz[TOTAL_VOZES][MAX_CORRENTE];

new gStreamLocal[MAX_PLAYERS] = { VB_INVALID_STREAM, ... };
new gVozJogador[MAX_PLAYERS];

main() {}

CriarEfeitosDeVoz()
{
	// Eco: 30% de efeito, 40% de repetição, 300 ms dos dois lados, sem alternar lados
	gEfeitosVoz[VOZ_ECO][0] = VB_CreateEchoEffect(0, 30.0, 40.0, 300.0, 300.0, false);

	// Salão: ganho de entrada 0 dB, mistura do reverb -6 dB, cauda de 1800 ms, agudos 0.5
	gEfeitosVoz[VOZ_SALAO][0] = VB_CreateReverbEffect(0, 0.0, -6.0, 1800.0, 0.5);

	// Rádio antigo = 3 efeitos em corrente (prioridade 3 roda primeiro)
	gEfeitosVoz[VOZ_RADIO][0] = VB_CreateParamEqEffect(3, 1500.0, 12.0, 12.0);                     // realça os médios
	gEfeitosVoz[VOZ_RADIO][1] = VB_CreateDistortionEffect(2, -20.0, 20.0, 2500.0, 2400.0, 4000.0); // chiado
	gEfeitosVoz[VOZ_RADIO][2] = VB_CreateCompressorEffect(1, 8.0, 5.0, 200.0, -20.0, 4.0, 2.0);    // volume uniforme

	// Robô: flanger rápido (onda senoidal) + gargle a 40 Hz
	gEfeitosVoz[VOZ_ROBO][0] = VB_CreateFlangerEffect(2, 60.0, 80.0, 70.0, 6.0, 1, 2.0, 2);
	gEfeitosVoz[VOZ_ROBO][1] = VB_CreateGargleEffect(1, 40, 0);

	// Monstro: muito grave, sem agudos e um pouco de distorção
	gEfeitosVoz[VOZ_MONSTRO][0] = VB_CreateParamEqEffect(3, 120.0, 24.0, 15.0);
	gEfeitosVoz[VOZ_MONSTRO][1] = VB_CreateParamEqEffect(2, 5000.0, 36.0, -15.0);
	gEfeitosVoz[VOZ_MONSTRO][2] = VB_CreateDistortionEffect(1, -25.0, 35.0, 600.0, 800.0, 3000.0);
}

public OnGameModeInit()
{
	for (new v = 0; v < TOTAL_VOZES; v++)
	{
		for (new i = 0; i < MAX_CORRENTE; i++)
		{
			gEfeitosVoz[v][i] = VB_INVALID_EFFECT;
		}
	}
	CriarEfeitosDeVoz();
	return 1;
}

DefinirVoz(playerid, voz)
{
	new stream = gStreamLocal[playerid];
	// Tira os efeitos da voz anterior...
	for (new i = 0; i < MAX_CORRENTE; i++)
	{
		if (gEfeitosVoz[gVozJogador[playerid]][i] != VB_INVALID_EFFECT)
		{
			VB_DetachEffect(gEfeitosVoz[gVozJogador[playerid]][i], stream);
		}
	}
	// ...e coloca os novos. Só o playerid fala nesse stream, então só a voz
	// dele muda.
	for (new i = 0; i < MAX_CORRENTE; i++)
	{
		if (gEfeitosVoz[voz][i] != VB_INVALID_EFFECT)
		{
			VB_AttachEffect(gEfeitosVoz[voz][i], stream);
		}
	}
	gVozJogador[playerid] = voz;
}

public VB_OnPlayerClientDetected(playerid, version, bool:extended, bool:hasmicro)
{
	if (gStreamLocal[playerid] == VB_INVALID_STREAM)
	{
		gStreamLocal[playerid] = VB_CreateDynamicStreamAtPlayer(35.0, VB_INFINITE, playerid, 0xFF66FF66, "Local");
		gVozJogador[playerid] = VOZ_NORMAL;
		VB_AddKey(playerid, VB_KEY_B);
	}
	return 1;
}

public OnPlayerDisconnect(playerid, reason)
{
	if (gStreamLocal[playerid] != VB_INVALID_STREAM)
	{
		VB_DeleteStream(gStreamLocal[playerid]); // os efeitos continuam para os outros jogadores
		gStreamLocal[playerid] = VB_INVALID_STREAM;
	}
	return 1;
}

public OnPlayerActivationKeyPress(playerid, keyid)
{
	if (keyid == VB_KEY_B) VB_AddSpeaker(gStreamLocal[playerid], playerid);
	return 1;
}

public OnPlayerActivationKeyRelease(playerid, keyid)
{
	if (keyid == VB_KEY_B) VB_RemoveSpeaker(gStreamLocal[playerid], playerid);
	return 1;
}

public OnPlayerCommandText(playerid, cmdtext[])
{
	if (!strcmp(cmdtext, "/voz", true, 4))
	{
		if (gStreamLocal[playerid] == VB_INVALID_STREAM)
		{
			return SendClientMessage(playerid, 0xFF5555FF, "Você precisa do client de voz (voice-bridge.asi)."), 1;
		}
		for (new v = 0; v < TOTAL_VOZES; v++)
		{
			if (cmdtext[4] == ' ' && !strcmp(cmdtext[5], gNomesVoz[v], true))
			{
				DefinirVoz(playerid, v);
				new mensagem[64];
				format(mensagem, sizeof mensagem, "Voz: %s", gNomesVoz[v]);
				VB_Notify(playerid, mensagem, 0x66CCFFFF, 2000);
				return 1;
			}
		}
		return SendClientMessage(playerid, 0xFFFFFFFF, "Uso: /voz normal | eco | salao | radio | robo | monstro"), 1;
	}
	return 0;
}
