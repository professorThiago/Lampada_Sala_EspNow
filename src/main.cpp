#include <Arduino.h>

/* ============================================================================
 *  INTERRUPTOR INTELIGENTE - ESP32 (esp32dev)
 *  ----------------------------------------------------------------------------
 *  2 lampadas + 2 pulsadores por modulo. Dois modulos identicos conversam
 *  por ESP-NOW (broadcast). Atualizacao OTA via biblioteca AtualizadorOTA.
 *
 *  ROBUSTEZ DESTA VERSAO (contra falhas intermitentes):
 *   - ENVIO EM RAJADA (burst): cada comando e enviado N_REPETICOES vezes.
 *     ESP-NOW broadcast NAO tem ACK nem retransmissao; um unico frame perdido
 *     = comando perdido. Repetir reduz a perda ao cubo (~10% -> ~0,1%).
 *   - DEDUPLICACAO: cada mensagem carrega (origem, id). As copias do burst
 *     compartilham a mesma id; o receptor age UMA vez e ignora as repetidas.
 *   - COMANDOS IDEMPOTENTES: os comandos enviam ESTADO ABSOLUTO (liga/desliga
 *     alvo), nao "inverte". Aplicar 2x ou 3x da no mesmo -> burst e seguro.
 *   - TX POWER no maximo (WIFI_POWER_19_5dBm) para alcance/margem de sinal.
 *   - ESTADO periodico (heartbeat) mantem o cache do outro modulo sempre
 *     ressincronizado (auto-curativo) alem de servir de diagnostico.
 *
 *  CORRECOES ANTERIORES MANTIDAS:
 *   - WiFi.setSleep(false): sem isso o radio dorme e descarta frames ESP-NOW.
 *   - IP FIXO dependente do ID_MODULO -> .11 (mod 1) / .12 (mod 2).
 *   - mDNS gerido pela AtualizadorOTA (sem MDNS.begin duplicado).
 *   - Tempos: pressao longa 2s, timeout de sync 7s.
 *
 *  DIAGNOSTICO (serial):
 *   - HEARTBEAT a cada 1s: "Canal:X RX:Y (+dY) TX_ok:Z TX_fail:W Sync:...".
 *       -> RX sobe nos dois     = radio OK.
 *       -> RX em 0, canais IGUAIS   = revisar HW/power save.
 *       -> RX em 0, canais DIFERENTES = multi-AP (modulos em canais distintos).
 *
 *  GESTOS:
 *   - Toque curto no pulsador N   -> inverte a lampada N do proprio modulo.
 *   - Segurar os DOIS por 2s      -> entra em SINCRONISMO (espelhamento).
 *   - Segurar UM pulsador por 2s  -> "master toggle" (liga/desliga geral).
 *
 *  Autor: Thiago  |  Biblioteca OTA: github.com/professorThiago/AtualizadorOTA
 * ========================================================================== */

#include <WiFi.h>
#include <esp_now.h>
#include <AtualizadorOTA.h>

/* ----------------------------- CONFIGURACAO ------------------------------- */
// !!! Troque o ID em cada placa: 1 na primeira, 2 na segunda.
#define ID_MODULO          2

#define WIFI_SSID          "SENAI IoT"
#define WIFI_SENHA         "info@IoT"
#define OTA_SENHA          "info@134"

// IP fixo dependente do ID: modulo 1 -> .11 ; modulo 2 -> .12
static IPAddress IP_LOCAL (172, 16, 7, ID_MODULO);
static IPAddress GATEWAY  (172, 16,   0,   1);
static IPAddress MASCARA  (255, 255, 248,   0);  // /21
static IPAddress DNS1     (172, 16,   0,   1);

// Pinos (esp32dev). Evitam flash (6-11) e GPIO12 (trava boot).
// #define PINO_LAMPADA_1     18
// #define PINO_LAMPADA_2     5
// #define PINO_BOTAO_1       19
// #define PINO_BOTAO_2       21

