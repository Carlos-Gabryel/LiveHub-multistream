# Protocolo LiveHub ⇄ LiveHub-multistream

O plugin registra no obs-websocket (v5) o vendor **`livehub-multistream`**. Os clientes chamam os requests com `CallVendorRequest`:

```json
{"requestType": "CallVendorRequest",
 "requestData": {"vendorName": "livehub-multistream", "requestType": "<request>", "requestData": {...}}}
```

A resposta chega em `responseData.responseData`. Para receber os eventos do vendor, o cliente precisa assinar `EventSubscription::Vendors` (`1 << 9` = 512).

**Versão da API: 6** (a fase 1 adicionou as saídas extras; a API 3 adiciona os campos do codificador próprio: `bitrate_proprio`, `encoder_proprio`, `modo_encoder` e `encoder_proprio`/`modo_encoder` no `saidas.status`).

**API 4** (painel do OBS): adiciona `saida.iniciar`/`saida.parar`, o campo `kbps` em cada saída do `saidas.status` e o bloco `principal`.

**API 5** (saídas personalizadas e formato): cada saída aceita `"formato": "horizontal" | "vertical"` (padrão `horizontal`), e ids livres como `custom1`…`custom5` para destinos personalizados (ex.: Restream). Enquanto o canvas vertical não existir, uma saída `vertical` não inicia e fica `estado: "erro"`, `erro: "vertical_indisponivel"`. O `saidas.status` devolve `formato` em cada saída.

**API 6** (canvas vertical): o plugin cria o canvas "LiveHub Vertical" (1080×1920) depois do `FINISHED_LOADING` do OBS e passa a atender os requests `vertical.*` (abaixo). Saídas `vertical` agora funcionam: usam um par de encoders próprio ligado ao vídeo do canvas vertical (ver "Canvas vertical"). `vertical_indisponivel` só acontece se o canvas não existir. O restante da API 5 não mudou.

## Requests

### `saida.iniciar` / `saida.parar` (API 4)
Request: `{"id": "youtube"}`. Liga ou para **agora** só essa saída. Não muda o `ativo` gravado: `ativo` continua significando "liga sozinha quando a principal entrar no ar".
- Pode ser chamado com a principal fora do ar. Nesse caso a saída usa o **codificador próprio**, porque o encoder da principal não está rodando.
- Com a principal no ar, a saída segue o `modo_encoder`: `auto` usa o encoder da principal (ou o próprio, se a principal for multitrack); `proprio` usa sempre o próprio.
- Quando a principal para, **todas** as extras param, inclusive as ligadas pelo botão.
- Respostas: `{"ok": true}`, `{"ok": false, "erro": "id_desconhecido"}` ou `{"ok": false, "erro": "sem_encoder"}`.

### `status`
Resposta: `{"plugin_versao": "0.6.0", "obs_versao": "33.0.1", "api": 6}`

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
- `erro`: `""` | `key_invalida` | `falha_conexao` | `desconectado` | `sem_encoder` | `multitrack` | `vertical_indisponivel` | `outro`.
- `desde`: momento (unix, em segundos) em que a saída entrou `ao_vivo`. Fica `0` fora do ar.
- `kbps` (API 4): bitrate de saída atual, calculado pelo plugin com média dos últimos ~3 s. Fica `0` fora do ar.
- `principal` (API 4): `{"plataforma": "twitch"|"youtube"|"kick"|"", "ao_vivo": bool, "kbps": int, "quadros_perdidos": int}`. A plataforma é detectada pelo serviço de transmissão do OBS: `rtmp_common` pelo nome do serviço, ou `rtmp_custom` pelo host do servidor; vazio se não reconhecer. No `rtmp_custom`, hosts `global-contribute.live-video.net` contam como `kick`.

### `saida.ligar`
Request: `{"id": "kick", "ativo": false}`. Liga ou desliga uma saída e grava a mudança. Com a principal no ar, inicia ou para só essa saída.
Resposta: `{"ok": true}` ou `{"ok": false, "erro": "id_desconhecido"}`.

