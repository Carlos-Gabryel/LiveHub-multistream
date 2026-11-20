# Protocolo LiveHub ⇄ LiveHub-multistream

O plugin registra no obs-websocket (v5) o vendor **`livehub-multistream`**. Os clientes chamam os requests com `CallVendorRequest`:

```json
{"requestType": "CallVendorRequest",
 "requestData": {"vendorName": "livehub-multistream", "requestType": "<request>", "requestData": {...}}}
```

A resposta chega em `responseData.responseData`. Para receber os eventos do vendor, o cliente precisa assinar `EventSubscription::Vendors` (`1 << 9` = 512).

**Versão da API: 4** (a fase 1 adicionou as saídas extras; a API 3 adiciona os campos do codificador próprio: `bitrate_proprio`, `encoder_proprio`, `modo_encoder` e `encoder_proprio`/`modo_encoder` no `saidas.status`).

**API 4** (painel do OBS): adiciona `saida.iniciar`/`saida.parar`, o campo `kbps` em cada saída do `saidas.status` e o bloco `principal`.

## Requests

### `saida.iniciar` / `saida.parar` (API 4)
Request: `{"id": "youtube"}`. Liga ou para **agora** só essa saída. Não muda o `ativo` gravado: `ativo` continua significando "liga sozinha quando a principal entrar no ar".
- Pode ser chamado com a principal fora do ar. Nesse caso a saída usa o **codificador próprio**, porque o encoder da principal não está rodando.
- Com a principal no ar, a saída segue o `modo_encoder`: `auto` usa o encoder da principal (ou o próprio, se a principal for multitrack); `proprio` usa sempre o próprio.
- Quando a principal para, **todas** as extras param, inclusive as ligadas pelo botão.
- Respostas: `{"ok": true}`, `{"ok": false, "erro": "id_desconhecido"}` ou `{"ok": false, "erro": "sem_encoder"}`.

### `status`
Resposta: `{"plugin_versao": "0.4.0", "obs_versao": "33.0.1", "api": 4}`

### `saidas.definir`
Substitui a lista inteira de saídas extras e grava em `obs_module_config_path("saidas.json")`. O plugin continua funcionando sem o LiveHub aberto.

Request:
```json
{"saidas": [
  {"id": "youtube", "nome": "YouTube", "servidor": "rtmps://a.rtmps.youtube.com:443/live2", "key": "xxxx", "ativo": true}
],
 "bitrate_proprio": 6000, "encoder_proprio": "", "modo_encoder": "auto"}
```
- `id`: `^[a-z0-9_-]{1,32}$`, único na lista. No máximo 8 saídas.
- `servidor`: começa com `rtmp://` ou `rtmps://`, até 300 caracteres. `key`: 1–300 caracteres, sem espaço nem quebra de linha. `nome`: até 40 caracteres.
- Dado inválido: nada muda e a resposta é `{"ok": false, "erro": "<motivo>"}`.
- Com a transmissão principal **no ar**:
  - saída nova ou que passou a ativa: inicia;
  - saída removida ou desativada: para;
  - saída que mudou servidor ou key: reinicia;
  - o resto continua sem interrupção.
- Resposta: `{"ok": true}`.

### `saidas.status`
Resposta (**nunca inclui a key**):
```json
{"ao_vivo": true, "multitrack": false,
 "saidas": [{"id": "youtube", "nome": "YouTube", "ativo": true, "estado": "ao_vivo", "erro": "",
             "bytes": 123456, "quadros_perdidos": 0, "desde": 1795100000}]}
```
- `ao_vivo`: a saída principal do OBS está transmitindo.
- `multitrack`: a saída principal não tem encoder de vídeo simples, como no Enhanced Broadcasting da Twitch. Nesse caso as extras usam o codificador próprio (ver abaixo).
- `estado`: `parado` | `conectando` | `ao_vivo` | `reconectando` | `erro`.
- `erro`: `""` | `key_invalida` | `falha_conexao` | `desconectado` | `sem_encoder` | `multitrack` | `outro`.
- `desde`: momento (unix, em segundos) em que a saída entrou `ao_vivo`. Fica `0` fora do ar.
- `kbps` (API 4): bitrate de saída atual, calculado pelo plugin com média dos últimos ~3 s. Fica `0` fora do ar.
- `principal` (API 4): `{"plataforma": "twitch"|"youtube"|"kick"|"", "ao_vivo": bool, "kbps": int, "quadros_perdidos": int}`. A plataforma é detectada pelo serviço de transmissão do OBS: `rtmp_common` pelo nome do serviço, ou `rtmp_custom` pelo host do servidor; vazio se não reconhecer. No `rtmp_custom`, hosts `global-contribute.live-video.net` contam como `kick`.

