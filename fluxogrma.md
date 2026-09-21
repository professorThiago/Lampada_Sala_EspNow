# Fluxograma do Sistema — Interruptor Inteligente ESP32

Diagramas do firmware (`main.cpp`), do boot ao regime permanente. Renderizam no
GitHub e no VS Code (extensão Markdown Preview Mermaid). Legenda das formas:
**oval** = início/fim · **retângulo** = ação · **losango** = decisão · **cilindro** = NVS.

---

## 1. Visão geral (modos de operação)

O sistema tem dois modos: **Estação** (operação normal) e **Portal AP** (configuração
de Wi-Fi). O boot decide para qual ir.

```mermaid
stateDiagram-v2
    [*] --> Boot
    Boot --> Estacao: conectou ou ja conectou antes
    Boot --> PortalAP: nunca conectou
    Estacao --> PortalAP: segurar BOOT 10s
    PortalAP --> Boot: salvar credenciais (reinicia)
    PortalAP --> Boot: 3 min sem cliente (reinicia)
    Estacao --> Estacao: botoes + web + ESP-NOW
    PortalAP --> PortalAP: configura Wi-Fi
```

---

## 2. Boot / `setup()`

Restaura as lâmpadas **antes** de qualquer rede, carrega a config do NVS e decide entre
operar como estação ou abrir o portal — com o **fallback** que ignora o BSSID/canal fixos
quando eles apontam para um AP inválido.

```mermaid
flowchart TD
    A(["Ligar / Reset"]) --> B["Serial + pinos"]
    B --> C["restaurarEstadoLampadas (NVS)"]
    C --> D["WiFi.mode STA<br/>carregarConfig (NVS)"]
    D --> E{"apReq pendente?<br/>(BOOT 10s anterior)"}
    E -->|sim| F["limparApReq"]
    F --> AP["iniciarModoAP"]
    AP --> ZP(["loop em modo portal"])
    E -->|nao| G["setHostname, onEvent<br/>setSleep(false), TX max"]
    G --> H["conectarWifi<br/>com BSSID/canal fixos"]
    H --> I{"conectou?"}
    I -->|sim| L["marcarWifiOk"]
    I -->|nao e usarBssid| J["conectarWifi SOLTO<br/>fallback de recuperacao"]
    I -->|nao e sem BSSID| M{"ja conectou antes?"}
    J --> K{"conectou?"}
    K -->|sim| L
    K -->|nao| M
    M -->|sim| N["segue OFFLINE<br/>reconecta em 2o plano"]
    M -->|nao| AP
    L --> O["configurarRotas"]
    N --> O
    O --> P(["loop normal"])
```

---

## 3. Loop principal (modo estação)

Laço cooperativo: OTA, subida de serviços sinalizada pelos eventos de Wi-Fi, HTTP, botões,
BOOT, drenagem da fila ESP-NOW, expiração do sincronismo, reconexão e heartbeat.

```mermaid
flowchart TD
    A(["loop()"]) --> B{"modoAP?"}
    B -->|sim| BP["loopPortal()"]
    BP --> A
    B -->|nao| C["ArduinoOTA.handle()"]
    C --> D{"OTA ativa?"}
    D -->|sim| A
    D -->|nao| E{"reinicio agendado?"}
    E -->|sim| ER(["ESP.restart()"])
    E -->|nao| F{"espnowPendente?"}
    F -->|sim| FE["iniciarEspNow()"]
    FE --> G
    F -->|nao| G{"servicosPendentes?"}
    G -->|sim| GE["iniciarServicos()<br/>mDNS / OTA / HTTP"]
    GE --> H
    G -->|nao| H["server.handleClient()"]
    H --> I["tratarBotoes()"]
    I --> J["verificarBotaoBoot()<br/>BOOT 10s -> AP"]
    J --> K["drena fila ESP-NOW<br/>processarMensagem()"]
    K --> L{"sync expirou?"}
    L -->|sim| LS["encerra sincronismo"]
    LS --> Mx
    L -->|nao| Mx["tratarWiFi()<br/>so reconecta, sem reboot"]
    Mx --> Nn{"hora do heartbeat?"}
    Nn -->|sim| NH["notificarEstado()<br/>+ diagnostico"]
    NH --> A
    Nn -->|nao| A
```

---

## 4. Botões e gestos (`tratarBotoes()`)

Uma leitura com debounce alimenta a decisão entre toque curto, pressão longa de um botão e
pressão longa dos dois. Flags de "consumido" impedem dois disparos no mesmo gesto.

```mermaid
flowchart TD
    A(["tratarBotoes()"]) --> B["atualizaBotao (debounce)<br/>botao 1 e botao 2"]
    B --> C{"os DOIS<br/>pressionados >= 1s?"}
    C -->|sim| CS["entrarEmSync()<br/>marca consumidos"]
    C -->|nao| D{"UM pressionado<br/>>= 1s?"}
    D -->|sim| DS["masterToggle()<br/>liga/desliga tudo (local+par)"]
    D -->|nao| E{"soltou antes de 1s<br/>e nao consumido?"}
    E -->|sim| ES["toggleLampada()<br/>inverte lampada local"]
    E -->|nao| F(["fim"])
    CS --> F
    DS --> F
    ES --> F
```