#define PINO_LAMPADA_1     13
#define PINO_LAMPADA_2     12
#define PINO_BOTAO_1       14
#define PINO_BOTAO_2       27

#define LAMPADA_ATIVO_ALTO true
#define BOTAO_PRESSIONADO  LOW

// Tempos (ms)
#define MS_DEBOUNCE        50
#define MS_PRESSAO_LONGA   2000
#define MS_TIMEOUT_SYNC    7000
#define MS_HEARTBEAT       1000

// Robustez de envio
#define N_REPETICOES       3      // copias por comando (burst)
#define MS_BURST_GAP       8      // intervalo entre copias do burst (ms)

/* ------------------------------ TIPOS / MSG ------------------------------- */
enum TipoMsg : uint8_t {
  MSG_ESTADO      = 1,
  MSG_ESPELHAR    = 2,
  MSG_ENTRAR_SYNC = 3,
  MSG_MASTER_SET  = 4
};

const char* nomeTipo(uint8_t t) {
  switch (t) {
    case MSG_ESTADO:      return "ESTADO";
    case MSG_ESPELHAR:    return "ESPELHAR";
    case MSG_ENTRAR_SYNC: return "ENTRAR_SYNC";
    case MSG_MASTER_SET:  return "MASTER_SET";
    default:              return "DESCONHECIDO";
  }
}

typedef struct __attribute__((packed)) {
  uint8_t origem;   // ID_MODULO do remetente (1 ou 2) -> para dedup
  uint8_t id;       // sequencia por remetente; iguais no burst -> dedup
  uint8_t tipo;
  uint8_t indice;   // 0 = lampada 1, 1 = lampada 2 (MSG_ESPELHAR)
  uint8_t estado;   // 0/1 estado alvo (MSG_ESPELHAR)
  uint8_t lamp1;    // estado completo (MSG_ESTADO / MSG_MASTER_SET)
  uint8_t lamp2;
} Mensagem;

/* ------------------------------- GLOBAIS ---------------------------------- */
AtualizadorOTA ota;

uint8_t enderecoBroadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

bool lampada[2]      = {false, false};
bool lampRemota[2]   = {false, false};

bool      modoSync            = false;
uint32_t  ultimaAtividadeSync = 0;

// Sequencia de saida e ultimo id visto por remetente (dedup do burst)
uint8_t   idSaida             = 0;
uint8_t   ultimoIdDe[3]       = {255, 255, 255};   // indices 1 e 2

// Contadores de diagnostico
volatile uint32_t rxTotal   = 0;
volatile uint32_t txOk      = 0;
volatile uint32_t txFail    = 0;
uint32_t          rxAnterior = 0;

struct Botao {
  uint8_t  pino;
  bool     pressionado;
  bool     leituraAnt;
  uint32_t tDebounce;
  uint32_t tPressionado;
  bool     acaoLongaFeita;
  bool     consumido;
};

Botao botao[2];

uint32_t tAmbos        = 0;
bool     syncDisparado = false;

// Ring buffer de recepcao (16 para absorver bursts sem estourar)
volatile Mensagem fila[16];
volatile uint8_t  filaInicio = 0;
volatile uint8_t  filaFim    = 0;

/* ----------------------------- LAMPADAS ----------------------------------- */
void escreveLampada(uint8_t i, bool ligada) {
  lampada[i] = ligada;
  bool nivel = LAMPADA_ATIVO_ALTO ? ligada : !ligada;
  digitalWrite(i == 0 ? PINO_LAMPADA_1 : PINO_LAMPADA_2, nivel ? HIGH : LOW);
  Serial.printf("[LAMP] Lampada %d -> %s\n", i + 1, ligada ? "ON" : "OFF");
}

