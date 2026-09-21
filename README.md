# Interruptor Inteligente ESP32 (ESP-NOW)

Firmware para um interruptor de parede inteligente baseado em ESP32. Cada **módulo**
controla **2 lâmpadas** e lê **2 pulsadores**. Dois módulos idênticos formam um **par**
e conversam diretamente por **ESP-NOW** (sem depender de servidor ou nuvem), de modo que
um pulsador em um cômodo pode espelhar/comandar as lâmpadas do outro módulo.

Além do controle físico, cada módulo sobe uma **interface web** (com login), guarda toda
a configuração em memória não-volátil (**Preferences/NVS**) e oferece um **portal de
configuração Wi-Fi (modo AP)** para o primeiro uso ou recuperação.

> Autor: **Thiago Augusto de Oliveira** — github.com/professorThiago

---

## Índice

1. [Recursos](#recursos)
2. [Estrutura de arquivos](#estrutura-de-arquivos)
3. [Hardware e ligação](#hardware-e-ligação)
4. [Compilação e gravação](#compilação-e-gravação)
5. [Primeiro uso](#primeiro-uso)
6. [Uso no dia a dia — gestos físicos](#uso-no-dia-a-dia--gestos-físicos)
7. [Interface web](#interface-web)
8. [Portal de configuração Wi-Fi (modo AP)](#portal-de-configuração-wi-fi-modo-ap)
9. [Configuração persistente](#configuração-persistente)
10. [Protocolo ESP-NOW](#protocolo-esp-now)
11. [Explicação do código](#explicação-do-código)
12. [Comportamento de rede e resiliência](#comportamento-de-rede-e-resiliência)
13. [Segurança](#segurança)
14. [Referência da API HTTP](#referência-da-api-http)
15. [Solução de problemas](#solução-de-problemas)
16. [Limitações conhecidas](#limitações-conhecidas)

---

## Recursos

- **Controle físico local**: toque curto liga/desliga cada lâmpada; pressão longa dispara
  ações gerais (ver [gestos](#uso-no-dia-a-dia--gestos-físicos)).
- **Pares por ESP-NOW**: dois módulos se espelham/comandam sem passar pelo roteador.
- **Isolamento de pares**: vários pares convivem na mesma rede/canal sem interferir, via
  um identificador de **grupo** carimbado em cada mensagem.
- **Modo sincronismo**: espelha os toques de um módulo no outro.
- **Master toggle**: liga/desliga tudo (local + par) com um gesto.
- **Interface web responsiva** com autenticação **HTTP Digest** (controle e configuração
  protegidos; visualização livre).
- **Configuração persistente** (Preferences/NVS): módulo, grupo, hostname, canal, BSSID,
  credenciais Wi-Fi e tempos — tudo editável pela web, sem recompilar.
- **Portal de configuração (AP)** com varredura de redes e captive portal, acionado por
  falha de credencial nova **ou** segurando o botão BOOT por 10 s.
- **Estado das lâmpadas persistente**: sobrevive a reinício, queda de energia e OTA.
- **OTA nativo** (ArduinoOTA) e **mDNS** (`<hostname>.local`) que se recupera sozinho após
  quedas de Wi-Fi.

---

## Estrutura de arquivos

| Arquivo          | Conteúdo                                                                 |
|------------------|--------------------------------------------------------------------------|
| `main.cpp`       | Firmware: rede, ESP-NOW, botões, HTTP, persistência, modo AP.            |
| `web_index.h`    | HTML da interface principal (servido em `/`).                            |
| `web_style.h`    | CSS da interface (servido em `/style.css`), reutilizado pelo portal.     |
| `web_script.h`   | JavaScript da interface (servido em `/app.js`).                          |
| `web_portal.h`   | Página do portal de configuração Wi-Fi (modo AP).                        |

A página web é servida a partir de constantes `PROGMEM` (flash, não RAM). HTML, CSS e JS
ficam em arquivos separados e são entregues em rotas distintas, para o navegador poder
cachear CSS/JS.

---

## Hardware e ligação

Plataforma: **ESP32** (env PlatformIO `esp32dev`).

| Função              | GPIO | Observação                                             |
|---------------------|------|--------------------------------------------------------|
| Lâmpada 1 (relé)    | 13   | `PINO_LAMPADA_1`                                        |
| Lâmpada 2 (relé)    | 12   | `PINO_LAMPADA_2`                                        |
| Pulsador 1          | 14   | `PINO_BOTAO_1`, `INPUT_PULLUP`, ativo em `LOW`         |
| Pulsador 2          | 27   | `PINO_BOTAO_2`, `INPUT_PULLUP`, ativo em `LOW`         |
| Botão BOOT          | 0    | `PINO_BOOT` (botão da placa), segurar 10 s → modo AP    |

- **Polaridade do relé**: `LAMPADA_ATIVO_ALTO true` significa que `HIGH` liga a lâmpada.
  Se seu módulo relé for acionado em nível baixo, mude para `false`.
- **Pulsadores**: ligados entre o GPIO e o GND (usam o pull-up interno). Pressionado = `LOW`.
- **GPIO0**: é um pino de *strapping*. Segurar o BOOT **durante a energização** entra no
  modo de gravação (comportamento de fábrica); o gesto de 10 s é lido **com o firmware já
  rodando**.

> ⚠️ Rede elétrica: o acionamento de lâmpadas em 127/220 V exige relé/optoacoplador
> adequado e isolação. Isto foge do escopo do firmware.

---

## Compilação e gravação

Todas as bibliotecas usadas fazem parte do core do ESP32 (Arduino), **sem dependências
externas** no `platformio.ini`:

`WiFi`, `esp_now`, `WebServer`, `ESPmDNS`, `ArduinoOTA`, `Preferences`, `DNSServer`.

`platformio.ini` mínimo:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200
```

Gravação inicial pela USB. Atualizações seguintes podem ser feitas por **OTA**: com o
módulo na rede, o hostname aparece na IDE (via mDNS/`enableArduino`) e o upload pede a
senha definida em `OTA_SENHA`.

---

## Primeiro uso

1. **Grave o mesmo binário** nas duas placas do par (não há mais `#define ID_MODULO`; a
   identidade é configurada pela web).
2. Cada placa nasce com um **hostname único** derivado do MAC (`interruptor-XXXXXX`), o que
   evita colisão de mDNS antes de você renomear.
3. **Wi-Fi**: se as credenciais padrão não conectarem, a placa sobe o
   [portal de configuração (AP)](#portal-de-configuração-wi-fi-modo-ap). Conecte-se a ela,
   escolha a rede e informe a senha.
4. **Identidade do par**: acesse `http://<hostname>.local` (ou o IP), abra
   **⚙ Configurações** e defina:
   - **Módulo**: `1` em uma placa, `2` na outra.
   - **Par / grupo**: o **mesmo número** nas duas (ex.: `5`). Use um grupo diferente para
     cada par que existir na rede.
   - **Hostname**: opcional, para nomes amigáveis (ex.: `interruptor-sala`).
5. Salve — o módulo reinicia e aplica. Repita para a segunda placa. Pares diferentes usam
   grupos diferentes (`5`, `6`, ...).

---

## Uso no dia a dia — gestos físicos

| Gesto                                   | Efeito                                                        |
|-----------------------------------------|--------------------------------------------------------------|
| Toque curto no pulsador N               | Inverte a lâmpada N **do próprio módulo**.                   |
| Segurar os **dois** pulsadores por ~1 s | Entra em **sincronismo** (espelhamento com o par).           |
| Segurar **um** pulsador por ~1 s        | **Master toggle**: liga/desliga tudo (local + par).         |
| Segurar o **BOOT** por 10 s             | Reinicia no **portal de configuração (AP)**.                |

- O tempo de pressão longa é `msPressaoLonga` (padrão 1000 ms), configurável.
- Em **sincronismo**, um toque acende/apaga a mesma lâmpada nos dois módulos. O modo se
  encerra sozinho após `msTimeoutSync` (padrão 7 s) de inatividade.
- No **master toggle**, se **qualquer** lâmpada (local ou remota) estiver acesa, tudo apaga;
  senão, tudo acende. O comando é absoluto (idempotente) e propaga ao par.

---

## Interface web

Servida na porta 80. Todas as rotas usam caminhos relativos, então funcionam por IP ou por
`<hostname>.local`.

A página mostra o estado das duas lâmpadas (com atualização a cada 1,5 s), botões
**Acender/Apagar tudo**, o interruptor de **sincronismo**, um painel recolhível de
**estatísticas de rede** e o painel **⚙ Configurações**.

**Autenticação (HTTP Digest).** A senha não trafega em texto claro. A separação é:

- **Livres (visualização)**: `/`, `/style.css`, `/app.js`, `/status`.
- **Protegidas (controle e configuração)**: `/lamp`, `/master`, `/sync`, `/config` (GET e POST).

O login (usuário `admin`, senha em `WEB_SENHA`) é pedido pelo navegador na **primeira ação
de controle** ou ao **abrir o painel de configuração** — não no carregamento da página. A
configuração é carregada sob demanda justamente para não pedir senha à toa.

---

## Portal de configuração Wi-Fi (modo AP)

Quando ativo, o módulo sobe um **SoftAP** e serve apenas a página de configuração de rede.

**Como entra no modo AP:**

- No boot, se **nunca** conectou com as credenciais atuais (flag `wifiOk` em NVS) — típico
  de primeira gravação ou credencial trocada que não conecta.
- A qualquer momento, segurando o **BOOT por 10 s**.

> Uma queda **transitória** do Wi-Fi (o módulo já conectou antes) **não** joga no portal:
> ele continua operando localmente e reconecta em segundo plano.

**Dados do AP:**

- SSID: `Interruptor-Cfg-XXXXXX` (sufixo do MAC).
- Senha: definida em `AP_SENHA` (padrão de fábrica; **troque em produção**).
- IP do portal: `192.168.4.1` (com **captive DNS**, a página costuma abrir sozinha).

**Fluxo:** conecte-se ao AP → a página lista as redes (`/scan`, varredura assíncrona) →
toque na rede desejada → informe a senha → **Salvar e conectar**. O módulo grava as
credenciais no NVS e reinicia para conectar na rede escolhida.

**Auto-recuperação:** se ninguém se conectar ao portal por 3 min (`MS_PORTAL_TIMEOUT`), o
módulo reinicia e tenta a rede de novo — assim uma queda transitória do AP não deixa a
placa presa no portal.

---

## Configuração persistente

Toda a configuração vive numa `struct Config` gravada como *blob* em NVS
(namespace `interr`, chave `cfg`). Um campo `magic` (`0xA53C0003`) valida a versão: se o
layout mudar, o `magic` muda e o módulo regrava os padrões automaticamente.

> ⚠️ **Migração**: quando o layout da `struct` muda (novo `magic`), a configuração salva é
> reinicializada com os padrões. Após uma atualização que mude o `magic`, reconfigure
> módulo/grupo/hostname.

**Campos de identidade e rede:**

| Campo        | Descrição                                                        | Padrão            |
|--------------|------------------------------------------------------------------|-------------------|
| `modulo`     | Identidade dentro do par (`1` ou `2`).                           | `1`               |
| `grupo`      | Id do par (`1..255`). Só conversam módulos do mesmo grupo.       | `1`               |
| `hostname`   | Nome mDNS/DHCP.                                                  | `interruptor-<MAC>` |
| `canal`      | Canal Wi-Fi (`0` = automático, segue o AP).                     | `0`               |
| `usarBssid`  | Prende a um AP específico pelo BSSID.                           | `false`           |
| `bssid`      | MAC do AP (se `usarBssid`).                                     | —                 |
| `wifiSsid`   | Rede Wi-Fi (reconfigurável pelo portal).                        | `WIFI_SSID`       |
| `wifiSenha`  | Senha Wi-Fi.                                                     | `WIFI_SENHA`      |

**Tempos configuráveis (ms):**

| Campo            | Descrição                                        | Padrão   |
|------------------|--------------------------------------------------|----------|
| `msDebounce`     | Debounce dos pulsadores.                          | `50`     |
| `msPressaoLonga` | Limiar de pressão longa (gestos).                 | `1000`   |
| `msTimeoutSync`  | Inatividade que encerra o sincronismo.            | `7000`   |
| `msHeartbeat`    | Intervalo do heartbeat/estado.                    | `500`    |
| `nRepeticoes`    | Cópias por comando (burst ESP-NOW).               | `3`      |
| `msBurstGap`     | Intervalo entre cópias do burst.                  | `8`      |
| `msWifiCheck`    | Intervalo de checagem/reconexão Wi-Fi.            | `5000`   |
| `msWifiReset`    | *(inerte nesta versão — ver abaixo)*              | `300000` |

> **Nota**: `msWifiReset` existia para reiniciar o módulo após muito tempo sem Wi-Fi. Foi
> **desativado**: um interruptor precisa seguir funcionando offline (ver
> [resiliência](#comportamento-de-rede-e-resiliência)). O campo continua na struct/UI, mas
> não tem efeito.

O **estado das lâmpadas** é persistido separadamente (chave NVS `lamp`, 2 bits) e restaurado
no boot, para que nenhum reinício apague as luzes.

---

## Protocolo ESP-NOW

Os módulos enviam para o endereço de **broadcast** (`FF:FF:FF:FF:FF:FF`); o filtro de
**grupo** decide quem processa. Estrutura da mensagem (empacotada, 8 bytes):

```c
typedef struct __attribute__((packed)) {
  uint8_t grupo;   // id do par -> RX ignora se != cfg.grupo (isola pares)
  uint8_t origem;  // módulo remetente (1 ou 2) -> deduplicação
  uint8_t id;      // sequência por remetente; iguais no burst -> dedup
  uint8_t tipo;    // ver enum abaixo
  uint8_t indice;  // 0 = lâmpada 1, 1 = lâmpada 2  (MSG_ESPELHAR)
  uint8_t estado;  // 0/1 estado alvo               (MSG_ESPELHAR)
  uint8_t lamp1;   // estado completo               (MSG_ESTADO / MSG_MASTER_SET)
  uint8_t lamp2;
} Mensagem;
```

**Tipos de mensagem:**

| Tipo             | Valor | Uso                                                          |
|------------------|-------|--------------------------------------------------------------|
| `MSG_ESTADO`     | 1     | Heartbeat: informa o estado das lâmpadas (atualiza o cache). |
| `MSG_ESPELHAR`   | 2     | Sincronismo: espelha uma lâmpada específica.                 |
| `MSG_ENTRAR_SYNC`| 3     | Pede ao par para entrar em sincronismo.                      |
| `MSG_MASTER_SET` | 4     | Define o estado das duas lâmpadas (master toggle).           |

**Garantias de robustez:**

- **Isolamento de pares** — o 1º byte (`grupo`) é conferido logo na recepção
  (`aoReceber`); frames de outros grupos são descartados antes de entrar na fila.
- **Envio em rajada (burst)** — cada comando é enviado `nRepeticoes` vezes (padrão 3), com
  intervalo `msBurstGap`, para tolerar perda de pacotes.
- **Deduplicação** — todas as cópias de um burst compartilham a mesma `id`; o receptor
  guarda o último `id` por `origem` e ignora repetições.
- **Comandos idempotentes** — os comandos carregam estado **absoluto** (liga/desliga), não
  "inverte", então uma cópia extra nunca produz efeito colateral.
- **Heartbeat** — a cada `msHeartbeat` (500 ms) um `MSG_ESTADO` ressincroniza o cache
  `lampRemota[]` do outro módulo.

---

## Explicação do código

Visão geral do fluxo em `main.cpp`.

### `setup()`

1. Configura pinos e **restaura o estado das lâmpadas** do NVS (`restaurarEstadoLampadas`)
   — antes de qualquer rede, para não "piscar" apagado.
2. Entra em `WIFI_STA`, carrega a configuração (`carregarConfig`).
3. Se houver pedido de portal pendente (`apReqPendente`, setado pelo BOOT de 10 s), sobe o
   [modo AP](#portal-de-configuração-wi-fi-modo-ap) e retorna.
4. Registra o handler de eventos Wi-Fi, desliga o *modem sleep* (crítico para o ESP-NOW),
   fixa a potência de TX e chama `WiFi.begin()` com as credenciais do NVS.
5. Ao terminar a tentativa de conexão:
   - **Conectou** → marca `wifiOk` e segue.
   - **Não conectou, mas já conectou antes** → segue operando (queda transitória).
   - **Nunca conectou** → abre o portal AP (setup inicial).

### `loop()`

- Em **modo AP**, delega tudo a `loopPortal()` (DNS captive + servidor do portal) e retorna.
- Em operação normal: trata OTA, sobe serviços pendentes (mDNS/OTA/HTTP e ESP-NOW,
  sinalizados por flags dos eventos Wi-Fi), atende HTTP, lê botões, monitora o BOOT (10 s),
  drena a fila de mensagens ESP-NOW, expira o sincronismo, cuida da reconexão Wi-Fi e emite
  o heartbeat.

### Eventos Wi-Fi e (re)início de serviços

O handler `onWiFiEvent` só levanta flags:

- `STA_CONNECTED` (associou, canal definido) → `espnowPendente` → `iniciarEspNow()`.
- `STA_GOT_IP` (recebeu IP) → `servicosPendentes` → `iniciarServicos()` (ArduinoOTA uma vez,
  **mDNS recriado a cada reconexão**, HTTP reaberto).

Separar ESP-NOW (depende só do canal/associação) de mDNS/HTTP (dependem de IP) é o que
garante que o par volte a conversar mesmo antes do DHCP entregar o IP, e que o `.local`
"ressuscite" sozinho após uma queda.

### Botões (máquina de estados)

`atualizaBotao()` faz o **debounce** e devolve eventos de "pressionou"/"soltou".
`tratarBotoes()` combina isso para distinguir: toque curto (soltou antes do limiar), pressão
longa de um botão (master), e pressão longa dos dois (sincronismo), usando flags
`consumido`/`acaoLongaFeita` para não disparar duas ações no mesmo gesto.

### Recepção ESP-NOW (produtor/consumidor)

`aoReceber()` roda no contexto da *task* Wi-Fi: filtra por grupo e copia a mensagem para uma
**fila circular** (`fila[FILA_TAM]`). O `loop()` (consumidor) drena a fila e chama
`processarMensagem()`, que deduplica por `(origem, id)` e aplica o efeito conforme o tipo.
Manter o processamento fora do callback evita trabalho pesado no contexto da pilha Wi-Fi.

### Persistência do estado das lâmpadas

`escreveLampada()` grava no NVS **apenas quando o estado muda** (`salvarEstadoLampadas`,
com cache para evitar escritas repetidas). No boot, `restaurarEstadoLampadas()` relê e
reaplica. Resultado: qualquer reinício (energia, OTA, watchdog, entrada no AP) preserva as
luzes.

---

## Comportamento de rede e resiliência

- **Sem reboot por queda de Wi-Fi.** `tratarWiFi()` apenas tenta reconectar em segundo
  plano. Botões, ESP-NOW local e estado das lâmpadas seguem funcionando offline; mDNS/OTA/HTTP
  voltam sozinhos no evento `GOT_IP`.
- **Distinção transitório × credencial nova.** A flag `wifiOk` evita que uma queda
  temporária do AP jogue um módulo já instalado no portal de configuração.
- **`WiFi.setSleep(false)`** — sem isso o rádio dorme e descarta frames ESP-NOW.
- **Potência de TX no máximo** (`WIFI_POWER_19_5dBm`).
- **mDNS controlado pelo sketch** — encerrado e recriado a cada reconexão (não sobrevive à
  queda do Wi-Fi sozinho).
- **Estado das lâmpadas persistente** — reinícios não apagam as luzes.

---

## Segurança

- **Interface web**: HTTP **Digest** (a senha não trafega em claro). Protege controle e
  configuração; visualização é aberta.
- **Portal AP**: gateado pela senha do próprio AP (`AP_SENHA`).
- **OTA**: protegido por `OTA_SENHA`.
- **Limite honesto**: o Digest protege a *senha*, mas **não criptografa o tráfego** — os
  comandos e a configuração ainda são HTTP puro. Para uma LAN confiável é um mínimo
  razoável; para produção, o caminho é TLS e/ou *flash encryption* + *secure boot*.

> 🔐 **Credenciais no código.** As senhas (Wi-Fi, OTA, web, AP) ficam em `#define` no topo
> do `main.cpp`. **Não versione os valores reais em repositório público** — mantenha
> placeholders no repositório e preencha ao gravar, e troque a `AP_SENHA` e a `WEB_SENHA`
> padrão. Qualquer senha embutida aparece num dump de flash; se o modelo de ameaça exigir,
> use *flash encryption*.

---

## Referência da API HTTP

Base: `http://<hostname>.local/` ou `http://<ip>/`. 🔒 = exige autenticação.

| Rota          | Método | Parâmetros                         | Descrição                                  |
|---------------|--------|------------------------------------|--------------------------------------------|
| `/`           | GET    | —                                  | Interface (HTML).                          |
| `/style.css`  | GET    | —                                  | Folha de estilo.                           |
| `/app.js`     | GET    | —                                  | Script da interface.                       |
| `/status`     | GET    | —                                  | Estado atual (JSON).                       |
| `/lamp` 🔒    | GET    | `n=1\|2` `s=on\|off\|toggle`       | Comanda **uma** lâmpada local.             |
| `/master` 🔒  | GET    | `s=on\|off\|toggle`                | Comanda as duas (propaga ao par).          |
| `/sync` 🔒    | GET    | `s=on\|off`                        | Entra/sai do sincronismo.                  |
| `/config` 🔒  | GET    | —                                  | Configuração atual (JSON).                 |
| `/config` 🔒  | POST   | campos do formulário               | Salva a configuração e reinicia.           |

Exemplo de `/status`:

```json
{"modulo":1,"grupo":5,"hostname":"interruptor-sala",
 "lamp1":1,"lamp2":0,"remota1":0,"remota2":0,
 "sync":false,"canal":11,"ip":"...","bssid":"...","mac":"...","rssi":-58}
```

**Rotas exclusivas do modo AP:** `/` (portal), `/style.css`, `/scan` (varredura de redes,
`?force=1` para reiniciar a busca) e `/save` (POST `ssid`,`senha` → grava e reinicia).

---

## Solução de problemas

- **`<hostname>.local` não resolve.** Verifique no serial se saiu `[mDNS] OK`. Depois de uma
  atualização que reseta a config, o hostname volta ao padrão `interruptor-<MAC>` — descubra
  o nome real com `avahi-browse -rt _http._tcp` (Linux) e reconfigure. Não use dois módulos
  com o mesmo hostname (colisão). Android costuma falhar com `.local`; teste do desktop.
- **O par parou de conversar.** Confirme o **mesmo grupo** e o **mesmo canal** nos dois
  módulos. Em rede com seleção automática de canal, prefira `canal = 0` (auto).
- **Não consigo rolar o painel de Configurações.** Corrigido no CSS (`body` usa `min-height`
  e `align-items:flex-start`); garanta que está com o `web_style.h` atualizado.
- **As lâmpadas apagam sozinhas depois de um tempo.** Corrigido: o firmware não reinicia
  mais por queda de Wi-Fi e persiste o estado das lâmpadas.
- **Caí no portal AP sem querer.** Acontece se as credenciais nunca conectaram, ou se o BOOT
  ficou pressionado ~10 s. Reconfigure a rede e salve.

---

## Limitações conhecidas

Débitos técnicos assumidos, aceitáveis para o cenário atual (poucos módulos em LAN confiável):

- **Servidor HTTP síncrono + polling.** O `WebServer` atende requisições em sequência no
  mesmo laço dos botões/ESP-NOW; muitos clientes simultâneos aumentam latência e disputa de
  ar. Para escalar, o caminho é SSE/WebSocket (ex.: `ESPAsyncWebServer`).
- **Fila circular ad-hoc.** O buffer `volatile` produtor/consumidor não usa barreira de
  memória; como produtor (callback) e consumidor (`loop`) podem rodar em cores diferentes,
  há risco teórico de ordenação. O ideal é uma `FreeRTOS queue` (`xQueueSend`/`xQueueReceive`).
  Na prática, mensagens idempotentes + heartbeat autocorrigem.
- **Canal fixo × ESP-NOW.** ESP-NOW e Wi-Fi dividem o rádio; fixar canal/BSSID pode quebrar
  o par se o AP mudar de canal. Preferir `canal = auto`.
- **Credenciais embutidas.** Senhas no código-fonte (ver [Segurança](#segurança)).

---

*Documentação gerada para o firmware do Interruptor Inteligente ESP32.*