### `saida.ligar`
Request: `{"id": "kick", "ativo": false}`. Liga ou desliga uma saída e grava a mudança. Com a principal no ar, inicia ou para só essa saída.
Resposta: `{"ok": true}` ou `{"ok": false, "erro": "id_desconhecido"}`.

## Eventos
- `saida_estado`: `{"id": "kick", "estado": "reconectando", "erro": ""}`. É emitido a cada mudança de estado de uma saída.

## Enhanced Broadcasting (multitrack) — codificador próprio
Quando a saída principal é multitrack (Enhanced Broadcasting da Twitch), não existe um encoder simples para compartilhar. As saídas extras passam a usar um **par de encoders próprio do plugin**. É uma codificação a mais, e todas as extras compartilham essa mesma codificação.

- O request `saidas.definir` aceita os campos opcionais `"bitrate_proprio"` (kbps, inteiro de 1000 a 20000, padrão 6000) e `"encoder_proprio"` (`""` = automático, ou o id de um encoder de vídeo do OBS).
- No modo automático, a escolha do encoder segue esta ordem, usando o primeiro que existir: `obs_nvenc_h264_tex`, `jim_nvenc`, `h264_texture_amf`, `obs_qsv11_v2`, `obs_qsv11`, `obs_x264`.
- Configuração do encoder de vídeo: `rate_control` CBR, `bitrate` = bitrate_proprio, `keyint_sec` 2. Áudio: AAC (`ffmpeg_aac`, ou `CoreAudio_AAC` se existir) a 160 kbps, na trilha/mixer 0.
- Valores inválidos respondem `{"ok": false, "erro": "bitrate_invalido"}` ou `"encoder_invalido"`; ambos ficam salvos em `saidas.json`. Se uma extra já está no ar com encoders próprios, mudanças de bitrate/encoder só valem no próximo início.
- `"modo_encoder"` (opcional, padrão `"auto"`): `"auto"` compartilha os encoders da principal e só usa os próprios quando ela é multitrack; `"proprio"` usa sempre o par próprio, mesmo com encoder simples na principal (útil para um bitrate diferente nas extras). Valor inválido responde `"modo_encoder_invalido"`; se ausente, mantém o valor atual; fica salvo em `saidas.json`. O `saidas.status` ganha `"modo_encoder"`, e `multitrack` continua refletindo só a saída principal.
- Os encoders próprios são criados quando a primeira saída extra inicia no modo multitrack e liberados quando a última para.
- O `saidas.status` ganha `"encoder_proprio": "<id em uso ou \"\">"`. Com codificador próprio, as extras **não ficam em erro**: `multitrack: true` passa a ser só informativo. O erro `multitrack` só acontece se nenhum encoder puder ser criado (aí o erro é `sem_encoder`).

## Comportamento
- As saídas extras **iniciam** quando a principal entra no ar (`OBS_FRONTEND_EVENT_STREAMING_STARTED`) e **param** quando ela para (`STREAMING_STOPPING`).
- Cada saída extra é um `rtmp_output` com serviço `rtmp_custom`, ligado ao **mesmo encoder de vídeo e áudio** da saída principal (`obs_frontend_get_streaming_output`). Não há segunda codificação.
- Reconexão do próprio `rtmp_output`: 20 tentativas a cada 10 s. Uma saída cair não afeta as outras nem a principal.
- A key nunca vai para o log. O log mostra só o `id` e o host do servidor.
