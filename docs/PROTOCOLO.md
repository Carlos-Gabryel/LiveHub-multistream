# Protocolo LiveHub ⇄ LiveHub-multistream

O plugin registra no obs-websocket (v5) o vendor **`livehub-multistream`**. Os clientes chamam os requests com `CallVendorRequest`:

```json
{"requestType": "CallVendorRequest",
 "requestData": {"vendorName": "livehub-multistream", "requestType": "<request>", "requestData": {...}}}
```

A resposta chega em `responseData.responseData`. Para receber os eventos do vendor, o cliente precisa assinar `EventSubscription::Vendors` (`1 << 9` = 512).

**Versão da API: 2** (a fase 1 adiciona as saídas extras).

## Requests

### `status`
Resposta: `{"plugin_versao": "0.2.0", "obs_versao": "33.0.1", "api": 2}`

### `saidas.definir`
Substitui a lista inteira de saídas extras e grava em `obs_module_config_path("saidas.json")`. O plugin continua funcionando sem o LiveHub aberto.

Request:
```json
{"saidas": [
  {"id": "youtube", "nome": "YouTube", "servidor": "rtmps://a.rtmps.youtube.com:443/live2", "key": "xxxx", "ativo": true}
]}
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
- `multitrack`: a saída principal não tem encoder de vídeo simples, como no Enhanced Broadcasting da Twitch. Nesse caso as saídas extras não iniciam e ficam com `erro: "multitrack"`.
- `estado`: `parado` | `conectando` | `ao_vivo` | `reconectando` | `erro`.
- `erro`: `""` | `key_invalida` | `falha_conexao` | `desconectado` | `sem_encoder` | `multitrack` | `outro`.
- `desde`: momento (unix, em segundos) em que a saída entrou `ao_vivo`. Fica `0` fora do ar.

### `saida.ligar`
Request: `{"id": "kick", "ativo": false}`. Liga ou desliga uma saída e grava a mudança. Com a principal no ar, inicia ou para só essa saída.
Resposta: `{"ok": true}` ou `{"ok": false, "erro": "id_desconhecido"}`.

## Eventos
- `saida_estado`: `{"id": "kick", "estado": "reconectando", "erro": ""}`. É emitido a cada mudança de estado de uma saída.

## Comportamento
- As saídas extras **iniciam** quando a principal entra no ar (`OBS_FRONTEND_EVENT_STREAMING_STARTED`) e **param** quando ela para (`STREAMING_STOPPING`).
- Cada saída extra é um `rtmp_output` com serviço `rtmp_custom`, ligado ao **mesmo encoder de vídeo e áudio** da saída principal (`obs_frontend_get_streaming_output`). Não há segunda codificação.
- Reconexão do próprio `rtmp_output`: 20 tentativas a cada 10 s. Uma saída cair não afeta as outras nem a principal.
- A key nunca vai para o log. O log mostra só o `id` e o host do servidor.