## Canvas vertical (API 6)

- **Canvas:** `LiveHub Vertical`, base e saída 1080×1920. FPS, formato de cor e espaço de cor vêm do vídeo principal. Flags `ACTIVATE | SCENE_REF | EPHEMERAL`, **sem** `MIX_AUDIO`: o áudio das saídas verticais é só a mixagem principal (`obs_get_audio()`), então nada toca em dobro.
- **Persistência:** as cenas verticais ficam **dentro da coleção de cenas** do OBS, na chave `livehub_vertical` dos dados de módulos: `{"versao": 1, "cenas": [<obs_save_source>...], "ativa": "<cena>", "ligacoes": {"<cena horizontal>": "<cena vertical>"}}`. Cada coleção tem o seu vertical. Trocar de coleção desmonta o canvas (parando antes as saídas verticais) e remonta com os dados da nova. As fontes dos itens são as mesmas fontes globais da coleção (por uuid/nome), sem duplicar. Alterações pelos requests marcam a coleção como alterada e o OBS salva no ciclo normal.
- **Troca automática:** no `SCENE_CHANGED` (ou `TRANSITION_STOPPED`) do OBS, a cena **de programa** (no modo estúdio também) é procurada em `ligacoes`. Se houver ligação, a cena vertical vira o canal 0 do canvas (corte seco). Sem ligação, a vertical atual continua. `vertical.cena.ativar` troca na mão; a próxima mudança de cena horizontal ligada volta a trocar sozinha.
- **Saídas verticais:** uma saída com `"formato": "vertical"` usa **sempre** o par de encoders próprio vertical (mesma escolha automática de encoder, `bitrate_proprio` e `encoder_proprio` do par horizontal; áudio AAC 160 kbps), ligado a `obs_canvas_get_video` + `obs_get_audio()`. O par é criado quando a primeira saída vertical inicia e liberado quando a última para; todas as verticais o compartilham. Independente do `modo_encoder`. Elas iniciam com a principal (se `ativo`) ou manualmente (`saida.iniciar`), como as outras. Ao desmontar o canvas, as verticais param; quando o canvas volta, as `ativo` religam sozinhas se a principal está no ar.
- Nomes de cena vertical: 1 a 64 caracteres, sem caracteres de controle nem `<` `>`. Nomes são únicos dentro do canvas vertical e **não** aparecem na lista de cenas do OBS (nem colidem com as cenas horizontais). Limites: 32 cenas, 128 itens por cena.
- Todos os requests abaixo respondem `{"ok": true, ...}` ou `{"ok": false, "erro": "<motivo>"}`. Erros comuns: `canvas_indisponivel` (canvas não existe, exceto em `vertical.status`), `cena_inexistente`, `item_inexistente`, `parametro_invalido` (campo ausente, fora de faixa ou de tipo errado; nada é alterado).

### `vertical.status`
Request: `{}`. Resposta: `{"ok": true, "canvas": true, "largura": 1080, "altura": 1920, "fps": 60, "cena_ativa": "Jogo", "cenas": 3}`. Sem canvas: `canvas: false`, `fps: 0`, `cena_ativa: ""`, `cenas: 0`.

### `vertical.cenas.listar`
Request: `{}`. Resposta: `{"ok": true, "cenas": [{"nome": "Jogo", "itens": 4}]}`.

### `vertical.cenas.criar`
Request: `{"nome": "Jogo"}`. Resposta: `{"ok": true}`. Erros: `nome_invalido`, `nome_em_uso`, `limite_cenas`. Se não havia cena ativa, a nova vira a ativa.

### `vertical.cenas.remover`
Request: `{"nome": "Jogo"}`. Remove a cena e as ligações que apontavam para ela. Se era a ativa, a primeira cena restante (ou nenhuma) vira ativa. Erro: `cena_inexistente`.