---

## 5. ESP-NOW — recepção

O callback (contexto da task Wi-Fi) só filtra por **grupo** e enfileira; o `loop()` deduplica
e aplica o efeito. Separar callback (leve) de processamento (no loop) evita trabalho pesado
no contexto da pilha Wi-Fi.

```mermaid
flowchart TD
    subgraph RX["Callback aoReceber() — task Wi-Fi"]
      A(["frame recebido"]) --> B{"tamanho ok?"}
      B -->|nao| X1["descarta"]
      B -->|sim| C{"grupo == cfg.grupo?"}
      C -->|nao| X2["descarta<br/>(isola outros pares)"]
      C -->|sim| D["enfileira (fila circular)"]
    end
    subgraph LP["loop() — processarMensagem()"]
      D --> F{"id repetido?<br/>dedup por origem"}
      F -->|sim| X3["ignora"]
      F -->|nao| G{"tipo?"}
      G -->|ESTADO| H["atualiza cache lampRemota"]
      G -->|ESPELHAR| I["escreve lampada<br/>entra em sync"]
      G -->|ENTRAR_SYNC| J["entra em sync"]
      G -->|MASTER_SET| K["escreve as 2 lampadas"]
    end
```

---

## 6. ESP-NOW — envio e eventos de Wi-Fi

Envio em rajada (idempotente + dedup) e os eventos de Wi-Fi que apenas levantam flags — o
trabalho pesado de (re)subir serviços roda no loop.

```mermaid
flowchart LR
    subgraph TX["enviarRobusto()"]
      A["carimba grupo, origem, id"] --> B["envia nRepeticoes vezes<br/>(burst, gap ms)"]
    end
    subgraph EV["onWiFiEvent()"]
      C{"evento Wi-Fi"} -->|STA_CONNECTED| D["espnowPendente = true"]
      C -->|STA_GOT_IP| E["servicosPendentes = true"]
    end
    D -.-> F["loop: iniciarEspNow()"]
    E -.-> H["loop: iniciarServicos()"]
```

---

## 7. Portal de configuração Wi-Fi (`loopPortal()`)

SoftAP + DNS captive servindo só a página de configuração. Sai por salvamento de
credenciais ou por timeout sem cliente (auto-recuperação).

```mermaid
flowchart TD
    A(["iniciarModoAP()"]) --> B["WiFi.mode AP_STA<br/>softAP + DNS captive"]
    B --> C["rotas do portal<br/>scanNetworks (async)"]
    C --> D(["loopPortal()"])
    D --> E["dnsServer + handleClient"]
    E -.->|scan| S1["GET /scan: lista redes"]
    E -.->|save| S2["POST /save: grava SSID/senha (NVS)<br/>agenda reinicio"]
    E --> F{"reinicio agendado?"}
    F -->|sim| G(["ESP.restart -> nova rede"])
    F -->|nao| H{"cliente conectado?"}
    H -->|sim| I["zera timeout"]
    I --> D
    H -->|nao| Jj{"3 min sem cliente?"}
    Jj -->|sim| K(["ESP.restart -> retenta STA"])
    Jj -->|nao| D
```

---

## 8. Persistência do estado das lâmpadas

Grava no NVS só quando muda; restaura no boot. É o que impede um reinício (energia, OTA,
Wi-Fi) de apagar as luzes.

```mermaid
flowchart LR
    subgraph W["Em operacao"]
      A["escreveLampada(i, estado)"] --> B{"estado mudou?"}
      B -->|sim| C["digitalWrite + grava NVS (chave lamp)"]
      B -->|nao| D["digitalWrite"]
    end
    subgraph R["No boot"]
      E(["setup()"]) --> Fr["restaurarEstadoLampadas()<br/>le NVS -> reaplica nos pinos"]
    end
```

---

## 9. Reconexão de Wi-Fi (sem reboot)

Um interruptor precisa funcionar offline: a queda de Wi-Fi **não** reinicia nem apaga as
luzes; apenas tenta reconectar, e os serviços de rede voltam sozinhos no evento `GOT_IP`.

```mermaid
flowchart TD
    A(["tratarWiFi() — periodico"]) --> B{"conectado?"}
    B -->|sim| C["marcarWifiOk"]
    B -->|nao| D["WiFi.reconnect()<br/>(lampadas/botoes/ESP-NOW seguem locais)"]
    D --> E["... aguarda proximo ciclo ..."]
    C --> F["evento GOT_IP<br/>-> servicosPendentes"]
    F --> G["loop: iniciarServicos()<br/>mDNS/OTA/HTTP de volta"]
```