/* ----------------------------- ESP-NOW ------------------------------------ */
// Envia a mesma mensagem em rajada (burst). Carimba origem + id UMA vez, de
// modo que as N copias compartilhem a id -> o receptor deduplica.
void enviarRobusto(Mensagem m, uint8_t repeticoes) {
  m.origem = ID_MODULO;
  m.id     = ++idSaida;
  for (uint8_t k = 0; k < repeticoes; k++) {
    esp_err_t r = esp_now_send(enderecoBroadcast, (const uint8_t*)&m, sizeof(m));
    if (r != ESP_OK)
      Serial.printf("[TX] Falha esp_now_send (%s, err=%d)\n", nomeTipo(m.tipo), r);
    if (k + 1 < repeticoes) delay(MS_BURST_GAP);
  }
  Serial.printf("[TX] %s enviado %dx (id=%u)\n", nomeTipo(m.tipo), repeticoes, m.id);
}

// Estado periodico: 1 copia basta (e auto-curativo, repete a cada heartbeat).
void notificarEstado() {
  Mensagem m = {};
  m.tipo  = MSG_ESTADO;
  m.lamp1 = lampada[0];
  m.lamp2 = lampada[1];
  enviarRobusto(m, 1);
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void aoEnviar(const wifi_tx_info_t *info, esp_now_send_status_t status) {
#else
void aoEnviar(const uint8_t *mac, esp_now_send_status_t status) {
#endif
  if (status == ESP_NOW_SEND_SUCCESS) txOk++;
  else                                txFail++;
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void aoReceber(const esp_now_recv_info_t *info, const uint8_t *dados, int tam) {
#else
void aoReceber(const uint8_t *mac, const uint8_t *dados, int tam) {
#endif
  if (tam < (int)sizeof(Mensagem)) return;
  rxTotal++;
  uint8_t prox = (filaFim + 1) % 16;
  if (prox == filaInicio) return;            // fila cheia -> descarta
  memcpy((void*)&fila[filaFim], dados, sizeof(Mensagem));
  filaFim = prox;
}

void processarMensagem(const Mensagem &m) {
  // Deduplicacao do burst: ignora copias com a mesma id do mesmo remetente.
  if (m.origem >= 1 && m.origem <= 2) {
    if (m.id == ultimoIdDe[m.origem]) return;   // duplicata -> descarta
    ultimoIdDe[m.origem] = m.id;
  }

  Serial.printf("[RX] %s de mod%d (id=%u idx=%d est=%d l1=%d l2=%d)\n",
                nomeTipo(m.tipo), m.origem, m.id, m.indice, m.estado, m.lamp1, m.lamp2);

  switch (m.tipo) {

    case MSG_ESTADO: = LAMPADA_ATIVO_ALTO ? ligada : !ligada;
      lampRemota[0] = m.lamp1;
      lampRemota[1] = m.lamp2;
      break;

    case MSG_ESPELHAR:
      if (m.indice < 2) escreveLampada(m.indice, m.estado);
      if (!modoSync) Serial.println("[SYNC] Entrando em sync (via ESPELHAR).");
      modoSync = true;
      ultimaAtividadeSync = millis();
      notificarEstado();
      break;

    case MSG_ENTRAR_SYNC:
      if (!modoSync) Serial.println("[SYNC] Entrando em sync (pedido do outro modulo).");
      modoSync = true;
      ultimaAtividadeSync = millis();
      break;

    case MSG_MASTER_SET:
      Serial.println("[MASTER] Aplicando master set recebido.");
      escreveLampada(0, m.lamp1);
      escreveLampada(1, m.lamp2);
      notificarEstado();
      break;
  }
}

/* ------------------------------- GESTOS ----------------------------------- */
void toggleLampada(uint8_t i) {
  Serial.printf("[GESTO] Toque curto no pulsador %d\n", i + 1);
  escreveLampada(i, !lampada[i]);
  notificarEstado();

  if (modoSync) {
    Serial.printf("[SYNC] Espelhando lampada %d no outro modulo.\n", i + 1);
    Mensagem m = {};
    m.tipo   = MSG_ESPELHAR;
    m.indice = i;
    m.estado = lampada[i];
    enviarRobusto(m, N_REPETICOES);          // burst
    ultimaAtividadeSync = millis();
  }
}

void entrarEmSync() {
  Serial.println("[GESTO] Ambos pressionados 2s -> entrando em SYNC.");
  modoSync = true;
  ultimaAtividadeSync = millis();
  Mensagem m = {};
  m.tipo = MSG_ENTRAR_SYNC;
  enviarRobusto(m, N_REPETICOES);            // burst
}

void masterToggle() {
  bool algumAceso = lampada[0] || lampada[1] || lampRemota[0] || lampRemota[1];
  bool alvo = !algumAceso;

  Serial.printf("[GESTO] Master toggle -> %s tudo (local+remoto).\n",
                alvo ? "LIGAR" : "DESLIGAR");

  escreveLampada(0, alvo);
  escreveLampada(1, alvo);

  Mensagem m = {};
  m.tipo  = MSG_MASTER_SET;
  m.lamp1 = alvo;
  m.lamp2 = alvo;
  enviarRobusto(m, N_REPETICOES);            // burst
  notificarEstado();
}

/* ------------------------------- BOTOES ----------------------------------- */
uint8_t atualizaBotao(Botao &b) {
  bool leitura = (digitalRead(b.pino) == BOTAO_PRESSIONADO);
  uint8_t evento = 0;

  if (leitura != b.leituraAnt) {
    b.tDebounce = millis();
    b.leituraAnt = leitura;
  }

  if ((millis() - b.tDebounce) > MS_DEBOUNCE && leitura != b.pressionado) {
    b.pressionado = leitura;
    if (b.pressionado) {
      b.tPressionado   = millis();
      b.acaoLongaFeita = false;
      b.consumido      = false;
      evento = 1;
    } else {
      evento = 2;
    }
  }
  return evento;
}

void tratarBotoes() {
  uint8_t ev0 = atualizaBotao(botao[0]);
  uint8_t ev1 = atualizaBotao(botao[1]);

  bool ambos = botao[0].pressionado && botao[1].pressionado;

  if (ambos) {
    if (tAmbos == 0) { tAmbos = millis(); syncDisparado = false; }
  } else {
    tAmbos = 0;
  }

  // 1) SYNC: ambos segurados por 2s
  if (ambos && !syncDisparado && (millis() - tAmbos) >= MS_PRESSAO_LONGA) {
    entrarEmSync();
    syncDisparado = true;
    botao[0].consumido = botao[1].consumido = true;
    botao[0].acaoLongaFeita = botao[1].acaoLongaFeita = true;
  }

  // 2) MASTER TOGGLE: um pulsador segurado por 2s (e o outro solto)
  for (uint8_t i = 0; i < 2; i++) {
    uint8_t j = 1 - i;
    if (botao[i].pressionado && !botao[j].pressionado &&
        !botao[i].acaoLongaFeita && !botao[i].consumido &&
        (millis() - botao[i].tPressionado) >= MS_PRESSAO_LONGA) {
      masterToggle();
      botao[i].acaoLongaFeita = true;
      botao[i].consumido = true;
    }
  }

  // 3) TOQUE CURTO: ao soltar, se nao consumido e durou < 2s -> toggle
  for (uint8_t i = 0; i < 2; i++) {
    uint8_t ev = (i == 0) ? ev0 : ev1;
    if (ev == 2 && !botao[i].consumido &&
        (millis() - botao[i].tPressionado) < MS_PRESSAO_LONGA) {
      toggleLampada(i);
    }
  }
}

/* ------------------------------- SETUP ------------------------------------ */
void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println("=====================================================");
  Serial.printf ("  INTERRUPTOR INTELIGENTE ESP32 - MODULO %d\n", ID_MODULO);
  Serial.println("=====================================================");

  pinMode(PINO_LAMPADA_1, OUTPUT);
  pinMode(PINO_LAMPADA_2, OUTPUT);
  escreveLampada(0, false);
  escreveLampada(1, false);

  botao[0] = {PINO_BOTAO_1, false, false, 0, 0, false, false};
  botao[1] = {PINO_BOTAO_2, false, false, 0, 0, false, false};
  pinMode(PINO_BOTAO_1, INPUT_PULLUP);
  pinMode(PINO_BOTAO_2, INPUT_PULLUP);

  WiFi.mode(WIFI_STA);

  // CRITICO: sem modem sleep -> ESP-NOW RX confiavel.
  WiFi.setSleep(false);
  Serial.println("[WiFi] Modem sleep DESATIVADO.");

  // Potencia de TX no maximo -> mais margem contra perda de frames.
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  Serial.println("[WiFi] TX power no maximo (19.5 dBm).");

  if (!WiFi.config(IP_LOCAL, GATEWAY, MASCARA, DNS1)) {
    Serial.println("[WiFi] Falha ao aplicar IP fixo - seguindo com DHCP.");
  }

  WiFi.begin(WIFI_SSID, WIFI_SENHA);
  Serial.print("[WiFi] Conectando");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 15000) {
    delay(250); Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] CONECTADO.");
    Serial.print  ("[WiFi] MAC   : "); Serial.println(WiFi.macAddress());
    Serial.print  ("[WiFi] IP    : "); Serial.println(WiFi.localIP());
    Serial.print  ("[WiFi] CANAL : "); Serial.println(WiFi.channel());
    Serial.println("[WiFi] >>> O CANAL precisa ser IGUAL nos dois modulos! <<<");
  } else {
    Serial.println("[WiFi] NAO conectou - OTA/mDNS indisponivel.");
  }

  char hostname[24];
  snprintf(hostname, sizeof(hostname), "interruptor-%02d", ID_MODULO);

  ota.beginLocal(hostname, OTA_SENHA);
  Serial.printf("[OTA] Host: %s.local (mDNS gerido pela AtualizadorOTA)\n", hostname);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] FALHA ao iniciar!");
  } else {
    esp_now_register_recv_cb(aoReceber);
    esp_now_register_send_cb(aoEnviar);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, enderecoBroadcast, 6);
    peer.channel = 0;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK)
      Serial.println("[ESP-NOW] FALHA ao adicionar peer broadcast!");
    else
      Serial.println("[ESP-NOW] Pronto (broadcast, burst + dedup).");
  }

  Serial.println("[SYS] Sistema iniciado.");
  Serial.println("-----------------------------------------------------");
}