### `vertical.cenas.renomear`
Request: `{"nome": "Jogo", "novo": "Gameplay"}`. As ligações acompanham o novo nome. Erros: `cena_inexistente`, `nome_invalido`, `nome_em_uso`.

### `vertical.cena.itens`
Request: `{"cena": "Jogo"}`. Resposta (itens de baixo para cima; `posicao` 0 = fundo):
```json
{"ok": true, "cena": "Jogo", "itens": [
  {"id": 1, "posicao": 0, "fonte": "Câmera", "tipo": "dshow_input", "visivel": true,
   "largura": 1920, "altura": 1080,
   "pos": {"x": 540.0, "y": 960.0}, "escala": {"x": 0.5, "y": 0.5}, "rot": 0.0, "alinhamento": 0,
   "bounds": {"tipo": "nenhum", "x": 0.0, "y": 0.0, "alinhamento": 0, "cortar": false},
   "crop": {"l": 0, "t": 0, "r": 0, "b": 0}}]}
```
- `largura`/`altura`: tamanho base da fonte (`obs_source_get_width/height`; 0 se a fonte ainda não tem quadro). `tipo`: id da fonte no OBS.
- `alinhamento` (e `bounds.alinhamento`): bits do OBS, esquerda 1, direita 2, topo 4, base 8 (0 = centro). Padrão de item novo: 5 (esquerda+topo).
- `bounds.tipo`: `nenhum` | `esticar` | `interno` | `externo` | `largura` | `altura` | `maximo`.
- `pos`/`bounds` em pixels do canvas (1080×1920); `escala` é multiplicador; `rot` em graus.

### `vertical.item.adicionar`
Request: `{"cena": "Jogo", "fonte": "Câmera"}`. Resposta: `{"ok": true, "id": 7}`. A fonte precisa existir na coleção (fonte global; é a mesma do horizontal, não é copiada). Erros: `fonte_inexistente`, `fonte_invalida` (grupos não são aceitos), `limite_itens`. O item entra no topo, em `pos` 0,0, escala 1.

### `vertical.item.transformar`
Request: `{"cena": "Jogo", "id": 7, "pos": {"x": 0, "y": 0}, "escala": {"x": 1, "y": 1}, "rot": 0, "bounds": {"tipo": "interno", "x": 1080, "y": 608, "alinhamento": 0, "cortar": false}, "crop": {"l": 0, "t": 0, "r": 0, "b": 0}, "alinhamento": 5}`. Todos os campos (e sub-campos) depois de `id` são opcionais; o que não vier fica como está. `alinhamento` 0 a 15; valores numéricos finitos até ±1.000.000. Resposta: `{"ok": true}`. Erros: `item_inexistente`, `parametro_invalido`.

### `vertical.item.visivel`
Request: `{"cena": "Jogo", "id": 7, "visivel": false}`. Resposta: `{"ok": true}`.

### `vertical.item.ordem`
Request: `{"cena": "Jogo", "id": 7, "posicao": 2}`. `posicao` 0 = fundo; é limitada ao último índice. Resposta: `{"ok": true, "posicao": 2}`.

### `vertical.item.remover`
Request: `{"cena": "Jogo", "id": 7}`. Resposta: `{"ok": true}`. Não mexe na fonte, só tira o item da cena vertical.

### `vertical.cena.ativar`
Request: `{"nome": "Jogo"}`. Coloca a cena no canal 0 do canvas (corte seco). Erro: `cena_inexistente`.

### `vertical.ligacoes.definir`
Request: `{"ligacoes": {"Cena Jogo": "Jogo", "Cena Papo": "Papo"}}`. Substitui o mapa inteiro (chave = nome da cena horizontal, qualquer texto até 256 caracteres; valor = cena vertical, que precisa existir). Depois de gravar, já aplica a ligação da cena de programa atual. Erros: `parametro_invalido` (valor que não é texto), `cena_inexistente`. Nada muda se der erro.

### `vertical.ligacoes.listar`
Request: `{}`. Resposta: `{"ok": true, "ligacoes": {"Cena Jogo": "Jogo"}}`.

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
