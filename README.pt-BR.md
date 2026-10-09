# Voice Bridge

Chat de voz para servidores **open.mp** e **SA-MP**, com client próprio e **compatível com o SampVoice**.

Criado por **[devbluen](https://github.com/devbluen)** · [Repositório](https://github.com/devbluen/omp-Voice-Bridge) · [Releases](https://github.com/devbluen/omp-Voice-Bridge/releases) · [English](README.md)

| | |
|---|---|
| **Servidor** | Um único arquivo funciona como componente do open.mp e como plugin do SA-MP (0.3.7-R2 e 0.3.DL), no Windows e no Linux. |
| **Client** | Um único `voice-bridge.asi` para SA-MP 0.3.7 R1, R2, R3, R4, R5 e 0.3.DL. Sem instalador. |
| **SampVoice** | Quem usa `sampvoice.asi` funciona no Voice Bridge, o `voice-bridge.asi` funciona em servidores SampVoice (3.x e o porte para open.mp) e gamemodes com natives `Sv*` rodam sem alterações. |
| **Conexão** | Porta de voz fixa e configurável. Se o UDP estiver bloqueado, a voz passa pela própria conexão do jogo (túnel). |
| **Áudio** | Som 3D a partir do personagem (direção, distância e eco do ambiente), efeitos (rádio, telefone, megafone...) e volume por jogador. |
| **Segurança** | Chave por jogador presa ao IP do jogo, bloqueio automático de quem envia pacotes inválidos e nenhum IP/porta exposto no client. |

## Sumário

1. [Instalação](#instalação)
2. [Configuração](#configuração)
3. [Primeiros passos em Pawn](#primeiros-passos-em-pawn)
4. [Exemplos](#exemplos)
5. [Controlando o client dos jogadores](#controlando-o-client-dos-jogadores)
6. [Referência da API](#referência-da-api)
7. [Client: menu e configurações](#client-menu-e-configurações)
8. [Segurança](#segurança)
9. [Solução de problemas](#solução-de-problemas)
10. [Migrando do SampVoice](#migrando-do-sampvoice)
11. [Compilando](#compilando)
12. [Como funciona](#como-funciona)

## Instalação

### Jogador

1. Copie `voice-bridge.asi` para a pasta do GTA San Andreas (onde está o `gta_sa.exe`).
2. Se tiver o `sampvoice.asi`, apague. O Voice Bridge também funciona em servidores SampVoice.
3. No jogo, **F11** abre o menu de voz.

É preciso um ASI Loader (a maioria das instalações de SA-MP/CLEO já tem). Se o mod não carregar, instale o *Silent's ASI Loader*.

### Servidor

| | open.mp | SA-MP 0.3.7-R2 / 0.3.DL |
|---|---|---|
| Plugin | `voice-bridge.dll`/`.so` em `components/` | `voice-bridge.dll`/`.so` em `plugins/` e `voice-bridge` (ou `voice-bridge.so`) na linha `plugins` do `server.cfg` |
| Includes | `include/*.inc` em `qawno/include/` | `include/*.inc` em `pawno/include/` |
| Firewall | Libere a **porta UDP de voz** (porta do jogo + 1) | igual |

A porta usada aparece no log ao iniciar:

```
[VoiceBridge] voice server listening on UDP 0.0.0.0:7778. Open this UDP port in the firewall/hosting panel.
```

> Se o arquivo estiver em `components/` **e** em `plugins/` no open.mp, só o componente é usado.

## Configuração

Tudo é opcional. Cada opção pode vir de uma variável de ambiente, do `config.json` (open.mp, dentro de `"voice_bridge"`) ou do `server.cfg` (prefixo `voice_`), nessa ordem.

```json
"voice_bridge": { "port": 7778, "bitrate": 24000 }
```

```
voice_port 7778
voice_bitrate 24000
```

**Rede**

| config.json | server.cfg | Padrão | Descrição |
|---|---|---|---|
| `port` | `voice_port` | 0 | Porta UDP de voz. `0` = porta do jogo + 1. |
| `bind` | `voice_bind` | - | IP local da porta de voz (padrão: o `bind` do servidor). |
| `public_host` | `voice_public_host` | - | Host anunciado aos clients (servidor atrás de proxy/anti-DDoS). |
| `tunnel` | `voice_tunnel` | true | Usa a conexão do jogo quando o UDP está bloqueado. |
| `force_tunnel` | `voice_force_tunnel` | false | Usa sempre o túnel (hosts que não permitem portas UDP). |
| `keepalive_ms` | `voice_keepalive_ms` | 5000 | Keep-alive (mantém o NAT aberto). |

**Áudio**

| config.json | server.cfg | Padrão | Descrição |
|---|---|---|---|
| `bitrate` | `voice_bitrate` | 24000 | Bitrate do Opus (6000-128000). |
| `frame_ms` | `voice_frame_ms` | 100 | Quadro dos clients Voice Bridge: 20, 40, 60 ou 100. O client SampVoice só toca 100. |
| `stream_tick_ms` | `voice_stream_tick_ms` | 100 | Atualização dos streams dinâmicos. |
| `position_rate_ms` | `voice_position_rate_ms` | 100 | Envio de posições aos clients Voice Bridge. |

**Clients**

| config.json | server.cfg | Padrão | Descrição |
|---|---|---|---|
| `allow_sampvoice` | `voice_allow_sampvoice` | true | Aceita o client SampVoice. |
| `allow_voicebridge` | `voice_allow_voicebridge` | true | Aceita o client Voice Bridge. |
| `allow_voice_activation` | `voice_allow_voice_activation` | false | Permite falar por ativação de voz (sem segurar a tecla). |
| `show_speaker_list` | `voice_show_speaker_list` | true | Mostra quem está falando. |
| `show_mic_icon` | `voice_show_mic_icon` | true | Mostra o ícone do microfone. |

**Segurança e diagnóstico**

| config.json | server.cfg | Padrão | Descrição |
|---|---|---|---|
| `strict_ip` | `voice_strict_ip` | false | Aceita voz só do IP do jogo. Desligado, outro IP (proxy/anti-DDoS) é aceito e registrado no log. |
| `max_packets_per_second` | `voice_max_packets_per_second` | 80 | Limite anti-flood por jogador. |
| `debug` | `voice_debug` | false | Registra os detalhes (quem ouve quem, veículos, voz descartada) no console e no log da voz. |
| `log_file` | `voice_log_file` | - | Log só da voz, com data e hora (os detalhes só com `debug` ligado). Padrão: `logs/voice-bridge.log` (open.mp) ou `voice-bridge.log`. `off` desliga. |

Variáveis de ambiente: `VOICE_BRIDGE_` + nome em maiúsculas (ex.: `VOICE_BRIDGE_PORT`, `VOICE_BRIDGE_MAX_PACKETS`).

> Vários servidores na mesma máquina? Defina `voice_port` em cada um: a porta padrão (jogo + 1) de um pode ser a porta de jogo do outro.

## Primeiros passos em Pawn

Um **stream** é uma sala de voz: **speakers** falam nela e **listeners** ouvem. Ninguém ouve a si mesmo.

| Tipo | Uso |
|---|---|
| Global | Todos ouvem no mesmo volume (rádio, telefone). |
| Estático | Posicionado em ponto/jogador/veículo/objeto; você escolhe quem ouve. |
| Dinâmico | Posicionado; quem estiver perto entra e sai sozinho. |

```pawn
#include <open.mp>      // ou <a_samp>
#include <voice-bridge>

new gLocal[MAX_PLAYERS] = { VB_INVALID_STREAM, ... };

public VB_OnPlayerClientDetected(playerid, version, bool:extended, bool:hasmicro)
{
    gLocal[playerid] = VB_CreateDynamicStreamAtPlayer(35.0, VB_INFINITE, playerid, 0xFF66FF66, "Local");
    VB_AddKey(playerid, VB_KEY_B);
    return 1;
}

public OnPlayerDisconnect(playerid, reason)
{
    VB_DeleteStream(gLocal[playerid]);
    gLocal[playerid] = VB_INVALID_STREAM;
    return 1;
}

public OnPlayerActivationKeyPress(playerid, keyid)
{
    if (keyid == VB_KEY_B) VB_AddSpeaker(gLocal[playerid], playerid);
    return 1;
}

public OnPlayerActivationKeyRelease(playerid, keyid)
{
    if (keyid == VB_KEY_B) VB_RemoveSpeaker(gLocal[playerid], playerid);
    return 1;
}
```

> Inclua `voice-bridge.inc` **depois** de `<open.mp>`/`<a_samp>`.

## Exemplos

Em [`examples/pt-BR`](examples/pt-BR) (versões em inglês em [`examples/en`](examples/en)):

| Arquivo | Mostra |
|---|---|
| `01-chat-de-proximidade.pwn` | Chat local em 3D (B) e global (Z). |
| `02-frequencias-de-radio.pwn` | `/freq` com efeito de rádio. |
| `03-ligacoes.pwn` | `/call` e `/hangup` com efeito de telefone e microfone aberto. |
| `04-veiculos-e-megafone.pwn` | Rádio interno do veículo e `/megafone`. |
| `05-ferramentas-de-admin.pwn` | `/vinfo`, `/vmute`, `/vblock`. |
| `06-compativel-com-sampvoice.pwn` | Gamemode oficial do SampVoice, sem alterações. |
| `07-controle-do-client.pwn` | Aceitar só o Voice Bridge, exigir versão mínima e esconder quem fala. |
| `08-efeitos-personalizados.pwn` | Efeitos sem preset: eco, salão, rádio antigo, robô e monstro, combinando vários efeitos. |

## Controlando o client dos jogadores

**Qual client o jogador usa**

```pawn
switch (VB_GetClientType(playerid))
{
    case VB_CLIENT_VOICEBRIDGE: // voice-bridge.asi
    case VB_CLIENT_SAMPVOICE:   // sampvoice.asi
    case VB_CLIENT_NONE:        // sem client de voz
}
```

**Versão do client**: bloquear uma versão específica ou exigir uma mínima

```pawn
new versao[16];
VB_GetClientVersionString(playerid, versao); // "1.0.0" (Voice Bridge) ou "3.1" (SampVoice)

if (VB_GetClientBuild(playerid) < VB_VERSION(1, 2, 0))
{
    SendClientMessage(playerid, -1, "Atualize o Voice Bridge para a versão 1.2.0.");
    // Kick com SetTimerEx para a mensagem chegar antes
}

new major, minor, patch;
if (VB_GetClientVersionNumbers(playerid, major, minor, patch) == VB_CLIENT_VOICEBRIDGE && major == 1 && minor == 1)
{
    // bloquear só a 1.1.x
}
```

A versão já é conhecida em `VB_OnPlayerClientDetected` (normalmente antes do `OnPlayerConnect`), inclusive para clients que não estão permitidos. A versão do SA-MP do jogador (R1, R3...) é dada pela native padrão `GetPlayerVersion`.

**Quais clients podem usar a voz**

```pawn
VB_AllowClientType(VB_CLIENT_SAMPVOICE, false); // igual a voice_allow_sampvoice false
```

**O que o jogador vê**

```pawn
VB_SetPlayerSpeakerList(playerid, false);     // não mostra quem está falando (nem envia os nomes)
VB_SetPlayerMicIcon(playerid, false);         // esconde o ícone do microfone
VB_SetPlayerVoiceActivation(playerid, true);  // permite ativação por voz só para ele
```

## Referência da API

Handles de stream e efeito são inteiros; `0` é inválido. Handles do SampVoice e do Voice Bridge são intercambiáveis. Lista completa em [`include/voice-bridge.inc`](include/voice-bridge.inc).

### Servidor

| Native | Descrição |
|---|---|
| `VB_GetPluginVersion(version[], size)` | Versão do plugin. |
| `VB_GetVoicePort()` | Porta UDP de voz. |
| `VB_SetDebug(bool:enabled)` | Log detalhado. |
| `VB_SetBitrate(bitrate)` / `VB_GetBitrate()` | Bitrate para quem conectar depois. |
| `VB_AllowClientType(type, bool:allowed)` / `VB_IsClientTypeAllowed(type)` | Quais clients podem usar a voz. |

### Jogadores: client

| Native | Descrição |
|---|---|
| `VB_HasPlugin(playerid)` | Tem client de voz ativo. |
| `VB_GetClientType(playerid)` | `VB_CLIENT_NONE`, `VB_CLIENT_SAMPVOICE` ou `VB_CLIENT_VOICEBRIDGE`. |
| `VB_IsExtendedClient(playerid)` | Usa o client Voice Bridge (recursos extras). |
| `VB_GetClientVersionString(playerid, version[], size)` | Versão em texto (`"1.0.0"`, `"3.1"`); retorna o tipo do client. |
| `VB_GetClientVersionNumbers(playerid, &major, &minor, &patch)` | Versão em números; retorna o tipo do client. |
| `VB_GetClientBuild(playerid)` | Versão do Voice Bridge como número (`VB_VERSION(1, 2, 0)` = 10200); 0 nos outros. |
| `VB_GetClientVersion(playerid)` | Protocolo SampVoice (11). |
| `VB_GetTransport(playerid)` | `VB_TRANSPORT_NONE`, `_UDP` ou `_TUNNEL`. |
| `VB_HasMicrophone(playerid)` | Tem microfone. |
| `VB_IsClientMicMuted(playerid)` / `VB_IsClientSoundMuted(playerid)` | Desligou o microfone / o som no menu. |
| `VB_IsTalking(playerid)` | Está falando agora. |

### Jogadores: falar

| Native | Descrição |
|---|---|
| `VB_AddKey` / `VB_RemoveKey` / `VB_HasKey` / `VB_RemoveAllKeys` | Teclas de falar (códigos VK, ex.: `VB_KEY_B`). |
| `VB_StartRecord` / `VB_StopRecord` / `VB_IsRecording` | Microfone aberto, sem tecla. |
| `VB_MutePlayer(playerid, bool:mute)` / `VB_IsPlayerMuted` | Bloqueia o microfone. |

### Jogadores: ouvir e tela

| Native | Descrição |
|---|---|
| `VB_BlockSpeaker(listenerid, speakerid, bool:block)` / `VB_IsSpeakerBlocked` | `listenerid` deixa de ouvir `speakerid`. |
| `VB_SetSpeakerVolume(listenerid, speakerid, Float:volume)` | Volume de um jogador só para outro. |
| `VB_SetPlayerSpeakerList` / `VB_SetPlayerMicIcon` / `VB_SetPlayerVoiceActivation` | Opções do client por jogador. |
| `VB_Notify(playerid, text[], color, duration)` | Notificação na tela. |

### Streams

| Native | Descrição |
|---|---|
| `VB_CreateGlobalStream(color, name[])` | Global. |
| `VB_CreateStaticStreamAt{Point,Player,Vehicle,Object}(Float:distance, ..., color, name[])` | Estático. |
| `VB_CreateDynamicStreamAt{Point,Player,Vehicle,Object}(Float:distance, maxlisteners, ..., color, name[])` | Dinâmico (`VB_INFINITE` = sem limite). |
| `VB_DeleteStream` / `VB_IsValidStream` / `VB_GetStreamType` | Gerenciamento. |
| `VB_SetStreamDistance` / `VB_GetStreamDistance` | Alcance. |
| `VB_SetStreamPosition` / `VB_GetStreamPosition` | Posição. |
| `VB_SetStreamMaxListeners` | Limite de ouvintes (dinâmico). |
| `VB_SetStreamWorld(stream, worldid, interiorid)` | Filtro de mundo/interior (dinâmico). |
| `VB_SetStreamFlat(stream, bool:flat)` | Sem 3D. |
| `VB_SetStreamVolume(stream, Float:volume)` | Volume. |
| `VB_AddListener` / `VB_RemoveListener` / `VB_HasListener` / `VB_RemoveAllListeners` / `VB_GetListenerCount` / `VB_GetListeners` | Ouvintes. |
| `VB_AddSpeaker` / `VB_RemoveSpeaker` / `VB_HasSpeaker` / `VB_RemoveAllSpeakers` / `VB_GetSpeakerCount` / `VB_GetSpeakers` | Quem fala. |
| `VB_SetStreamParameter` / `VB_GetStreamParameter` / `VB_HasStreamParameter` / `VB_ResetStreamParameter` | `VB_PARAM_VOLUME`, `_PANNING`, `_FREQUENCY`, `_EAXMIX`, `_SRC`. |
| `VB_SlideStreamParameter` / `VB_SlideStreamParameterTo` | Transição suave em `time` ms. |

### Efeitos

Um efeito é criado uma vez e colocado em um ou mais streams. Tudo o que se ouve no stream passa pelo efeito.

```pawn
new eco = VB_CreateEchoEffect(0, 30.0, 40.0, 300.0, 300.0, false); // cria
VB_AttachEffect(eco, stream);                                      // aplica no stream
VB_DetachEffect(eco, stream);                                      // tira do stream
VB_DeleteEffect(eco);                                              // apaga de vez
```

- **Prontos**: `VB_CreatePresetEffect(VB_PRESET_RADIO)`, e também `_PHONE`, `_MEGAPHONE`, `_HALL`, `_CAVE`, `_UNDERWATER`, `_ROBOT`, `_ECHO`, `_WALKIE_TALKIE`.
- **Combinar**: coloque vários efeitos no mesmo stream. O primeiro parâmetro (`priority`) define a ordem: **maior prioridade é aplicada primeiro**.
- Para mudar a voz de um jogador só, use um stream em que só ele fala (ex.: o stream local dele).
- Exemplo completo: [`08-efeitos-personalizados.pwn`](examples/pt-BR/08-efeitos-personalizados.pwn) (`/voz eco`, `radio`, `robo`, `monstro`...).

Parâmetros (efeitos DirectX 8, os mesmos do SampVoice):

| Native | Parâmetros (faixa) |
|---|---|
| `VB_CreateEchoEffect(priority, wetdrymix, feedback, leftdelay, rightdelay, pandelay)` | mistura 0-100 %, repetição 0-100 %, atraso esq./dir. 1-2000 ms, `pandelay` alterna os lados |
| `VB_CreateReverbEffect(priority, ingain, reverbmix, reverbtime, highfreqrtratio)` | ganho -96-0 dB, mistura -96-0 dB, duração 0.001-3000 ms, agudos 0.001-0.999 |
| `VB_CreateI3dl2ReverbEffect(priority, room, roomhf, roomrollofffactor, decaytime, decayhfratio, reflections, reflectionsdelay, reverb, reverbdelay, diffusion, density, hfreference)` | room/roomhf -10000-0 mB, rolloff 0-10, decaimento 0.1-20 s, decayhf 0.1-2, reflexões -10000-1000 mB, atraso 0-0.3 s, reverb -10000-2000 mB, atraso 0-0.1 s, difusão/densidade 0-100 %, referência 20-20000 Hz |
| `VB_CreateChorusEffect(priority, wetdrymix, depth, feedback, frequency, waveform, delay, phase)` | mistura 0-100 %, profundidade 0-100 %, repetição -99-99 %, frequência 0-10 Hz, onda 0 triangular / 1 senoidal, atraso 0-20 ms, fase 0-4 (-180°, -90°, 0°, 90°, 180°) |
| `VB_CreateFlangerEffect(priority, wetdrymix, depth, feedback, frequency, waveform, delay, phase)` | igual ao chorus, atraso 0-4 ms |
| `VB_CreateDistortionEffect(priority, gain, edge, posteqcenterfrequency, posteqbandwidth, prelowpasscutoff)` | ganho -60-0 dB, intensidade 0-100 %, centro/largura do EQ 100-8000 Hz, corte de agudos 100-8000 Hz |
| `VB_CreateCompressorEffect(priority, gain, attack, release, threshold, ratio, predelay)` | ganho -60-60 dB, ataque 0.01-500 ms, liberação 50-3000 ms, limiar -60-0 dB, razão 1-100, pré-atraso 0-4 ms |
| `VB_CreateParamEqEffect(priority, center, bandwidth, gain)` | frequência 80-16000 Hz, largura 1-36 semitons, ganho -15-15 dB |
| `VB_CreateGargleEffect(priority, ratehz, waveshape)` | frequência 1-1000 Hz, onda 0 triangular / 1 quadrada |

Outros: `VB_IsValidEffect(effect)`. Efeitos e streams são liberados sozinhos quando o script que os criou é descarregado.

### Callbacks

| Callback | Quando |
|---|---|
| `OnPlayerActivationKeyPress(playerid, keyid)` / `OnPlayerActivationKeyRelease` | Tecla de falar pressionada/solta. |
| `VB_OnPlayerClientDetected(playerid, version, bool:extended, bool:hasmicro)` | Client de voz detectado. |
| `VB_OnPlayerTransportChange(playerid, transport)` | Voz conectou, caiu ou foi para o túnel. |
| `VB_OnPlayerStartTalking(playerid)` / `VB_OnPlayerStopTalking(playerid)` | Começou/parou de falar. |
| `VB_OnPlayerClientStatus(playerid, bool:micavailable, bool:micmuted, bool:soundmuted)` | Mudou as opções de voz no menu. |
| `VB_OnPlayerVoiceIgnored(playerid, reason)` | O jogador está falando, mas a voz foi descartada (no máximo 1x a cada 10 s). `reason`: `VB_VOICE_IGNORED_NOT_SPEAKER` (não é speaker de nenhum stream, falta `VB_AddSpeaker`), `_NO_KEY` (sem tecla de falar), `_MUTED` (mutado), `_CLIENT_NOT_ALLOWED` (client bloqueado). |

## Client: menu e configurações

**F11** abre o menu (a tecla pode ser trocada nele):

| Página | Conteúdo |
|---|---|
| Status | Conectado ou não, ping, versão do SA-MP, microfone e teclas de falar. |
| Som | Volume, som 3D (realista, estéreo simples ou desligado), eco do ambiente, queda com a distância, direção pelo personagem ou pela câmera, inverter canais e bipe da tecla de falar. |
| Microfone | Dispositivo, ganho, filtro de ruído, medidor, teste e ativação por voz (se o servidor permitir). |
| Jogadores | Volume e silenciar por jogador. |
| Interface | Idioma (automático, English ou Português), tamanho, posição do ícone do microfone (ou arraste o ícone com o menu aberto) e tecla do menu. |
| Sobre | Versão, link do projeto e créditos. |

As opções ficam no `voicebridge.ini`, na pasta do GTA. O `voicebridge.log` registra a versão do samp.dll detectada e erros de áudio, sem IP nem porta.

## Segurança

- Cada jogador recebe uma chave aleatória; pacotes sem chave válida ou com CRC errado são descartados.
- Depois do primeiro pacote válido, a sessão fica presa àquele IP (o IP da conexão do jogo sempre tem prioridade): uma chave descoberta não serve em outra máquina.
- Um IP que manda 30 pacotes inválidos em 10 s (varredura de porta ou tentativa de adivinhar chaves) é bloqueado por 5 minutos, com aviso no log.
- Cada jogador tem um limite de pacotes por segundo.
- O client nunca mostra IP, porta ou tipo de conexão; esses dados ficam só no log do servidor.

## Solução de problemas

| Sintoma | O que verificar |
|---|---|
| Jogadores com SampVoice surdos e mudos, aviso "none of its UDP packets reached port" com uma porta estranha (ex.: 51665) | A porta de voz estava ocupada quando o servidor iniciou e o plugin usou uma aleatória, que não está liberada. Veja o início do `voice-bridge.log`: ele diz qual porta estava ocupada. Libere essa porta (outro servidor/programa usando) ou defina `voice_port` com uma porta livre e liberada. |
| Ninguém conecta na voz | A porta UDP do log está liberada no firewall/painel? Com o túnel ativo (padrão), clients Voice Bridge funcionam mesmo assim. |
| Fica em "Conectando..." | Veja o log do servidor: ele avisa quando o UDP de um jogador nunca chega. |
| Vários servidores na mesma máquina | Defina `voice_port` diferente em cada um. |
| O mod não carrega | Falta um ASI Loader, ou o `sampvoice.asi` ainda está na pasta. Veja o `voicebridge.log`. |
| Jogador com SampVoice fala mas não escuta | Ele entrou no servidor por nome (`localhost` ou domínio). O client original do SampVoice só recebe voz quando o servidor é acessado por IP (ex.: `127.0.0.1:7777`). O client Voice Bridge funciona com nome. |
| Não escuta ninguém | Volume do menu (F11 > Som) e se o jogador está em um stream que você ouve. |

Diagnóstico em Pawn: `VB_GetTransport`, `VB_OnPlayerTransportChange`, `VB_HasMicrophone` e `VB_IsClientSoundMuted`.

O que o Voice Bridge corrige em relação ao SampVoice:

- Porta fixa em vez de aleatória.
- A recepção não para depois de um erro ICMP do Windows.
- Funciona ao conectar por domínio.
- Funciona atrás de proxy/anti-DDoS.
- Acompanha mudanças de porta do NAT.
- Responde pelo IP certo em máquinas com vários IPs.
- Usa o túnel quando o UDP está bloqueado.
- Ordena as mensagens no open.mp.
- Libera streams e efeitos após GMX/unloadfs.

## Migrando do SampVoice

1. Troque o plugin `sampvoice` pelo `voice-bridge` (no open.mp, em `components/`).
2. Use o `sampvoice.inc` deste projeto ou mantenha o `.amx` atual. Nada mais muda.
3. Jogadores com `sampvoice.asi` continuam funcionando; quem usar `voice-bridge.asi` ganha túnel, som 3D, menu e os recursos novos.

Diferenças de comportamento (todas correções): streams dinâmicos respeitam o mundo virtual da entidade; streams e efeitos são apagados quando o script que os criou é descarregado; `maxplayers` 0 é tratado como infinito.

## Compilando

Requisitos: CMake 3.19+ e Visual Studio 2022 com C++ (Windows) ou GCC com multilib e Ninja (Linux).

**Windows**: [`build.ps1`](build.ps1) compila, roda os testes e junta tudo em `dist/`.

```powershell
.\build.ps1                         # servidor + client
.\build.ps1 client                  # só o voice-bridge.asi
.\build.ps1 server                  # só o plugin (com testes)
.\build.ps1 -Clean                  # do zero
.\build.ps1 -Package -Version 1.2.0 # gera os .zip de release
.\build.ps1 client -GameDir "C:\Games\GTA San Andreas"   # já copia para o jogo
.\build.ps1 server -ServerDir "D:\Samp\meu-servidor"     # já copia para o servidor
```

**Versão**: fica no arquivo [`VERSION`](VERSION). `-Version 1.2.0` grava a nova versão nele e no `voice-bridge.inc`, e ela vai para o plugin, o client (é o que `VB_GetClientBuild`/`VB_GetClientVersionString` retornam), o log, as propriedades do `.dll`/`.asi` e o nome dos `.zip`. Sem `-Version`, usa a que está no arquivo. Limites: MAJOR 0-6, MINOR 0-99, PATCH 0-99.

Se o PowerShell bloquear o script: `powershell -ExecutionPolicy Bypass -File .\build.ps1`.

Resultado:

```
dist/
  server/components/voice-bridge.dll   open.mp
  server/plugins/voice-bridge.dll      SA-MP
  server/include/*.inc
  client/voice-bridge.asi
```

**Linux** (servidor): `./build.sh` (opções `--clean`, `--no-tests`, `--version 1.2.0`). Gera `dist/server/`.

**Manual** com presets do CMake: `cmake --preset windows-server` e depois `cmake --build --preset windows-server` (também `windows-client` e `linux-server`).

## Como funciona

- O handshake do SampVoice (`0xDEADBEEF` anexado ao RPC 25) é mantido; o client Voice Bridge anexa também um hello próprio com sua versão.
- Controle pelo pacote RakNet 222 (formato do SampVoice + extensões a partir de `0x100`). Voz por UDP com o cabeçalho de 24 bytes do SampVoice (CRC32C) e Opus 48 kHz.
- No SA-MP o plugin intercepta o `RakServer` por padrão de bytes (0.3.7/0.3.DL) e lê posições chamando as natives do próprio servidor. No open.mp usa o SDK.
- O client usa a `bass.dll` do SA-MP e renderiza o som 3D a partir do personagem: diferença de tempo e de timbre entre os ouvidos, perda de agudos com a distância e eco do ambiente.

## Screenshots Client
<img width="1097" height="628" alt="Captura de tela 2026-10-07 031248" src="https://github.com/user-attachments/assets/4f9a444b-c85d-497b-a7bb-1f04bb038e54" />
<img width="1045" height="659" alt="Captura de tela 2026-10-07 031309" src="https://github.com/user-attachments/assets/ccd3cb0f-be8e-4a93-a6cc-2181c782b81f" />
<img width="1155" height="657" alt="Captura de tela 2026-10-07 031224" src="https://github.com/user-attachments/assets/bdfacfbf-5b63-4946-9f88-fb7d34d0e1ba" />
<img width="274" height="240" alt="Captura de tela 2026-10-07 031408" src="https://github.com/user-attachments/assets/f9272d9a-387b-4a6a-a1b2-02d94be5102a" />

## Créditos

- **Voice Bridge**: criado por [devbluen](https://github.com/devbluen). Código, issues e releases em [github.com/devbluen/omp-Voice-Bridge](https://github.com/devbluen/omp-Voice-Bridge).
- **MMV (Ramon)**: testes e ideias.
- **Claude (Anthropic)**: auxílio no desenvolvimento (servidor, client, protocolo, testes e documentação).
- **SampVoice**: protocolo e API originais de MOR (CyberMor), seguidos para manter a compatibilidade; porte para open.mp por AmyrAhmady (iAmir).
- Bibliotecas: [Opus](https://opus-codec.org), [Dear ImGui](https://github.com/ocornut/imgui), BASS (já vem com o SA-MP) e o SDK do open.mp.

Licença: MIT.