/* -------------------------------- LOOP ------------------------------------ */
void loop() {
  ota.atualizar();

  tratarBotoes();

  while (filaInicio != filaFim) {
    Mensagem m;
    memcpy(&m, (const void*)&fila[filaInicio], sizeof(Mensagem));
    filaInicio = (filaInicio + 1) % 16;
    processarMensagem(m);
  }

  if (modoSync && (millis() - ultimaAtividadeSync) > MS_TIMEOUT_SYNC) {
    modoSync = false;
    Serial.println("[SYNC] Sincronismo encerrado (inatividade).");
  }

  // ---------------------- HEARTBEAT / DIAGNOSTICO -------------------------
  static uint32_t tHb = 0;
  if (millis() - tHb >= MS_HEARTBEAT) {
    tHb = millis();

    notificarEstado();   // ressincroniza cache do outro + trafego para RX

    uint32_t rxAgora = rxTotal;
    uint32_t delta   = rxAgora - rxAnterior;
    rxAnterior       = rxAgora;

    Serial.printf("[HB] Canal:%d  RX:%lu (+%lu)  TX_ok:%lu  TX_fail:%lu  Sync:%s\n",
                  WiFi.channel(),
                  (unsigned long)rxAgora,
                  (unsigned long)delta,
                  (unsigned long)txOk,
                  (unsigned long)txFail,
                  modoSync ? "SIM" : "nao");
  }
}





