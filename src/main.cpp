#include <Arduino.h>

/* ============================================================================
 *  INTERRUPTOR INTELIGENTE - ESP32 (esp32dev)
 *  ----------------------------------------------------------------------------
 *  2 lampadas + 2 pulsadores por modulo. Dois modulos identicos formam um PAR
 *  e conversam por ESP-NOW (broadcast). Atualizacao OTA via ArduinoOTA.
 *
 *  >>> NOVIDADES DESTA VERSAO <<<
 *   - CONFIGURACAO PELA WEB, PERSISTENTE (Preferences/NVS): modulo (1/2), grupo
 *     do par, hostname, canal WiFi, BSSID fixo (on/off + valor) e TODOS os
 *     tempos. Salvar reinicia o modulo para aplicar de forma limpa e atomica.
 *   - MESMO BINARIO PARA TODAS AS PLACAS: nao ha mais #define ID_MODULO. Cada
 *     placa nasce com um hostname unico derivado do MAC; voce configura modulo
 *     e grupo pela pagina. Reflashe as duas placas com este firmware.
 *   - ISOLAMENTO DE PARES: cada mensagem carrega o "grupo". Um modulo so aceita
 *     mensagens do proprio grupo, entao varios pares coexistem no mesmo canal
 *     sem interferir. Frames de outros grupos sao descartados ja na recepcao.
 *   - PAGINA WEB SEPARADA: HTML, CSS e JS em arquivos proprios (web_index.h,
 *     web_style.h, web_script.h), servidos em "/", "/style.css" e "/app.js".
 *
 *  ROBUSTEZ (mantida):
 *   - ENVIO EM RAJADA (burst): cada comando enviado nRepeticoes vezes.
 *   - DEDUPLICACAO: (origem, id); copias do burst compartilham a id.
 *   - COMANDOS IDEMPOTENTES: estado absoluto (liga/desliga), nao "inverte".
 *   - TX POWER no maximo; ESTADO periodico (heartbeat) ressincroniza o cache.
 *
 *  RESILIENCIA DE REDE (mantida):
 *   - Reconexao automatica; sem conexao por msWifiReset -> ESP.restart().
 *   - mDNS/OTA/HTTP e ESP-NOW recriados a cada (re)conexao via eventos WiFi.
 *   - WiFi.setSleep(false): sem isso o radio dorme e descarta frames ESP-NOW.
 *   - mDNS sob controle explicito do sketch (o .local volta sozinho apos queda).
 *
 *  GESTOS:
 *   - Toque curto no pulsador N   -> inverte a lampada N do proprio modulo.
 *   - Segurar os DOIS por 1s      -> entra em SINCRONISMO (espelhamento).
 *   - Segurar UM pulsador por 1s  -> "master toggle" (liga/desliga geral).
 *
 *  CONTROLE POR REDE (HTTP):
 *       GET  /status                      -> estado atual em JSON
 *       GET  /lamp?n=1|2&s=on|off|toggle  -> comanda 1 lampada local
 *       GET  /master?s=on|off|toggle      -> comanda as 2 (propaga p/ o par)
 *       GET  /sync?s=on|off               -> entra/sai de sincronismo
 *       GET  /config                      -> configuracao atual em JSON
 *       POST /config                      -> salva configuracao e reinicia
 *
 *  Autor: Thiago
 * ========================================================================== */

#include <WiFi.h>
#include <esp_now.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <DNSServer.h>

#include "web_index.h"   // WEB_INDEX_HTML  -> "/"
#include "web_style.h"   // WEB_STYLE_CSS   -> "/style.css"
#include "web_script.h"  // WEB_SCRIPT_JS   -> "/app.js"
#include "web_portal.h"  // WEB_PORTAL_HTML -> "/" no modo AP

/* ============================= CREDENCIAIS ================================ */
#define WIFI_SSID  "SENAI IoT"      // rede PADRAO de fabrica (reconfiguravel no portal AP)
#define WIFI_SENHA "info@IoT"
#define OTA_SENHA  "info@134"

// Acesso web protegido por Digest (senha nao trafega em texto claro).
// Protege controle (/lamp,/master,/sync) e configuracao (/config).
#define WEB_USER  "admin"
#define WEB_SENHA "senha"

/* ============================== HARDWARE =================================
 * Pinos e polaridade sao fixos (dependem da placa), nao vao para a config.   */
#define PINO_LAMPADA_1 13
#define PINO_LAMPADA_2 12
#define PINO_BOTAO_1   14
#define PINO_BOTAO_2   27
#define LAMPADA_ATIVO_ALTO true
#define BOTAO_PRESSIONADO  LOW
#define PINO_BOOT 0          // botao BOOT (GPIO0) -> segurar 10s entra em modo AP

/* ======================= CONSTANTES DE COMPILACAO ========================= */
#define HTTP_PORTA 80        // porta do servidor HTTP
#define FILA_TAM   16         // tamanho do buffer circular de RX (potencia de 2)

/* -------- Modo AP / portal de configuracao WiFi -------- */
#define AP_SENHA          "configurar"   // senha do SoftAP de configuracao (>= 8 chars)
#define MS_BOOT_AP        10000UL        // segurar BOOT por 10s -> modo AP
#define MS_BOOT_CONNECT   20000UL        // timeout de conexao no boot antes de cair no AP
#define MS_PORTAL_TIMEOUT 180000UL       // sem cliente por 3 min -> reinicia e tenta STA
#define NVS_CHAVE_APREQ   "apReq"        // flag NVS: pedir modo AP no proximo boot
#define NVS_CHAVE_LAMP    "lamp"         // estado das lampadas (persistente entre boots)
#define NVS_CHAVE_WIFIOK  "wifiOk"       // ja conectou alguma vez com estas credenciais

/* ===================== VALORES PADRAO DE CONFIGURACAO =====================
 * Usados no PRIMEIRO boot (NVS vazio) ou quando a config salva e invalida.   */
#define PADRAO_CANAL          0        // 0 = automatico (segue o AP)
#define PADRAO_MS_DEBONCE     50
#define PADRAO_MS_LONGA       1000
#define PADRAO_MS_TIMEOUT     7000
#define PADRAO_MS_HEARTBEAT   500
#define PADRAO_N_REPETICOES   3
#define PADRAO_MS_BURST_GAP   8
#define PADRAO_MS_WIFI_CHECK  5000
#define PADRAO_MS_WIFI_RESET  300000UL

/* Preferences (NVS) */
#define NVS_NAMESPACE "interr"
#define NVS_CHAVE_CFG "cfg"
#define CFG_MAGIC     0xA53C0003UL   // muda quando o layout do struct muda (+wifiSsid/senha)

/* ============================ CONFIGURACAO ===============================
 * Tudo que e persistido em NVS. Um "magic" identifica versao/validade.       */
struct Config
{
  uint32_t magic;          // CFG_MAGIC -> config valida e compativel
  uint8_t  modulo;         // 1 ou 2 (identidade dentro do par)
  uint8_t  grupo;          // 1..255 (id do par -> isola de outros pares)
  uint8_t  canal;          // 0=auto, 1..13
  bool     usarBssid;      // prender a um AP especifico?
  uint8_t  bssid[6];       // MAC do AP (se usarBssid)
  char     hostname[32];   // nome mDNS / DHCP
  char     wifiSsid[33];   // credenciais WiFi (reconfiguraveis pelo portal AP)
  char     wifiSenha[65];

  // Tempos (ms) — parametrizaveis pela pagina
  uint16_t msDebounce;
  uint16_t msPressaoLonga;
  uint16_t msTimeoutSync;
  uint16_t msHeartbeat;
  uint8_t  nRepeticoes;    // copias por comando (burst)
  uint16_t msBurstGap;
  uint32_t msWifiCheck;
  uint32_t msWifiReset;
};

/* ============================== TIPOS / MSG ==============================
 * "grupo" e o primeiro campo: filtramos por ele logo na recepcao.            */
enum TipoMsg : uint8_t
{
  MSG_ESTADO      = 1,
  MSG_ESPELHAR    = 2,
  MSG_ENTRAR_SYNC = 3,
  MSG_MASTER_SET  = 4
};

typedef struct __attribute__((packed))
{
  uint8_t grupo;   // id do par -> RX ignora se != cfg.grupo (isola pares)
  uint8_t origem;  // modulo remetente (1 ou 2) -> dedup
  uint8_t id;      // sequencia por remetente; iguais no burst -> dedup
  uint8_t tipo;
  uint8_t indice;  // 0=lampada 1, 1=lampada 2 (MSG_ESPELHAR)
  uint8_t estado;  // 0/1 estado alvo (MSG_ESPELHAR)
  uint8_t lamp1;   // estado completo (MSG_ESTADO / MSG_MASTER_SET)
  uint8_t lamp2;
} Mensagem;

struct Botao
{
  uint8_t  pino;
  bool     pressionado;
  bool     leituraAnt;
  uint32_t tDebounce;
  uint32_t tPressionado;
  bool     acaoLongaFeita;
  bool     consumido;
};

/* ============================== PROTOTIPOS ================================ */
// Config / NVS
void   configPadrao(Config &c);
void   carregarConfig();
bool   salvarConfig(const Config &c);
bool   parseBssid(const String &s, uint8_t out[6]);
String bssidParaStr(const uint8_t b[6]);

// Nucleo
const char *nomeTipo(uint8_t t);
void escreveLampada(uint8_t i, bool ligada);
void enviarRobusto(Mensagem m, uint8_t repeticoes);
void notificarEstado();
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void aoEnviar(const wifi_tx_info_t *info, esp_now_send_status_t status);
void aoReceber(const esp_now_recv_info_t *info, const uint8_t *dados, int tam);
#else
void aoEnviar(const uint8_t *mac, esp_now_send_status_t status);
void aoReceber(const uint8_t *mac, const uint8_t *dados, int tam);
#endif
void processarMensagem(const Mensagem &m);
void toggleLampada(uint8_t i);
void entrarEmSync();
void masterToggle();
uint8_t atualizaBotao(Botao &b);
void tratarBotoes();
void onWiFiEvent(WiFiEvent_t event);
void iniciarServicos();
void iniciarEspNow();
void tratarWiFi();
void comandarLampada(uint8_t i, bool estado);
void comandarMaster(bool alvo);

// Modo AP / portal de configuracao
bool   apReqPendente();
void   marcarApReq();
void   limparApReq();
bool   wifiJaConectou();
void   marcarWifiOk();
void   salvarEstadoLampadas();
void   restaurarEstadoLampadas();
void   verificarBotaoBoot();
void   iniciarModoAP();
void   loopPortal();
void   configurarRotasPortal();
String jsonEscape(const String &s);
void   httpPortalRaiz();
void   httpPortalScan();
void   httpPortalSave();
void   httpPortalCaptive();

// HTTP
bool exigirAuth();
int  lerEstado(const String &s);
void configurarRotas();
void httpRaiz();
void httpEstilo();
void httpScript();
void httpStatus();
void httpLamp();
void httpMaster();
void httpSync();
void httpGetConfig();
void httpPostConfig();
void httpNaoEncontrado();

/* =============================== GLOBAIS ================================= */
Config      cfg;                     // configuracao viva (carregada do NVS)
Preferences prefs;
WebServer   server(HTTP_PORTA);

uint8_t enderecoBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

bool lampada[2]    = {false, false};
bool lampRemota[2] = {false, false};

bool     modoSync = false;
uint32_t ultimaAtividadeSync = 0;

uint8_t idSaida = 0;
uint8_t ultimoIdDe[3] = {255, 255, 255};   // indexado por origem (1..2)

volatile uint32_t rxTotal = 0;
volatile uint32_t txOk    = 0;
volatile uint32_t txFail  = 0;
uint32_t rxAnterior = 0;

Botao botao[2];
uint32_t tAmbos = 0;
bool syncDisparado = false;

volatile Mensagem fila[FILA_TAM];
volatile uint8_t filaInicio = 0;
volatile uint8_t filaFim    = 0;

volatile bool servicosPendentes = false;
volatile bool espnowPendente    = false;
volatile bool otaAtiva          = false;

uint32_t tReiniciar = 0;   // != 0 -> ESP.restart() quando millis() alcancar (config salva)

DNSServer dnsServer;       // captive DNS (so no modo AP)
bool      modoAP = false;  // true -> rodando o portal de configuracao
uint32_t  portalInicio = 0;

/* ==========================================================================
 *  CONFIGURACAO / NVS
 * ========================================================================== */
void configPadrao(Config &c)
{
  memset(&c, 0, sizeof(c));
  c.magic     = CFG_MAGIC;
  c.modulo    = 1;
  c.grupo     = 1;
  c.canal     = PADRAO_CANAL;
  c.usarBssid = false;

  // Hostname unico por placa (ultimos 3 bytes do MAC) -> evita colisao mDNS
  // no primeiro boot, antes de o usuario renomear.
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(c.hostname, sizeof(c.hostname), "interruptor-%02X%02X%02X",
           mac[3], mac[4], mac[5]);

  strncpy(c.wifiSsid,  WIFI_SSID,  sizeof(c.wifiSsid)  - 1);
  strncpy(c.wifiSenha, WIFI_SENHA, sizeof(c.wifiSenha) - 1);

  c.msDebounce     = PADRAO_MS_DEBONCE;
  c.msPressaoLonga = PADRAO_MS_LONGA;
  c.msTimeoutSync  = PADRAO_MS_TIMEOUT;
  c.msHeartbeat    = PADRAO_MS_HEARTBEAT;
  c.nRepeticoes    = PADRAO_N_REPETICOES;
  c.msBurstGap     = PADRAO_MS_BURST_GAP;
  c.msWifiCheck    = PADRAO_MS_WIFI_CHECK;
  c.msWifiReset    = PADRAO_MS_WIFI_RESET;
}

void carregarConfig()
{
  prefs.begin(NVS_NAMESPACE, false);
  size_t lido = prefs.getBytes(NVS_CHAVE_CFG, &cfg, sizeof(cfg));

  if (lido != sizeof(cfg) || cfg.magic != CFG_MAGIC)
  {
    Serial.println("[CFG] NVS vazio/incompativel -> gravando padroes.");
    configPadrao(cfg);
    prefs.putBytes(NVS_CHAVE_CFG, &cfg, sizeof(cfg));
  }
  prefs.end();

  Serial.printf("[CFG] modulo=%d grupo=%d canal=%d host=%s bssid=%s(%s)\n",
                cfg.modulo, cfg.grupo, cfg.canal, cfg.hostname,
                cfg.usarBssid ? "on" : "off", bssidParaStr(cfg.bssid).c_str());
}

bool salvarConfig(const Config &c)
{
  prefs.begin(NVS_NAMESPACE, false);
  size_t n = prefs.putBytes(NVS_CHAVE_CFG, &c, sizeof(c));
  prefs.end();
  return n == sizeof(c);
}

// Flag "entrar no portal AP no proximo boot" (chave NVS separada do blob cfg).
bool apReqPendente()
{
  prefs.begin(NVS_NAMESPACE, true);
  bool r = prefs.getBool(NVS_CHAVE_APREQ, false);
  prefs.end();
  return r;
}
void marcarApReq()
{
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putBool(NVS_CHAVE_APREQ, true);
  prefs.end();
}
void limparApReq()
{
  prefs.begin(NVS_NAMESPACE, false);
  prefs.remove(NVS_CHAVE_APREQ);
  prefs.end();
}

// "Ja conectou alguma vez com estas credenciais?" -> distingue queda
// transitoria do AP (nao vai para o portal) de credencial nunca validada.
bool wifiJaConectou()
{
  prefs.begin(NVS_NAMESPACE, true);
  bool r = prefs.getBool(NVS_CHAVE_WIFIOK, false);
  prefs.end();
  return r;
}
void marcarWifiOk()
{
  static bool gravado = false;
  if (gravado) return;              // grava uma vez por boot
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putBool(NVS_CHAVE_WIFIOK, true);
  prefs.end();
  gravado = true;
}

bool parseBssid(const String &s, uint8_t out[6])
{
  int v[6];
  if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x",
             &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
    return false;
  for (int i = 0; i < 6; i++)
  {
    if (v[i] < 0 || v[i] > 255) return false;
    out[i] = (uint8_t)v[i];
  }
  return true;
}

String bssidParaStr(const uint8_t b[6])
{
  char s[18];
  snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X",
           b[0], b[1], b[2], b[3], b[4], b[5]);
  return String(s);
}

/* ==========================================================================
 *  SETUP
 * ========================================================================== */
void setup()
{
  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println("=====================================================");
  Serial.println("  INTERRUPTOR INTELIGENTE ESP32");
  Serial.println("=====================================================");

  pinMode(PINO_LAMPADA_1, OUTPUT);
  pinMode(PINO_LAMPADA_2, OUTPUT);
  restaurarEstadoLampadas();      // mantem o estado anterior apos qualquer reinicio

  botao[0] = {PINO_BOTAO_1, false, false, 0, 0, false, false};
  botao[1] = {PINO_BOTAO_2, false, false, 0, 0, false, false};
  pinMode(PINO_BOTAO_1, INPUT_PULLUP);
  pinMode(PINO_BOTAO_2, INPUT_PULLUP);
  pinMode(PINO_BOOT,    INPUT_PULLUP);   // BOOT/GPIO0 -> entrada no modo AP

  WiFi.mode(WIFI_STA);            // necessario antes de ler o MAC / conectar
  WiFi.setAutoReconnect(true);

  carregarConfig();               // <<< define modulo/grupo/hostname/canal/tempos/wifi

  // Portal de configuracao solicitado? (falha de conexao anterior ou BOOT 10s)
  if (apReqPendente())
  {
    limparApReq();                // limpa: um novo boot volta a tentar a estacao
    iniciarModoAP();
    return;                       // segue no loop() em modo portal
  }

  WiFi.setHostname(cfg.hostname);
  WiFi.onEvent(onWiFiEvent);      // dispara (re)inicio de servicos -> antes do begin

  WiFi.setSleep(false);           // CRITICO: sem modem sleep -> ESP-NOW confiavel
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  Serial.printf("[WiFi] Hostname: %s | modem sleep OFF | TX 19.5dBm\n", cfg.hostname);

  const uint8_t *bssid = cfg.usarBssid ? cfg.bssid : nullptr;
  WiFi.begin(cfg.wifiSsid, cfg.wifiSenha, cfg.canal, bssid);
  Serial.printf("[WiFi] Conectando a \"%s\" (canal:%d, bssid fixo:%s)",
                cfg.wifiSsid, cfg.canal, cfg.usarBssid ? bssidParaStr(cfg.bssid).c_str() : "nao");

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < MS_BOOT_CONNECT)
  {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    marcarWifiOk();
    Serial.printf("[WiFi] CONECTADO. IP=%s  MAC=%s  BSSID=%s  CANAL=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.macAddress().c_str(),
                  WiFi.BSSIDstr().c_str(), WiFi.channel());
    Serial.println("[WiFi] >>> O CANAL precisa ser IGUAL nos dois modulos do par! <<<");
  }
  else if (wifiJaConectou())
  {
    // Ja conectou antes -> provavel queda transitoria do AP. NAO abre o portal:
    // segue operando (lampadas/botoes/ESP-NOW locais) e reconecta em 2o plano
    // quando o AP voltar (via evento GOT_IP).
    Serial.println("[WiFi] Sem conexao no boot, mas ja conectou antes -> seguindo (retenta em 2o plano).");
  }
  else
  {
    // Nunca conectou com estas credenciais -> setup inicial pelo portal AP.
    Serial.println("[WiFi] Nunca conectou com estas credenciais -> abrindo portal (AP).");
    iniciarModoAP();
    return;
  }

  configurarRotas();              // callbacks HTTP persistem entre reconexoes

  Serial.println("[SYS] Sistema iniciado.");
  Serial.println("-----------------------------------------------------");
}

/* ==========================================================================
 *  LOOP
 * ========================================================================== */
void loop()
{
  if (modoAP)                     // portal de configuracao WiFi (SoftAP)
  {
    loopPortal();
    return;
  }

  ArduinoOTA.handle();

  if (otaAtiva)                   // durante upload OTA, so cuidamos disso
    return;

  // Reinicio agendado (apos salvar config) — dá tempo de a resposta HTTP sair.
  if (tReiniciar && millis() >= tReiniciar)
  {
    Serial.println("[SYS] Reiniciando para aplicar configuracao...");
    delay(50);
    ESP.restart();
  }

  if (espnowPendente)             // ESP-NOW sobe na ASSOCIACAO (independe de IP)
  {
    espnowPendente = false;
    iniciarEspNow();
  }

  if (servicosPendentes)          // mDNS/OTA/HTTP sobem quando ha IP
  {
    servicosPendentes = false;
    iniciarServicos();
  }

  server.handleClient();
  tratarBotoes();
  verificarBotaoBoot();           // segurar BOOT (GPIO0) 10s -> modo AP

  while (filaInicio != filaFim)
  {
    Mensagem m;
    memcpy(&m, (const void *)&fila[filaInicio], sizeof(Mensagem));
    filaInicio = (filaInicio + 1) % FILA_TAM;
    processarMensagem(m);
  }

  if (modoSync && (millis() - ultimaAtividadeSync) > cfg.msTimeoutSync)
  {
    modoSync = false;
    Serial.println("[SYNC] Sincronismo encerrado (inatividade).");
  }

  tratarWiFi();

  // ---------------------- HEARTBEAT / DIAGNOSTICO -------------------------
  static uint32_t tHb = 0;
  if (millis() - tHb >= cfg.msHeartbeat)
  {
    tHb = millis();
    notificarEstado();

    uint32_t rxAgora = rxTotal;
    uint32_t delta   = rxAgora - rxAnterior;
    rxAnterior = rxAgora;

    Serial.printf("[HB] G%d M%d Canal:%d RX:%lu(+%lu) TXok:%lu TXfail:%lu Sync:%s\n",
                  cfg.grupo, cfg.modulo, WiFi.channel(),
                  (unsigned long)rxAgora, (unsigned long)delta,
                  (unsigned long)txOk, (unsigned long)txFail,
                  modoSync ? "SIM" : "nao");
  }
}

/* ==========================================================================
 *  UTIL / LAMPADAS
 * ========================================================================== */
const char *nomeTipo(uint8_t t)
{
  switch (t)
  {
  case MSG_ESTADO:      return "ESTADO";
  case MSG_ESPELHAR:    return "ESPELHAR";
  case MSG_ENTRAR_SYNC: return "ENTRAR_SYNC";
  case MSG_MASTER_SET:  return "MASTER_SET";
  default:              return "DESCONHECIDO";
  }
}

void escreveLampada(uint8_t i, bool ligada)
{
  bool mudou = (lampada[i] != ligada);
  lampada[i] = ligada;
  bool nivel = LAMPADA_ATIVO_ALTO ? ligada : !ligada;
  digitalWrite(i == 0 ? PINO_LAMPADA_1 : PINO_LAMPADA_2, nivel ? HIGH : LOW);
  Serial.printf("[LAMP] Lampada %d -> %s\n", i + 1, ligada ? "ON" : "OFF");
  if (mudou)
    salvarEstadoLampadas();   // persiste -> sobrevive a reboot/queda de energia
}

// Persiste o estado das 2 lampadas em NVS (2 bits). Grava so quando muda.
void salvarEstadoLampadas()
{
  static uint8_t ultimo = 0xFF;
  uint8_t v = (lampada[0] ? 0x01 : 0) | (lampada[1] ? 0x02 : 0);
  if (v == ultimo) return;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putUChar(NVS_CHAVE_LAMP, v);
  prefs.end();
  ultimo = v;
}

// Restaura o estado salvo (chamado no boot, ANTES de qualquer rede) para que
// um reinicio nao apague as luzes.
void restaurarEstadoLampadas()
{
  prefs.begin(NVS_NAMESPACE, true);
  uint8_t v = prefs.getUChar(NVS_CHAVE_LAMP, 0);   // 0 = ambas apagadas (1o boot)
  prefs.end();
  escreveLampada(0, v & 0x01);
  escreveLampada(1, v & 0x02);
  Serial.printf("[LAMP] Estado restaurado do NVS: L1=%s L2=%s\n",
                (v & 0x01) ? "ON" : "OFF", (v & 0x02) ? "ON" : "OFF");
}

/* ==========================================================================
 *  ESP-NOW
 * ========================================================================== */
void enviarRobusto(Mensagem m, uint8_t repeticoes)
{
  m.grupo  = cfg.grupo;           // carimba o par (isolamento)
  m.origem = cfg.modulo;
  m.id     = ++idSaida;

  for (uint8_t k = 0; k < repeticoes; k++)
  {
    esp_err_t r = esp_now_send(enderecoBroadcast, (const uint8_t *)&m, sizeof(m));
    if (r != ESP_OK)
      Serial.printf("[TX] Falha esp_now_send (%s, err=%d)\n", nomeTipo(m.tipo), r);
    if (k + 1 < repeticoes)
      delay(cfg.msBurstGap);
  }
  Serial.printf("[TX] %s enviado %dx (id=%u)\n", nomeTipo(m.tipo), repeticoes, m.id);
}

void notificarEstado()
{
  Mensagem m = {};
  m.tipo  = MSG_ESTADO;
  m.lamp1 = lampada[0];
  m.lamp2 = lampada[1];
  enviarRobusto(m, 1);
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void aoEnviar(const wifi_tx_info_t *info, esp_now_send_status_t status)
{
#else
void aoEnviar(const uint8_t *mac, esp_now_send_status_t status)
{
#endif
  if (status == ESP_NOW_SEND_SUCCESS) txOk++;
  else                                txFail++;
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void aoReceber(const esp_now_recv_info_t *info, const uint8_t *dados, int tam)
{
#else
void aoReceber(const uint8_t *mac, const uint8_t *dados, int tam)
{
#endif
  if (tam < (int)sizeof(Mensagem))
    return;

  // ISOLAMENTO DE PARES: descarta frames de outros grupos ja aqui, para nao
  // ocupar a fila nem contar como RX. O "grupo" e o 1o byte da mensagem.
  if (dados[0] != cfg.grupo)
    return;

  rxTotal++;
  uint8_t prox = (filaFim + 1) % FILA_TAM;
  if (prox == filaInicio)         // fila cheia -> descarta
    return;
  memcpy((void *)&fila[filaFim], dados, sizeof(Mensagem));
  filaFim = prox;
}

void processarMensagem(const Mensagem &m)
{
  // Grupo ja foi filtrado em aoReceber(); aqui so cuidamos da deduplicacao.
  if (m.origem >= 1 && m.origem <= 2)
  {
    if (m.id == ultimoIdDe[m.origem])
      return;
    ultimoIdDe[m.origem] = m.id;
  }

  Serial.printf("[RX] %s de mod%d (id=%u idx=%d est=%d l1=%d l2=%d)\n",
                nomeTipo(m.tipo), m.origem, m.id, m.indice, m.estado, m.lamp1, m.lamp2);

  switch (m.tipo)
  {
  case MSG_ESTADO:
    lampRemota[0] = m.lamp1;
    lampRemota[1] = m.lamp2;
    break;

  case MSG_ESPELHAR:
    if (m.indice < 2)
      escreveLampada(m.indice, m.estado);
    if (!modoSync)
      Serial.println("[SYNC] Entrando em sync (via ESPELHAR).");
    modoSync = true;
    ultimaAtividadeSync = millis();
    notificarEstado();
    break;

  case MSG_ENTRAR_SYNC:
    if (!modoSync)
      Serial.println("[SYNC] Entrando em sync (pedido do outro modulo).");
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

/* ==========================================================================
 *  GESTOS
 * ========================================================================== */
void toggleLampada(uint8_t i)
{
  Serial.printf("[GESTO] Toque curto no pulsador %d\n", i + 1);
  escreveLampada(i, !lampada[i]);
  notificarEstado();

  if (modoSync)
  {
    Serial.printf("[SYNC] Espelhando lampada %d no outro modulo.\n", i + 1);
    Mensagem m = {};
    m.tipo   = MSG_ESPELHAR;
    m.indice = i;
    m.estado = lampada[i];
    enviarRobusto(m, cfg.nRepeticoes);
    ultimaAtividadeSync = millis();
  }
}

void entrarEmSync()
{
  Serial.println("[GESTO] Ambos pressionados -> entrando em SYNC.");
  modoSync = true;
  ultimaAtividadeSync = millis();
  Mensagem m = {};
  m.tipo = MSG_ENTRAR_SYNC;
  enviarRobusto(m, cfg.nRepeticoes);
}

void masterToggle()
{
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
  enviarRobusto(m, cfg.nRepeticoes);
  notificarEstado();
}

/* ==========================================================================
 *  BOTOES
 * ========================================================================== */
uint8_t atualizaBotao(Botao &b)
{
  bool leitura = (digitalRead(b.pino) == BOTAO_PRESSIONADO);
  uint8_t evento = 0;

  if (leitura != b.leituraAnt)
  {
    b.tDebounce = millis();
    b.leituraAnt = leitura;
  }

  if ((millis() - b.tDebounce) > cfg.msDebounce && leitura != b.pressionado)
  {
    b.pressionado = leitura;
    if (b.pressionado)
    {
      b.tPressionado   = millis();
      b.acaoLongaFeita = false;
      b.consumido      = false;
      evento = 1;                 // pressionou
    }
    else
    {
      evento = 2;                 // soltou
    }
  }
  return evento;
}

void tratarBotoes()
{
  uint8_t ev0 = atualizaBotao(botao[0]);
  uint8_t ev1 = atualizaBotao(botao[1]);

  bool ambos = botao[0].pressionado && botao[1].pressionado;

  if (ambos)
  {
    if (tAmbos == 0)
    {
      tAmbos = millis();
      syncDisparado = false;
    }
  }
  else
  {
    tAmbos = 0;
  }

  // Segurar os dois -> entra em sync
  if (ambos && !syncDisparado && (millis() - tAmbos) >= cfg.msPressaoLonga)
  {
    entrarEmSync();
    syncDisparado = true;
    botao[0].consumido = botao[1].consumido = true;
    botao[0].acaoLongaFeita = botao[1].acaoLongaFeita = true;
  }

  // Segurar UM -> master toggle
  for (uint8_t i = 0; i < 2; i++)
  {
    uint8_t j = 1 - i;
    if (botao[i].pressionado && !botao[j].pressionado &&
        !botao[i].acaoLongaFeita && !botao[i].consumido &&
        (millis() - botao[i].tPressionado) >= cfg.msPressaoLonga)
    {
      masterToggle();
      botao[i].acaoLongaFeita = true;
      botao[i].consumido = true;
    }
  }

  // Toque curto (soltou antes da pressao longa) -> inverte a lampada
  for (uint8_t i = 0; i < 2; i++)
  {
    uint8_t ev = (i == 0) ? ev0 : ev1;
    if (ev == 2 && !botao[i].consumido &&
        (millis() - botao[i].tPressionado) < cfg.msPressaoLonga)
    {
      toggleLampada(i);
    }
  }
}

/* ==========================================================================
 *  REDE / SERVICOS
 * ========================================================================== */
// Handler de eventos WiFi: apenas sinaliza flags; o trabalho roda no loop.
void onWiFiEvent(WiFiEvent_t event)
{
  if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED)
    espnowPendente = true;        // canal ja definido -> ESP-NOW pode subir
  else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
    servicosPendentes = true;     // ha IP -> mDNS/OTA/HTTP
}

// (Re)inicia ArduinoOTA (1x), mDNS (sempre) e HTTP a cada (re)conexao.
void iniciarServicos()
{
  // --- ArduinoOTA: configurado UMA vez (begin() e no-op depois) ---
  static bool otaConfigurada = false;
  if (!otaConfigurada)
  {
    ArduinoOTA.setHostname(cfg.hostname);
    ArduinoOTA.setPassword(OTA_SENHA);
    ArduinoOTA.setMdnsEnabled(false);   // nos controlamos o mDNS

    ArduinoOTA.onStart([]()
    {
      otaAtiva = true;
      server.stop();
      esp_now_deinit();
      Serial.println("[OTA] Upload iniciado -> web/ESP-NOW suspensos.");
    });
    ArduinoOTA.onProgress([](unsigned int enviado, unsigned int total)
    {
      Serial.printf("[OTA] %u%%\r", total ? (enviado * 100u / total) : 0u);
    });
    ArduinoOTA.onEnd([]()
    {
      Serial.println("\n[OTA] Concluido. Reiniciando...");
    });
    ArduinoOTA.onError([](ota_error_t erro)
    {
      otaAtiva = false;
      espnowPendente = true;
      servicosPendentes = true;
      Serial.printf("\n[OTA] Erro %u -> servicos restaurados.\n", erro);
    });

    ArduinoOTA.begin();
    otaConfigurada = true;
    Serial.println("[OTA] ArduinoOTA configurado (mDNS proprio desligado).");
  }

  // --- mDNS reiniciado a CADA (re)conexao (o responder nao sobrevive a queda) ---
  MDNS.end();
  if (MDNS.begin(cfg.hostname))
  {
    MDNS.setInstanceName(String("Interruptor G") + cfg.grupo + " M" + cfg.modulo);
    MDNS.enableArduino(3232, true);
    MDNS.addService("http", "tcp", HTTP_PORTA);
    MDNS.addServiceTxt("http", "tcp", "grupo",  String(cfg.grupo).c_str());
    MDNS.addServiceTxt("http", "tcp", "modulo", String(cfg.modulo).c_str());
    MDNS.addServiceTxt("http", "tcp", "path", "/");
    Serial.printf("[mDNS] OK -> %s.local (http porta %d)\n", cfg.hostname, HTTP_PORTA);
  }
  else
  {
    Serial.println("[mDNS] FALHA em MDNS.begin() -> nome .local NAO vai resolver.");
  }

  // --- Servidor HTTP: reabre o socket de escuta ---
  server.stop();
  server.begin();
  Serial.printf("[HTTP] Servidor (re)iniciado na porta %d.\n", HTTP_PORTA);
}

// (Re)inicia ESP-NOW. NAO depende de IP -> disparado na ASSOCIACAO.
void iniciarEspNow()
{
  esp_now_deinit();
  if (esp_now_init() != ESP_OK)
  {
    Serial.println("[ESP-NOW] FALHA ao (re)iniciar!");
    return;
  }
  esp_now_register_recv_cb(aoReceber);
  esp_now_register_send_cb(aoEnviar);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, enderecoBroadcast, 6);
  peer.channel = 0;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK)
    Serial.println("[ESP-NOW] FALHA ao adicionar peer broadcast!");
  else
    Serial.printf("[ESP-NOW] (Re)iniciado (grupo %d, broadcast, burst+dedup, canal %d).\n",
                  cfg.grupo, WiFi.channel());
}

// Checagem periodica de conexao. NAO reinicia por falta de WiFi: um interruptor
// precisa seguir funcionando (botoes/ESP-NOW/estado das lampadas) mesmo offline.
// Apenas tenta reconectar; mDNS/OTA/HTTP voltam sozinhos no evento GOT_IP.
void tratarWiFi()
{
  static uint32_t tWifiCheck = 0;

  if (millis() - tWifiCheck < cfg.msWifiCheck)
    return;
  tWifiCheck = millis();

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("[WiFi] Sem conexao -> tentando reconectar (lampadas seguem locais).");
    WiFi.reconnect();
  }
  else
  {
    marcarWifiOk();
  }
}

/* ==========================================================================
 *  BOTAO BOOT / MODO AP (PORTAL DE CONFIGURACAO WiFi)
 * ========================================================================== */
// Segurar o BOOT (GPIO0) por MS_BOOT_AP -> agenda modo AP e reinicia.
void verificarBotaoBoot()
{
  static uint32_t tBoot = 0;
  static bool avisou = false;

  if (digitalRead(PINO_BOOT) == LOW)     // BOOT pressionado (ativo em LOW)
  {
    if (tBoot == 0) { tBoot = millis(); avisou = false; }

    if (!avisou && (millis() - tBoot) >= 2000)
    {
      Serial.println("[SYS] Segure o BOOT por 10s para entrar no modo de configuracao...");
      avisou = true;
    }
    if ((millis() - tBoot) >= MS_BOOT_AP)
    {
      Serial.println("[SYS] BOOT 10s -> reiniciando em modo AP.");
      marcarApReq();
      delay(100);
      ESP.restart();
    }
  }
  else
  {
    tBoot = 0;
  }
}

// Escapa aspas/barras/controle para embutir texto (ex.: SSID) em JSON.
String jsonEscape(const String &s)
{
  String o;
  o.reserve(s.length() + 4);
  for (size_t i = 0; i < s.length(); i++)
  {
    char c = s[i];
    if (c == '"' || c == '\\')      { o += '\\'; o += c; }
    else if (c == '\n')             { o += "\\n"; }
    else if ((uint8_t)c < 0x20)     { /* ignora controles */ }
    else                            { o += c; }
  }
  return o;
}

// Sobe SoftAP + DNS captive + servidor do portal. Sem ESP-NOW/OTA/mDNS aqui.
void iniciarModoAP()
{
  modoAP = true;
  WiFi.mode(WIFI_AP_STA);          // AP para o portal + STA para escanear redes
  WiFi.disconnect();               // garante estacao ociosa (so escaneia)

  char apSsid[32];
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(apSsid, sizeof(apSsid), "Interruptor-Cfg-%02X%02X%02X", mac[3], mac[4], mac[5]);
  WiFi.softAP(apSsid, AP_SENHA);

  IPAddress ip = WiFi.softAPIP();
  dnsServer.start(53, "*", ip);    // captive: qualquer dominio -> IP do portal
  configurarRotasPortal();
  server.begin();

  WiFi.scanNetworks(true);         // 1a varredura assincrona
  portalInicio = millis();

  Serial.println("=====================================================");
  Serial.println("  MODO CONFIGURACAO (SoftAP)");
  Serial.printf ("  SSID : %s\n", apSsid);
  Serial.printf ("  Senha: %s\n", AP_SENHA);
  Serial.printf ("  Abra : http://%s/\n", ip.toString().c_str());
  Serial.println("=====================================================");
}

void loopPortal()
{
  dnsServer.processNextRequest();
  server.handleClient();

  // Reinicio agendado apos salvar credenciais -> tenta a nova rede.
  if (tReiniciar && millis() >= tReiniciar)
  {
    Serial.println("[AP] Credenciais salvas -> reiniciando para conectar...");
    delay(50);
    ESP.restart();
  }

  // Havendo cliente no AP, zera o timeout (nao reinicia sob o usuario).
  if (WiFi.softAPgetStationNum() > 0)
    portalInicio = millis();

  // Sem ninguem por muito tempo -> reinicia e tenta a estacao de novo
  // (auto-recuperacao caso a queda de WiFi tenha sido transitoria).
  if (millis() - portalInicio > MS_PORTAL_TIMEOUT)
  {
    Serial.println("[AP] Timeout sem cliente -> reiniciando para tentar conectar.");
    ESP.restart();
  }
}

/* ==========================================================================
 *  COMANDOS (gestos / HTTP)
 * ========================================================================== */
void comandarLampada(uint8_t i, bool estado)
{
  escreveLampada(i, estado);
  notificarEstado();

  if (modoSync)
  {
    Serial.printf("[SYNC] Espelhando lampada %d no outro modulo.\n", i + 1);
    Mensagem m = {};
    m.tipo   = MSG_ESPELHAR;
    m.indice = i;
    m.estado = estado;
    enviarRobusto(m, cfg.nRepeticoes);
    ultimaAtividadeSync = millis();
  }
}

void comandarMaster(bool alvo)
{
  Serial.printf("[CMD] Master -> %s tudo (local+remoto).\n", alvo ? "LIGAR" : "DESLIGAR");
  escreveLampada(0, alvo);
  escreveLampada(1, alvo);

  Mensagem m = {};
  m.tipo  = MSG_MASTER_SET;
  m.lamp1 = alvo;
  m.lamp2 = alvo;
  enviarRobusto(m, cfg.nRepeticoes);
  notificarEstado();
}

/* ==========================================================================
 *  SERVIDOR HTTP
 * ========================================================================== */
int lerEstado(const String &s)
{
  if (s == "on"  || s == "1" || s == "true")  return 1;
  if (s == "off" || s == "0" || s == "false") return 0;
  if (s == "toggle" || s == "t")              return -1;
  return -2;
}

void configurarRotas()
{
  server.on("/",          httpRaiz);
  server.on("/style.css", httpEstilo);
  server.on("/app.js",    httpScript);
  server.on("/status",    httpStatus);
  server.on("/lamp",      httpLamp);
  server.on("/master",    httpMaster);
  server.on("/sync",      httpSync);
  server.on("/config",    HTTP_GET,  httpGetConfig);
  server.on("/config",    HTTP_POST, httpPostConfig);
  server.onNotFound(httpNaoEncontrado);
}

// Exige autenticacao Digest. Usar no topo das rotas de CONTROLE e CONFIG.
// As visualizacoes (/, /style.css, /app.js, /status) ficam livres.
// Retorna false ja tendo respondido 401 com o desafio -> o handler so faz "return".
bool exigirAuth()
{
  if (server.authenticate(WEB_USER, WEB_SENHA))
    return true;
  server.requestAuthentication(DIGEST_AUTH, "Interruptor",
                               "Autenticacao necessaria para controle/config.");
  return false;
}

// --- Assets estaticos (PROGMEM). CSS/JS com cache longo. ---
void httpRaiz()
{
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html; charset=utf-8", WEB_INDEX_HTML);
}

void httpEstilo()
{
  server.sendHeader("Cache-Control", "max-age=86400");
  server.send_P(200, "text/css; charset=utf-8", WEB_STYLE_CSS);
}

void httpScript()
{
  server.sendHeader("Cache-Control", "max-age=86400");
  server.send_P(200, "application/javascript; charset=utf-8", WEB_SCRIPT_JS);
}

// --- Estado runtime (consumido pela pagina a cada 1.5s) ---
void httpStatus()
{
  char buf[420];
  snprintf(buf, sizeof(buf),
           "{\"modulo\":%d,\"grupo\":%d,\"hostname\":\"%s\","
           "\"lamp1\":%d,\"lamp2\":%d,\"remota1\":%d,\"remota2\":%d,"
           "\"sync\":%s,\"canal\":%d,"
           "\"ip\":\"%s\",\"bssid\":\"%s\",\"mac\":\"%s\",\"rssi\":%d}",
           cfg.modulo, cfg.grupo, cfg.hostname,
           lampada[0], lampada[1], lampRemota[0], lampRemota[1],
           modoSync ? "true" : "false", WiFi.channel(),
           WiFi.localIP().toString().c_str(), WiFi.BSSIDstr().c_str(),
           WiFi.macAddress().c_str(), WiFi.RSSI());
  server.send(200, "application/json", buf);
}

void httpLamp()
{
  if (!exigirAuth()) return;
  if (!server.hasArg("n") || !server.hasArg("s"))
  {
    server.send(400, "text/plain", "uso: /lamp?n=1|2&s=on|off|toggle\n");
    return;
  }
  int n = server.arg("n").toInt();
  if (n < 1 || n > 2)
  {
    server.send(400, "text/plain", "parametro 'n' invalido (use 1 ou 2)\n");
    return;
  }
  uint8_t i = (uint8_t)(n - 1);

  int est = lerEstado(server.arg("s"));
  if (est == -2)
  {
    server.send(400, "text/plain", "parametro 's' invalido (on|off|toggle)\n");
    return;
  }

  bool alvo = (est == -1) ? !lampada[i] : (bool)est;
  Serial.printf("[HTTP] /lamp n=%d s=%s\n", n, server.arg("s").c_str());
  comandarLampada(i, alvo);
  httpStatus();
}

void httpMaster()
{
  if (!exigirAuth()) return;
  String s = server.hasArg("s") ? server.arg("s") : "toggle";
  int est = lerEstado(s);
  if (est == -2)
  {
    server.send(400, "text/plain", "parametro 's' invalido (on|off|toggle)\n");
    return;
  }

  bool alvo;
  if (est == -1)
  {
    bool algumAceso = lampada[0] || lampada[1] || lampRemota[0] || lampRemota[1];
    alvo = !algumAceso;
  }
  else
  {
    alvo = (bool)est;
  }

  Serial.printf("[HTTP] /master s=%s\n", s.c_str());
  comandarMaster(alvo);
  httpStatus();
}

void httpSync()
{
  if (!exigirAuth()) return;
  String s = server.hasArg("s") ? server.arg("s") : "on";
  int est = lerEstado(s);

  if (est == 1)
  {
    Serial.println("[HTTP] /sync on");
    entrarEmSync();
  }
  else if (est == 0)
  {
    Serial.println("[HTTP] /sync off");
    modoSync = false;
    Serial.println("[SYNC] Sincronismo encerrado (via HTTP).");
  }
  else
  {
    server.send(400, "text/plain", "parametro 's' invalido (on|off)\n");
    return;
  }
  httpStatus();
}

// --- Configuracao: GET devolve a atual; POST valida, salva e reinicia. ---
void httpGetConfig()
{
  if (!exigirAuth()) return;
  char buf[512];
  snprintf(buf, sizeof(buf),
           "{\"modulo\":%d,\"grupo\":%d,\"hostname\":\"%s\","
           "\"canal\":%d,\"usarBssid\":%s,\"bssid\":\"%s\","
           "\"msDebounce\":%u,\"msPressaoLonga\":%u,\"msTimeoutSync\":%u,"
           "\"msHeartbeat\":%u,\"nRepeticoes\":%u,\"msBurstGap\":%u,"
           "\"msWifiCheck\":%lu,\"msWifiReset\":%lu}",
           cfg.modulo, cfg.grupo, cfg.hostname,
           cfg.canal, cfg.usarBssid ? "true" : "false",
           bssidParaStr(cfg.bssid).c_str(),
           cfg.msDebounce, cfg.msPressaoLonga, cfg.msTimeoutSync,
           cfg.msHeartbeat, cfg.nRepeticoes, cfg.msBurstGap,
           (unsigned long)cfg.msWifiCheck, (unsigned long)cfg.msWifiReset);
  server.send(200, "application/json", buf);
}

// Helper: le um inteiro do POST, com clamp entre [lo,hi] e valor de fallback.
static long argInt(const char *nome, long lo, long hi, long fallback)
{
  if (!server.hasArg(nome)) return fallback;
  long v = server.arg(nome).toInt();
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  return v;
}

void httpPostConfig()
{
  if (!exigirAuth()) return;
  // Parte de uma copia da config atual e sobrescreve o que veio no POST.
  Config nova = cfg;
  nova.magic = CFG_MAGIC;

  nova.modulo = (uint8_t)argInt("modulo", 1, 2, cfg.modulo);
  nova.grupo  = (uint8_t)argInt("grupo",  1, 255, cfg.grupo);
  nova.canal  = (uint8_t)argInt("canal",  0, 13, cfg.canal);

  // Hostname: sanitiza (letras, numeros, hifen; 1..31 chars).
  if (server.hasArg("hostname"))
  {
    String h = server.arg("hostname");
    h.trim();
    bool ok = h.length() >= 1 && h.length() <= 31;
    for (size_t i = 0; ok && i < h.length(); i++)
    {
      char c = h[i];
      ok = isAlphaNumeric(c) || c == '-';
    }
    if (!ok)
    {
      server.send(400, "text/plain", "hostname invalido");
      return;
    }
    strncpy(nova.hostname, h.c_str(), sizeof(nova.hostname) - 1);
    nova.hostname[sizeof(nova.hostname) - 1] = '\0';
  }

  // BSSID fixo (opcional).
  nova.usarBssid = (argInt("usarBssid", 0, 1, cfg.usarBssid ? 1 : 0) == 1);
  if (nova.usarBssid)
  {
    if (!server.hasArg("bssid") || !parseBssid(server.arg("bssid"), nova.bssid))
    {
      server.send(400, "text/plain", "bssid invalido");
      return;
    }
  }

  // Tempos.
  nova.msDebounce     = (uint16_t)argInt("msDebounce",      10,   500,     cfg.msDebounce);
  nova.msPressaoLonga = (uint16_t)argInt("msPressaoLonga",  300,  5000,    cfg.msPressaoLonga);
  nova.msTimeoutSync  = (uint16_t)argInt("msTimeoutSync",   1000, 60000,   cfg.msTimeoutSync);
  nova.msHeartbeat    = (uint16_t)argInt("msHeartbeat",     100,  5000,    cfg.msHeartbeat);
  nova.nRepeticoes    = (uint8_t) argInt("nRepeticoes",     1,    10,      cfg.nRepeticoes);
  nova.msBurstGap     = (uint16_t)argInt("msBurstGap",      0,    100,     cfg.msBurstGap);
  nova.msWifiCheck    = (uint32_t)argInt("msWifiCheck",     1000, 60000,   cfg.msWifiCheck);
  nova.msWifiReset    = (uint32_t)argInt("msWifiReset",     30000,3600000, cfg.msWifiReset);

  if (!salvarConfig(nova))
  {
    server.send(500, "text/plain", "falha ao gravar NVS");
    return;
  }
  cfg = nova;

  Serial.println("[CFG] Nova configuracao salva. Reinicio agendado.");
  server.send(200, "application/json", "{\"ok\":true}");

  tReiniciar = millis() + 400;    // reinicia apos a resposta sair (aplica tudo)
}

void httpNaoEncontrado()
{
  server.send(404, "text/plain", "recurso nao encontrado\n");
}

/* ==========================================================================
 *  PORTAL DE CONFIGURACAO WiFi (rotas ativas SOMENTE no modo AP)
 * ========================================================================== */
void configurarRotasPortal()
{
  server.on("/",          httpPortalRaiz);
  server.on("/style.css", httpEstilo);        // reaproveita o CSS do app
  server.on("/scan",      httpPortalScan);
  server.on("/save",      HTTP_POST, httpPortalSave);
  server.onNotFound(httpPortalCaptive);       // captive -> tudo cai no portal
}

void httpPortalRaiz()
{
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html; charset=utf-8", WEB_PORTAL_HTML);
}

// Redireciona qualquer host/rota para o portal (dispara o "captive portal").
void httpPortalCaptive()
{
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  server.send(302, "text/plain", "");
}

// Lista de redes. ?force=1 reinicia a varredura. Enquanto varre: {status:scanning}.
void httpPortalScan()
{
  if (server.hasArg("force"))
  {
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    server.send(200, "application/json", "{\"status\":\"scanning\"}");
    return;
  }

  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED)         // nenhuma varredura iniciada ainda
  {
    WiFi.scanNetworks(true);
    server.send(200, "application/json", "{\"status\":\"scanning\"}");
    return;
  }
  if (n == WIFI_SCAN_RUNNING)
  {
    server.send(200, "application/json", "{\"status\":\"scanning\"}");
    return;
  }

  String json = "{\"status\":\"ok\",\"nets\":[";
  for (int i = 0; i < n; i++)
  {
    if (i) json += ",";
    json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",";
    json += "\"rssi\":"    + String(WiFi.RSSI(i)) + ",";
    json += "\"lock\":"    + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 0 : 1) + "}";
  }
  json += "]}";
  server.send(200, "application/json", json);
  // Mantem os resultados; a UI pede ?force=1 para atualizar.
}

// Salva as credenciais WiFi e agenda reinicio para conectar na rede escolhida.
void httpPortalSave()
{
  String ssid  = server.hasArg("ssid")  ? server.arg("ssid")  : "";
  String senha = server.hasArg("senha") ? server.arg("senha") : "";
  ssid.trim();

  if (ssid.length() < 1 || ssid.length() > 32)
  {
    server.send(400, "text/plain", "SSID invalido (1..32).");
    return;
  }
  if (senha.length() > 64)
  {
    server.send(400, "text/plain", "Senha muito longa (max 64).");
    return;
  }

  Config nova = cfg;
  strncpy(nova.wifiSsid,  ssid.c_str(),  sizeof(nova.wifiSsid)  - 1);
  nova.wifiSsid[sizeof(nova.wifiSsid) - 1]   = '\0';
  strncpy(nova.wifiSenha, senha.c_str(), sizeof(nova.wifiSenha) - 1);
  nova.wifiSenha[sizeof(nova.wifiSenha) - 1] = '\0';

  if (!salvarConfig(nova))
  {
    server.send(500, "text/plain", "Falha ao gravar NVS.");
    return;
  }
  cfg = nova;

  Serial.printf("[AP] Credenciais salvas para \"%s\". Reiniciando...\n", nova.wifiSsid);
  server.send(200, "application/json", "{\"ok\":true}");
  tReiniciar = millis() + 600;    // reinicia -> tenta conectar na rede nova
}



// #include <Arduino.h>

// /* ============================================================================
//  *  INTERRUPTOR INTELIGENTE - ESP32 (esp32dev)
//  *  ----------------------------------------------------------------------------
//  *  2 lampadas + 2 pulsadores por modulo. Dois modulos identicos formam um PAR
//  *  e conversam por ESP-NOW (broadcast). Atualizacao OTA via ArduinoOTA.
//  *
//  *  >>> NOVIDADES DESTA VERSAO <<<
//  *   - CONFIGURACAO PELA WEB, PERSISTENTE (Preferences/NVS): modulo (1/2), grupo
//  *     do par, hostname, canal WiFi, BSSID fixo (on/off + valor) e TODOS os
//  *     tempos. Salvar reinicia o modulo para aplicar de forma limpa e atomica.
//  *   - MESMO BINARIO PARA TODAS AS PLACAS: nao ha mais #define ID_MODULO. Cada
//  *     placa nasce com um hostname unico derivado do MAC; voce configura modulo
//  *     e grupo pela pagina. Reflashe as duas placas com este firmware.
//  *   - ISOLAMENTO DE PARES: cada mensagem carrega o "grupo". Um modulo so aceita
//  *     mensagens do proprio grupo, entao varios pares coexistem no mesmo canal
//  *     sem interferir. Frames de outros grupos sao descartados ja na recepcao.
//  *   - PAGINA WEB SEPARADA: HTML, CSS e JS em arquivos proprios (web_index.h,
//  *     web_style.h, web_script.h), servidos em "/", "/style.css" e "/app.js".
//  *
//  *  ROBUSTEZ (mantida):
//  *   - ENVIO EM RAJADA (burst): cada comando enviado nRepeticoes vezes.
//  *   - DEDUPLICACAO: (origem, id); copias do burst compartilham a id.
//  *   - COMANDOS IDEMPOTENTES: estado absoluto (liga/desliga), nao "inverte".
//  *   - TX POWER no maximo; ESTADO periodico (heartbeat) ressincroniza o cache.
//  *
//  *  RESILIENCIA DE REDE (mantida):
//  *   - Reconexao automatica; sem conexao por msWifiReset -> ESP.restart().
//  *   - mDNS/OTA/HTTP e ESP-NOW recriados a cada (re)conexao via eventos WiFi.
//  *   - WiFi.setSleep(false): sem isso o radio dorme e descarta frames ESP-NOW.
//  *   - mDNS sob controle explicito do sketch (o .local volta sozinho apos queda).
//  *
//  *  GESTOS:
//  *   - Toque curto no pulsador N   -> inverte a lampada N do proprio modulo.
//  *   - Segurar os DOIS por 1s      -> entra em SINCRONISMO (espelhamento).
//  *   - Segurar UM pulsador por 1s  -> "master toggle" (liga/desliga geral).
//  *
//  *  CONTROLE POR REDE (HTTP):
//  *       GET  /status                      -> estado atual em JSON
//  *       GET  /lamp?n=1|2&s=on|off|toggle  -> comanda 1 lampada local
//  *       GET  /master?s=on|off|toggle      -> comanda as 2 (propaga p/ o par)
//  *       GET  /sync?s=on|off               -> entra/sai de sincronismo
//  *       GET  /config                      -> configuracao atual em JSON
//  *       POST /config                      -> salva configuracao e reinicia
//  *
//  *  Autor: Thiago
//  * ========================================================================== */

// #include <WiFi.h>
// #include <esp_now.h>
// #include <WebServer.h>
// #include <ESPmDNS.h>
// #include <ArduinoOTA.h>
// #include <Preferences.h>
// #include <DNSServer.h>

// #include "web_index.h"   // WEB_INDEX_HTML  -> "/"
// #include "web_style.h"   // WEB_STYLE_CSS   -> "/style.css"
// #include "web_script.h"  // WEB_SCRIPT_JS   -> "/app.js"
// #include "web_portal.h"  // WEB_PORTAL_HTML -> "/" no modo AP

// /* ============================= CREDENCIAIS ================================ */
// #define WIFI_SSID  "SENAI IoT"      // rede PADRAO de fabrica (reconfiguravel no portal AP)
// #define WIFI_SENHA "info@IoT"
// #define OTA_SENHA  "info@134"

// // Acesso web protegido por Digest (senha nao trafega em texto claro).
// // Protege controle (/lamp,/master,/sync) e configuracao (/config).
// #define WEB_USER  "admin"
// #define WEB_SENHA "senha"

// /* ============================== HARDWARE =================================
//  * Pinos e polaridade sao fixos (dependem da placa), nao vao para a config.   */
// #define PINO_LAMPADA_1 13
// #define PINO_LAMPADA_2 12
// #define PINO_BOTAO_1   14
// #define PINO_BOTAO_2   27
// #define LAMPADA_ATIVO_ALTO true
// #define BOTAO_PRESSIONADO  LOW
// #define PINO_BOOT 0          // botao BOOT (GPIO0) -> segurar 10s entra em modo AP

// /* ======================= CONSTANTES DE COMPILACAO ========================= */
// #define HTTP_PORTA 80        // porta do servidor HTTP
// #define FILA_TAM   16         // tamanho do buffer circular de RX (potencia de 2)

// /* -------- Modo AP / portal de configuracao WiFi -------- */
// #define AP_SENHA          "configurar"   // senha do SoftAP de configuracao (>= 8 chars)
// #define MS_BOOT_AP        10000UL        // segurar BOOT por 10s -> modo AP
// #define MS_BOOT_CONNECT   20000UL        // timeout de conexao no boot antes de cair no AP
// #define MS_PORTAL_TIMEOUT 180000UL       // sem cliente por 3 min -> reinicia e tenta STA
// #define NVS_CHAVE_APREQ   "apReq"        // flag NVS: pedir modo AP no proximo boot

// /* ===================== VALORES PADRAO DE CONFIGURACAO =====================
//  * Usados no PRIMEIRO boot (NVS vazio) ou quando a config salva e invalida.   */
// #define PADRAO_CANAL          0        // 0 = automatico (segue o AP)
// #define PADRAO_MS_DEBONCE     50
// #define PADRAO_MS_LONGA       1000
// #define PADRAO_MS_TIMEOUT     7000
// #define PADRAO_MS_HEARTBEAT   500
// #define PADRAO_N_REPETICOES   3
// #define PADRAO_MS_BURST_GAP   8
// #define PADRAO_MS_WIFI_CHECK  5000
// #define PADRAO_MS_WIFI_RESET  300000UL

// /* Preferences (NVS) */
// #define NVS_NAMESPACE "interr"
// #define NVS_CHAVE_CFG "cfg"
// #define CFG_MAGIC     0xA53C0003UL   // muda quando o layout do struct muda (+wifiSsid/senha)

// /* ============================ CONFIGURACAO ===============================
//  * Tudo que e persistido em NVS. Um "magic" identifica versao/validade.       */
// struct Config
// {
//   uint32_t magic;          // CFG_MAGIC -> config valida e compativel
//   uint8_t  modulo;         // 1 ou 2 (identidade dentro do par)
//   uint8_t  grupo;          // 1..255 (id do par -> isola de outros pares)
//   uint8_t  canal;          // 0=auto, 1..13
//   bool     usarBssid;      // prender a um AP especifico?
//   uint8_t  bssid[6];       // MAC do AP (se usarBssid)
//   char     hostname[32];   // nome mDNS / DHCP
//   char     wifiSsid[33];   // credenciais WiFi (reconfiguraveis pelo portal AP)
//   char     wifiSenha[65];

//   // Tempos (ms) — parametrizaveis pela pagina
//   uint16_t msDebounce;
//   uint16_t msPressaoLonga;
//   uint16_t msTimeoutSync;
//   uint16_t msHeartbeat;
//   uint8_t  nRepeticoes;    // copias por comando (burst)
//   uint16_t msBurstGap;
//   uint32_t msWifiCheck;
//   uint32_t msWifiReset;
// };

// /* ============================== TIPOS / MSG ==============================
//  * "grupo" e o primeiro campo: filtramos por ele logo na recepcao.            */
// enum TipoMsg : uint8_t
// {
//   MSG_ESTADO      = 1,
//   MSG_ESPELHAR    = 2,
//   MSG_ENTRAR_SYNC = 3,
//   MSG_MASTER_SET  = 4
// };

// typedef struct __attribute__((packed))
// {
//   uint8_t grupo;   // id do par -> RX ignora se != cfg.grupo (isola pares)
//   uint8_t origem;  // modulo remetente (1 ou 2) -> dedup
//   uint8_t id;      // sequencia por remetente; iguais no burst -> dedup
//   uint8_t tipo;
//   uint8_t indice;  // 0=lampada 1, 1=lampada 2 (MSG_ESPELHAR)
//   uint8_t estado;  // 0/1 estado alvo (MSG_ESPELHAR)
//   uint8_t lamp1;   // estado completo (MSG_ESTADO / MSG_MASTER_SET)
//   uint8_t lamp2;
// } Mensagem;

// struct Botao
// {
//   uint8_t  pino;
//   bool     pressionado;
//   bool     leituraAnt;
//   uint32_t tDebounce;
//   uint32_t tPressionado;
//   bool     acaoLongaFeita;
//   bool     consumido;
// };

// /* ============================== PROTOTIPOS ================================ */
// // Config / NVS
// void   configPadrao(Config &c);
// void   carregarConfig();
// bool   salvarConfig(const Config &c);
// bool   parseBssid(const String &s, uint8_t out[6]);
// String bssidParaStr(const uint8_t b[6]);

// // Nucleo
// const char *nomeTipo(uint8_t t);
// void escreveLampada(uint8_t i, bool ligada);
// void enviarRobusto(Mensagem m, uint8_t repeticoes);
// void notificarEstado();
// #if ESP_ARDUINO_VERSION_MAJOR >= 3
// void aoEnviar(const wifi_tx_info_t *info, esp_now_send_status_t status);
// void aoReceber(const esp_now_recv_info_t *info, const uint8_t *dados, int tam);
// #else
// void aoEnviar(const uint8_t *mac, esp_now_send_status_t status);
// void aoReceber(const uint8_t *mac, const uint8_t *dados, int tam);
// #endif
// void processarMensagem(const Mensagem &m);
// void toggleLampada(uint8_t i);
// void entrarEmSync();
// void masterToggle();
// uint8_t atualizaBotao(Botao &b);
// void tratarBotoes();
// void onWiFiEvent(WiFiEvent_t event);
// void iniciarServicos();
// void iniciarEspNow();
// void tratarWiFi();
// void comandarLampada(uint8_t i, bool estado);
// void comandarMaster(bool alvo);

// // Modo AP / portal de configuracao
// bool   apReqPendente();
// void   marcarApReq();
// void   limparApReq();
// void   verificarBotaoBoot();
// void   iniciarModoAP();
// void   loopPortal();
// void   configurarRotasPortal();
// String jsonEscape(const String &s);
// void   httpPortalRaiz();
// void   httpPortalScan();
// void   httpPortalSave();
// void   httpPortalCaptive();

// // HTTP
// bool exigirAuth();
// int  lerEstado(const String &s);
// void configurarRotas();
// void httpRaiz();
// void httpEstilo();
// void httpScript();
// void httpStatus();
// void httpLamp();
// void httpMaster();
// void httpSync();
// void httpGetConfig();
// void httpPostConfig();
// void httpNaoEncontrado();

// /* =============================== GLOBAIS ================================= */
// Config      cfg;                     // configuracao viva (carregada do NVS)
// Preferences prefs;
// WebServer   server(HTTP_PORTA);

// uint8_t enderecoBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// bool lampada[2]    = {false, false};
// bool lampRemota[2] = {false, false};

// bool     modoSync = false;
// uint32_t ultimaAtividadeSync = 0;

// uint8_t idSaida = 0;
// uint8_t ultimoIdDe[3] = {255, 255, 255};   // indexado por origem (1..2)

// volatile uint32_t rxTotal = 0;
// volatile uint32_t txOk    = 0;
// volatile uint32_t txFail  = 0;
// uint32_t rxAnterior = 0;

// Botao botao[2];
// uint32_t tAmbos = 0;
// bool syncDisparado = false;

// volatile Mensagem fila[FILA_TAM];
// volatile uint8_t filaInicio = 0;
// volatile uint8_t filaFim    = 0;

// volatile bool servicosPendentes = false;
// volatile bool espnowPendente    = false;
// volatile bool otaAtiva          = false;

// uint32_t tReiniciar = 0;   // != 0 -> ESP.restart() quando millis() alcancar (config salva)

// DNSServer dnsServer;       // captive DNS (so no modo AP)
// bool      modoAP = false;  // true -> rodando o portal de configuracao
// uint32_t  portalInicio = 0;

// /* ==========================================================================
//  *  CONFIGURACAO / NVS
//  * ========================================================================== */
// void configPadrao(Config &c)
// {
//   memset(&c, 0, sizeof(c));
//   c.magic     = CFG_MAGIC;
//   c.modulo    = 1;
//   c.grupo     = 1;
//   c.canal     = PADRAO_CANAL;
//   c.usarBssid = false;

//   // Hostname unico por placa (ultimos 3 bytes do MAC) -> evita colisao mDNS
//   // no primeiro boot, antes de o usuario renomear.
//   uint8_t mac[6];
//   WiFi.macAddress(mac);
//   snprintf(c.hostname, sizeof(c.hostname), "interruptor-%02X%02X%02X",
//            mac[3], mac[4], mac[5]);

//   strncpy(c.wifiSsid,  WIFI_SSID,  sizeof(c.wifiSsid)  - 1);
//   strncpy(c.wifiSenha, WIFI_SENHA, sizeof(c.wifiSenha) - 1);

//   c.msDebounce     = PADRAO_MS_DEBONCE;
//   c.msPressaoLonga = PADRAO_MS_LONGA;
//   c.msTimeoutSync  = PADRAO_MS_TIMEOUT;
//   c.msHeartbeat    = PADRAO_MS_HEARTBEAT;
//   c.nRepeticoes    = PADRAO_N_REPETICOES;
//   c.msBurstGap     = PADRAO_MS_BURST_GAP;
//   c.msWifiCheck    = PADRAO_MS_WIFI_CHECK;
//   c.msWifiReset    = PADRAO_MS_WIFI_RESET;
// }

// void carregarConfig()
// {
//   prefs.begin(NVS_NAMESPACE, false);
//   size_t lido = prefs.getBytes(NVS_CHAVE_CFG, &cfg, sizeof(cfg));

//   if (lido != sizeof(cfg) || cfg.magic != CFG_MAGIC)
//   {
//     Serial.println("[CFG] NVS vazio/incompativel -> gravando padroes.");
//     configPadrao(cfg);
//     prefs.putBytes(NVS_CHAVE_CFG, &cfg, sizeof(cfg));
//   }
//   prefs.end();

//   Serial.printf("[CFG] modulo=%d grupo=%d canal=%d host=%s bssid=%s(%s)\n",
//                 cfg.modulo, cfg.grupo, cfg.canal, cfg.hostname,
//                 cfg.usarBssid ? "on" : "off", bssidParaStr(cfg.bssid).c_str());
// }

// bool salvarConfig(const Config &c)
// {
//   prefs.begin(NVS_NAMESPACE, false);
//   size_t n = prefs.putBytes(NVS_CHAVE_CFG, &c, sizeof(c));
//   prefs.end();
//   return n == sizeof(c);
// }

// // Flag "entrar no portal AP no proximo boot" (chave NVS separada do blob cfg).
// bool apReqPendente()
// {
//   prefs.begin(NVS_NAMESPACE, true);
//   bool r = prefs.getBool(NVS_CHAVE_APREQ, false);
//   prefs.end();
//   return r;
// }
// void marcarApReq()
// {
//   prefs.begin(NVS_NAMESPACE, false);
//   prefs.putBool(NVS_CHAVE_APREQ, true);
//   prefs.end();
// }
// void limparApReq()
// {
//   prefs.begin(NVS_NAMESPACE, false);
//   prefs.remove(NVS_CHAVE_APREQ);
//   prefs.end();
// }

// bool parseBssid(const String &s, uint8_t out[6])
// {
//   int v[6];
//   if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x",
//              &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
//     return false;
//   for (int i = 0; i < 6; i++)
//   {
//     if (v[i] < 0 || v[i] > 255) return false;
//     out[i] = (uint8_t)v[i];
//   }
//   return true;
// }

// String bssidParaStr(const uint8_t b[6])
// {
//   char s[18];
//   snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X",
//            b[0], b[1], b[2], b[3], b[4], b[5]);
//   return String(s);
// }

// /* ==========================================================================
//  *  SETUP
//  * ========================================================================== */
// void setup()
// {
//   Serial.begin(115200);
//   delay(200);

//   Serial.println();
//   Serial.println("=====================================================");
//   Serial.println("  INTERRUPTOR INTELIGENTE ESP32");
//   Serial.println("=====================================================");

//   pinMode(PINO_LAMPADA_1, OUTPUT);
//   pinMode(PINO_LAMPADA_2, OUTPUT);
//   escreveLampada(0, false);
//   escreveLampada(1, false);

//   botao[0] = {PINO_BOTAO_1, false, false, 0, 0, false, false};
//   botao[1] = {PINO_BOTAO_2, false, false, 0, 0, false, false};
//   pinMode(PINO_BOTAO_1, INPUT_PULLUP);
//   pinMode(PINO_BOTAO_2, INPUT_PULLUP);
//   pinMode(PINO_BOOT,    INPUT_PULLUP);   // BOOT/GPIO0 -> entrada no modo AP

//   WiFi.mode(WIFI_STA);            // necessario antes de ler o MAC / conectar
//   WiFi.setAutoReconnect(true);

//   carregarConfig();               // <<< define modulo/grupo/hostname/canal/tempos/wifi

//   // Portal de configuracao solicitado? (falha de conexao anterior ou BOOT 10s)
//   if (apReqPendente())
//   {
//     limparApReq();                // limpa: um novo boot volta a tentar a estacao
//     iniciarModoAP();
//     return;                       // segue no loop() em modo portal
//   }

//   WiFi.setHostname(cfg.hostname);
//   WiFi.onEvent(onWiFiEvent);      // dispara (re)inicio de servicos -> antes do begin

//   WiFi.setSleep(false);           // CRITICO: sem modem sleep -> ESP-NOW confiavel
//   WiFi.setTxPower(WIFI_POWER_19_5dBm);
//   Serial.printf("[WiFi] Hostname: %s | modem sleep OFF | TX 19.5dBm\n", cfg.hostname);

//   const uint8_t *bssid = cfg.usarBssid ? cfg.bssid : nullptr;
//   WiFi.begin(cfg.wifiSsid, cfg.wifiSenha, cfg.canal, bssid);
//   Serial.printf("[WiFi] Conectando a \"%s\" (canal:%d, bssid fixo:%s)",
//                 cfg.wifiSsid, cfg.canal, cfg.usarBssid ? bssidParaStr(cfg.bssid).c_str() : "nao");

//   uint32_t t0 = millis();
//   while (WiFi.status() != WL_CONNECTED && (millis() - t0) < MS_BOOT_CONNECT)
//   {
//     delay(250);
//     Serial.print(".");
//   }
//   Serial.println();

//   if (WiFi.status() != WL_CONNECTED)
//   {
//     Serial.println("[WiFi] NAO conectou -> reiniciando em modo AP (portal de config).");
//     marcarApReq();
//     delay(100);
//     ESP.restart();
//   }

//   Serial.printf("[WiFi] CONECTADO. IP=%s  MAC=%s  BSSID=%s  CANAL=%d\n",
//                 WiFi.localIP().toString().c_str(), WiFi.macAddress().c_str(),
//                 WiFi.BSSIDstr().c_str(), WiFi.channel());
//   Serial.println("[WiFi] >>> O CANAL precisa ser IGUAL nos dois modulos do par! <<<");

//   configurarRotas();              // callbacks HTTP persistem entre reconexoes

//   Serial.println("[SYS] Sistema iniciado.");
//   Serial.println("-----------------------------------------------------");
// }

// /* ==========================================================================
//  *  LOOP
//  * ========================================================================== */
// void loop()
// {
//   if (modoAP)                     // portal de configuracao WiFi (SoftAP)
//   {
//     loopPortal();
//     return;
//   }

//   ArduinoOTA.handle();

//   if (otaAtiva)                   // durante upload OTA, so cuidamos disso
//     return;

//   // Reinicio agendado (apos salvar config) — dá tempo de a resposta HTTP sair.
//   if (tReiniciar && millis() >= tReiniciar)
//   {
//     Serial.println("[SYS] Reiniciando para aplicar configuracao...");
//     delay(50);
//     ESP.restart();
//   }

//   if (espnowPendente)             // ESP-NOW sobe na ASSOCIACAO (independe de IP)
//   {
//     espnowPendente = false;
//     iniciarEspNow();
//   }

//   if (servicosPendentes)          // mDNS/OTA/HTTP sobem quando ha IP
//   {
//     servicosPendentes = false;
//     iniciarServicos();
//   }

//   server.handleClient();
//   tratarBotoes();
//   verificarBotaoBoot();           // segurar BOOT (GPIO0) 10s -> modo AP

//   while (filaInicio != filaFim)
//   {
//     Mensagem m;
//     memcpy(&m, (const void *)&fila[filaInicio], sizeof(Mensagem));
//     filaInicio = (filaInicio + 1) % FILA_TAM;
//     processarMensagem(m);
//   }

//   if (modoSync && (millis() - ultimaAtividadeSync) > cfg.msTimeoutSync)
//   {
//     modoSync = false;
//     Serial.println("[SYNC] Sincronismo encerrado (inatividade).");
//   }

//   tratarWiFi();

//   // ---------------------- HEARTBEAT / DIAGNOSTICO -------------------------
//   static uint32_t tHb = 0;
//   if (millis() - tHb >= cfg.msHeartbeat)
//   {
//     tHb = millis();
//     notificarEstado();

//     uint32_t rxAgora = rxTotal;
//     uint32_t delta   = rxAgora - rxAnterior;
//     rxAnterior = rxAgora;

//     Serial.printf("[HB] G%d M%d Canal:%d RX:%lu(+%lu) TXok:%lu TXfail:%lu Sync:%s\n",
//                   cfg.grupo, cfg.modulo, WiFi.channel(),
//                   (unsigned long)rxAgora, (unsigned long)delta,
//                   (unsigned long)txOk, (unsigned long)txFail,
//                   modoSync ? "SIM" : "nao");
//   }
// }

// /* ==========================================================================
//  *  UTIL / LAMPADAS
//  * ========================================================================== */
// const char *nomeTipo(uint8_t t)
// {
//   switch (t)
//   {
//   case MSG_ESTADO:      return "ESTADO";
//   case MSG_ESPELHAR:    return "ESPELHAR";
//   case MSG_ENTRAR_SYNC: return "ENTRAR_SYNC";
//   case MSG_MASTER_SET:  return "MASTER_SET";
//   default:              return "DESCONHECIDO";
//   }
// }

// void escreveLampada(uint8_t i, bool ligada)
// {
//   lampada[i] = ligada;
//   bool nivel = LAMPADA_ATIVO_ALTO ? ligada : !ligada;
//   digitalWrite(i == 0 ? PINO_LAMPADA_1 : PINO_LAMPADA_2, nivel ? HIGH : LOW);
//   Serial.printf("[LAMP] Lampada %d -> %s\n", i + 1, ligada ? "ON" : "OFF");
// }

// /* ==========================================================================
//  *  ESP-NOW
//  * ========================================================================== */
// void enviarRobusto(Mensagem m, uint8_t repeticoes)
// {
//   m.grupo  = cfg.grupo;           // carimba o par (isolamento)
//   m.origem = cfg.modulo;
//   m.id     = ++idSaida;

//   for (uint8_t k = 0; k < repeticoes; k++)
//   {
//     esp_err_t r = esp_now_send(enderecoBroadcast, (const uint8_t *)&m, sizeof(m));
//     if (r != ESP_OK)
//       Serial.printf("[TX] Falha esp_now_send (%s, err=%d)\n", nomeTipo(m.tipo), r);
//     if (k + 1 < repeticoes)
//       delay(cfg.msBurstGap);
//   }
//   Serial.printf("[TX] %s enviado %dx (id=%u)\n", nomeTipo(m.tipo), repeticoes, m.id);
// }

// void notificarEstado()
// {
//   Mensagem m = {};
//   m.tipo  = MSG_ESTADO;
//   m.lamp1 = lampada[0];
//   m.lamp2 = lampada[1];
//   enviarRobusto(m, 1);
// }

// #if ESP_ARDUINO_VERSION_MAJOR >= 3
// void aoEnviar(const wifi_tx_info_t *info, esp_now_send_status_t status)
// {
// #else
// void aoEnviar(const uint8_t *mac, esp_now_send_status_t status)
// {
// #endif
//   if (status == ESP_NOW_SEND_SUCCESS) txOk++;
//   else                                txFail++;
// }

// #if ESP_ARDUINO_VERSION_MAJOR >= 3
// void aoReceber(const esp_now_recv_info_t *info, const uint8_t *dados, int tam)
// {
// #else
// void aoReceber(const uint8_t *mac, const uint8_t *dados, int tam)
// {
// #endif
//   if (tam < (int)sizeof(Mensagem))
//     return;

//   // ISOLAMENTO DE PARES: descarta frames de outros grupos ja aqui, para nao
//   // ocupar a fila nem contar como RX. O "grupo" e o 1o byte da mensagem.
//   if (dados[0] != cfg.grupo)
//     return;

//   rxTotal++;
//   uint8_t prox = (filaFim + 1) % FILA_TAM;
//   if (prox == filaInicio)         // fila cheia -> descarta
//     return;
//   memcpy((void *)&fila[filaFim], dados, sizeof(Mensagem));
//   filaFim = prox;
// }

// void processarMensagem(const Mensagem &m)
// {
//   // Grupo ja foi filtrado em aoReceber(); aqui so cuidamos da deduplicacao.
//   if (m.origem >= 1 && m.origem <= 2)
//   {
//     if (m.id == ultimoIdDe[m.origem])
//       return;
//     ultimoIdDe[m.origem] = m.id;
//   }

//   Serial.printf("[RX] %s de mod%d (id=%u idx=%d est=%d l1=%d l2=%d)\n",
//                 nomeTipo(m.tipo), m.origem, m.id, m.indice, m.estado, m.lamp1, m.lamp2);

//   switch (m.tipo)
//   {
//   case MSG_ESTADO:
//     lampRemota[0] = m.lamp1;
//     lampRemota[1] = m.lamp2;
//     break;

//   case MSG_ESPELHAR:
//     if (m.indice < 2)
//       escreveLampada(m.indice, m.estado);
//     if (!modoSync)
//       Serial.println("[SYNC] Entrando em sync (via ESPELHAR).");
//     modoSync = true;
//     ultimaAtividadeSync = millis();
//     notificarEstado();
//     break;

//   case MSG_ENTRAR_SYNC:
//     if (!modoSync)
//       Serial.println("[SYNC] Entrando em sync (pedido do outro modulo).");
//     modoSync = true;
//     ultimaAtividadeSync = millis();
//     break;

//   case MSG_MASTER_SET:
//     Serial.println("[MASTER] Aplicando master set recebido.");
//     escreveLampada(0, m.lamp1);
//     escreveLampada(1, m.lamp2);
//     notificarEstado();
//     break;
//   }
// }

// /* ==========================================================================
//  *  GESTOS
//  * ========================================================================== */
// void toggleLampada(uint8_t i)
// {
//   Serial.printf("[GESTO] Toque curto no pulsador %d\n", i + 1);
//   escreveLampada(i, !lampada[i]);
//   notificarEstado();

//   if (modoSync)
//   {
//     Serial.printf("[SYNC] Espelhando lampada %d no outro modulo.\n", i + 1);
//     Mensagem m = {};
//     m.tipo   = MSG_ESPELHAR;
//     m.indice = i;
//     m.estado = lampada[i];
//     enviarRobusto(m, cfg.nRepeticoes);
//     ultimaAtividadeSync = millis();
//   }
// }

// void entrarEmSync()
// {
//   Serial.println("[GESTO] Ambos pressionados -> entrando em SYNC.");
//   modoSync = true;
//   ultimaAtividadeSync = millis();
//   Mensagem m = {};
//   m.tipo = MSG_ENTRAR_SYNC;
//   enviarRobusto(m, cfg.nRepeticoes);
// }

// void masterToggle()
// {
//   bool algumAceso = lampada[0] || lampada[1] || lampRemota[0] || lampRemota[1];
//   bool alvo = !algumAceso;

//   Serial.printf("[GESTO] Master toggle -> %s tudo (local+remoto).\n",
//                 alvo ? "LIGAR" : "DESLIGAR");

//   escreveLampada(0, alvo);
//   escreveLampada(1, alvo);

//   Mensagem m = {};
//   m.tipo  = MSG_MASTER_SET;
//   m.lamp1 = alvo;
//   m.lamp2 = alvo;
//   enviarRobusto(m, cfg.nRepeticoes);
//   notificarEstado();
// }

// /* ==========================================================================
//  *  BOTOES
//  * ========================================================================== */
// uint8_t atualizaBotao(Botao &b)
// {
//   bool leitura = (digitalRead(b.pino) == BOTAO_PRESSIONADO);
//   uint8_t evento = 0;

//   if (leitura != b.leituraAnt)
//   {
//     b.tDebounce = millis();
//     b.leituraAnt = leitura;
//   }

//   if ((millis() - b.tDebounce) > cfg.msDebounce && leitura != b.pressionado)
//   {
//     b.pressionado = leitura;
//     if (b.pressionado)
//     {
//       b.tPressionado   = millis();
//       b.acaoLongaFeita = false;
//       b.consumido      = false;
//       evento = 1;                 // pressionou
//     }
//     else
//     {
//       evento = 2;                 // soltou
//     }
//   }
//   return evento;
// }

// void tratarBotoes()
// {
//   uint8_t ev0 = atualizaBotao(botao[0]);
//   uint8_t ev1 = atualizaBotao(botao[1]);

//   bool ambos = botao[0].pressionado && botao[1].pressionado;

//   if (ambos)
//   {
//     if (tAmbos == 0)
//     {
//       tAmbos = millis();
//       syncDisparado = false;
//     }
//   }
//   else
//   {
//     tAmbos = 0;
//   }

//   // Segurar os dois -> entra em sync
//   if (ambos && !syncDisparado && (millis() - tAmbos) >= cfg.msPressaoLonga)
//   {
//     entrarEmSync();
//     syncDisparado = true;
//     botao[0].consumido = botao[1].consumido = true;
//     botao[0].acaoLongaFeita = botao[1].acaoLongaFeita = true;
//   }

//   // Segurar UM -> master toggle
//   for (uint8_t i = 0; i < 2; i++)
//   {
//     uint8_t j = 1 - i;
//     if (botao[i].pressionado && !botao[j].pressionado &&
//         !botao[i].acaoLongaFeita && !botao[i].consumido &&
//         (millis() - botao[i].tPressionado) >= cfg.msPressaoLonga)
//     {
//       masterToggle();
//       botao[i].acaoLongaFeita = true;
//       botao[i].consumido = true;
//     }
//   }

//   // Toque curto (soltou antes da pressao longa) -> inverte a lampada
//   for (uint8_t i = 0; i < 2; i++)
//   {
//     uint8_t ev = (i == 0) ? ev0 : ev1;
//     if (ev == 2 && !botao[i].consumido &&
//         (millis() - botao[i].tPressionado) < cfg.msPressaoLonga)
//     {
//       toggleLampada(i);
//     }
//   }
// }

// /* ==========================================================================
//  *  REDE / SERVICOS
//  * ========================================================================== */
// // Handler de eventos WiFi: apenas sinaliza flags; o trabalho roda no loop.
// void onWiFiEvent(WiFiEvent_t event)
// {
//   if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED)
//     espnowPendente = true;        // canal ja definido -> ESP-NOW pode subir
//   else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
//     servicosPendentes = true;     // ha IP -> mDNS/OTA/HTTP
// }

// // (Re)inicia ArduinoOTA (1x), mDNS (sempre) e HTTP a cada (re)conexao.
// void iniciarServicos()
// {
//   // --- ArduinoOTA: configurado UMA vez (begin() e no-op depois) ---
//   static bool otaConfigurada = false;
//   if (!otaConfigurada)
//   {
//     ArduinoOTA.setHostname(cfg.hostname);
//     ArduinoOTA.setPassword(OTA_SENHA);
//     ArduinoOTA.setMdnsEnabled(false);   // nos controlamos o mDNS

//     ArduinoOTA.onStart([]()
//     {
//       otaAtiva = true;
//       server.stop();
//       esp_now_deinit();
//       Serial.println("[OTA] Upload iniciado -> web/ESP-NOW suspensos.");
//     });
//     ArduinoOTA.onProgress([](unsigned int enviado, unsigned int total)
//     {
//       Serial.printf("[OTA] %u%%\r", total ? (enviado * 100u / total) : 0u);
//     });
//     ArduinoOTA.onEnd([]()
//     {
//       Serial.println("\n[OTA] Concluido. Reiniciando...");
//     });
//     ArduinoOTA.onError([](ota_error_t erro)
//     {
//       otaAtiva = false;
//       espnowPendente = true;
//       servicosPendentes = true;
//       Serial.printf("\n[OTA] Erro %u -> servicos restaurados.\n", erro);
//     });

//     ArduinoOTA.begin();
//     otaConfigurada = true;
//     Serial.println("[OTA] ArduinoOTA configurado (mDNS proprio desligado).");
//   }

//   // --- mDNS reiniciado a CADA (re)conexao (o responder nao sobrevive a queda) ---
//   MDNS.end();
//   if (MDNS.begin(cfg.hostname))
//   {
//     MDNS.setInstanceName(String("Interruptor G") + cfg.grupo + " M" + cfg.modulo);
//     MDNS.enableArduino(3232, true);
//     MDNS.addService("http", "tcp", HTTP_PORTA);
//     MDNS.addServiceTxt("http", "tcp", "grupo",  String(cfg.grupo).c_str());
//     MDNS.addServiceTxt("http", "tcp", "modulo", String(cfg.modulo).c_str());
//     MDNS.addServiceTxt("http", "tcp", "path", "/");
//     Serial.printf("[mDNS] OK -> %s.local (http porta %d)\n", cfg.hostname, HTTP_PORTA);
//   }
//   else
//   {
//     Serial.println("[mDNS] FALHA em MDNS.begin() -> nome .local NAO vai resolver.");
//   }

//   // --- Servidor HTTP: reabre o socket de escuta ---
//   server.stop();
//   server.begin();
//   Serial.printf("[HTTP] Servidor (re)iniciado na porta %d.\n", HTTP_PORTA);
// }

// // (Re)inicia ESP-NOW. NAO depende de IP -> disparado na ASSOCIACAO.
// void iniciarEspNow()
// {
//   esp_now_deinit();
//   if (esp_now_init() != ESP_OK)
//   {
//     Serial.println("[ESP-NOW] FALHA ao (re)iniciar!");
//     return;
//   }
//   esp_now_register_recv_cb(aoReceber);
//   esp_now_register_send_cb(aoEnviar);

//   esp_now_peer_info_t peer = {};
//   memcpy(peer.peer_addr, enderecoBroadcast, 6);
//   peer.channel = 0;
//   peer.encrypt = false;
//   if (esp_now_add_peer(&peer) != ESP_OK)
//     Serial.println("[ESP-NOW] FALHA ao adicionar peer broadcast!");
//   else
//     Serial.printf("[ESP-NOW] (Re)iniciado (grupo %d, broadcast, burst+dedup, canal %d).\n",
//                   cfg.grupo, WiFi.channel());
// }

// // Checagem periodica de conexao; reconecta e, em ultimo caso, reinicia.
// void tratarWiFi()
// {
//   static uint32_t tWifiCheck = 0;
//   static uint32_t tSemWifi   = 0;

//   if (millis() - tWifiCheck < cfg.msWifiCheck)
//     return;
//   tWifiCheck = millis();

//   if (WiFi.status() != WL_CONNECTED)
//   {
//     if (tSemWifi == 0)
//       tSemWifi = millis();
//     Serial.println("[WiFi] Queda detectada -> reconectando...");
//     WiFi.reconnect();

//     if (millis() - tSemWifi > cfg.msWifiReset)
//     {
//       Serial.println("[WiFi] Sem conexao ha muito tempo -> reiniciando.");
//       ESP.restart();
//     }
//   }
//   else
//   {
//     tSemWifi = 0;
//   }
// }

// /* ==========================================================================
//  *  BOTAO BOOT / MODO AP (PORTAL DE CONFIGURACAO WiFi)
//  * ========================================================================== */
// // Segurar o BOOT (GPIO0) por MS_BOOT_AP -> agenda modo AP e reinicia.
// void verificarBotaoBoot()
// {
//   static uint32_t tBoot = 0;
//   static bool avisou = false;

//   if (digitalRead(PINO_BOOT) == LOW)     // BOOT pressionado (ativo em LOW)
//   {
//     if (tBoot == 0) { tBoot = millis(); avisou = false; }

//     if (!avisou && (millis() - tBoot) >= 2000)
//     {
//       Serial.println("[SYS] Segure o BOOT por 10s para entrar no modo de configuracao...");
//       avisou = true;
//     }
//     if ((millis() - tBoot) >= MS_BOOT_AP)
//     {
//       Serial.println("[SYS] BOOT 10s -> reiniciando em modo AP.");
//       marcarApReq();
//       delay(100);
//       ESP.restart();
//     }
//   }
//   else
//   {
//     tBoot = 0;
//   }
// }

// // Escapa aspas/barras/controle para embutir texto (ex.: SSID) em JSON.
// String jsonEscape(const String &s)
// {
//   String o;
//   o.reserve(s.length() + 4);
//   for (size_t i = 0; i < s.length(); i++)
//   {
//     char c = s[i];
//     if (c == '"' || c == '\\')      { o += '\\'; o += c; }
//     else if (c == '\n')             { o += "\\n"; }
//     else if ((uint8_t)c < 0x20)     { /* ignora controles */ }
//     else                            { o += c; }
//   }
//   return o;
// }

// // Sobe SoftAP + DNS captive + servidor do portal. Sem ESP-NOW/OTA/mDNS aqui.
// void iniciarModoAP()
// {
//   modoAP = true;
//   WiFi.mode(WIFI_AP_STA);          // AP para o portal + STA para escanear redes
//   WiFi.disconnect();               // garante estacao ociosa (so escaneia)

//   char apSsid[32];
//   uint8_t mac[6];
//   WiFi.macAddress(mac);
//   snprintf(apSsid, sizeof(apSsid), "Interruptor-Cfg-%02X%02X%02X", mac[3], mac[4], mac[5]);
//   WiFi.softAP(apSsid, AP_SENHA);

//   IPAddress ip = WiFi.softAPIP();
//   dnsServer.start(53, "*", ip);    // captive: qualquer dominio -> IP do portal
//   configurarRotasPortal();
//   server.begin();

//   WiFi.scanNetworks(true);         // 1a varredura assincrona
//   portalInicio = millis();

//   Serial.println("=====================================================");
//   Serial.println("  MODO CONFIGURACAO (SoftAP)");
//   Serial.printf ("  SSID : %s\n", apSsid);
//   Serial.printf ("  Senha: %s\n", AP_SENHA);
//   Serial.printf ("  Abra : http://%s/\n", ip.toString().c_str());
//   Serial.println("=====================================================");
// }

// void loopPortal()
// {
//   dnsServer.processNextRequest();
//   server.handleClient();

//   // Reinicio agendado apos salvar credenciais -> tenta a nova rede.
//   if (tReiniciar && millis() >= tReiniciar)
//   {
//     Serial.println("[AP] Credenciais salvas -> reiniciando para conectar...");
//     delay(50);
//     ESP.restart();
//   }

//   // Havendo cliente no AP, zera o timeout (nao reinicia sob o usuario).
//   if (WiFi.softAPgetStationNum() > 0)
//     portalInicio = millis();

//   // Sem ninguem por muito tempo -> reinicia e tenta a estacao de novo
//   // (auto-recuperacao caso a queda de WiFi tenha sido transitoria).
//   if (millis() - portalInicio > MS_PORTAL_TIMEOUT)
//   {
//     Serial.println("[AP] Timeout sem cliente -> reiniciando para tentar conectar.");
//     ESP.restart();
//   }
// }

// /* ==========================================================================
//  *  COMANDOS (gestos / HTTP)
//  * ========================================================================== */
// void comandarLampada(uint8_t i, bool estado)
// {
//   escreveLampada(i, estado);
//   notificarEstado();

//   if (modoSync)
//   {
//     Serial.printf("[SYNC] Espelhando lampada %d no outro modulo.\n", i + 1);
//     Mensagem m = {};
//     m.tipo   = MSG_ESPELHAR;
//     m.indice = i;
//     m.estado = estado;
//     enviarRobusto(m, cfg.nRepeticoes);
//     ultimaAtividadeSync = millis();
//   }
// }

// void comandarMaster(bool alvo)
// {
//   Serial.printf("[CMD] Master -> %s tudo (local+remoto).\n", alvo ? "LIGAR" : "DESLIGAR");
//   escreveLampada(0, alvo);
//   escreveLampada(1, alvo);

//   Mensagem m = {};
//   m.tipo  = MSG_MASTER_SET;
//   m.lamp1 = alvo;
//   m.lamp2 = alvo;
//   enviarRobusto(m, cfg.nRepeticoes);
//   notificarEstado();
// }

// /* ==========================================================================
//  *  SERVIDOR HTTP
//  * ========================================================================== */
// int lerEstado(const String &s)
// {
//   if (s == "on"  || s == "1" || s == "true")  return 1;
//   if (s == "off" || s == "0" || s == "false") return 0;
//   if (s == "toggle" || s == "t")              return -1;
//   return -2;
// }

// void configurarRotas()
// {
//   server.on("/",          httpRaiz);
//   server.on("/style.css", httpEstilo);
//   server.on("/app.js",    httpScript);
//   server.on("/status",    httpStatus);
//   server.on("/lamp",      httpLamp);
//   server.on("/master",    httpMaster);
//   server.on("/sync",      httpSync);
//   server.on("/config",    HTTP_GET,  httpGetConfig);
//   server.on("/config",    HTTP_POST, httpPostConfig);
//   server.onNotFound(httpNaoEncontrado);
// }

// // Exige autenticacao Digest. Usar no topo das rotas de CONTROLE e CONFIG.
// // As visualizacoes (/, /style.css, /app.js, /status) ficam livres.
// // Retorna false ja tendo respondido 401 com o desafio -> o handler so faz "return".
// bool exigirAuth()
// {
//   if (server.authenticate(WEB_USER, WEB_SENHA))
//     return true;
//   server.requestAuthentication(DIGEST_AUTH, "Interruptor",
//                                "Autenticacao necessaria para controle/config.");
//   return false;
// }

// // --- Assets estaticos (PROGMEM). CSS/JS com cache longo. ---
// void httpRaiz()
// {
//   server.sendHeader("Cache-Control", "no-cache");
//   server.send_P(200, "text/html; charset=utf-8", WEB_INDEX_HTML);
// }

// void httpEstilo()
// {
//   server.sendHeader("Cache-Control", "max-age=86400");
//   server.send_P(200, "text/css; charset=utf-8", WEB_STYLE_CSS);
// }

// void httpScript()
// {
//   server.sendHeader("Cache-Control", "max-age=86400");
//   server.send_P(200, "application/javascript; charset=utf-8", WEB_SCRIPT_JS);
// }

// // --- Estado runtime (consumido pela pagina a cada 1.5s) ---
// void httpStatus()
// {
//   char buf[420];
//   snprintf(buf, sizeof(buf),
//            "{\"modulo\":%d,\"grupo\":%d,\"hostname\":\"%s\","
//            "\"lamp1\":%d,\"lamp2\":%d,\"remota1\":%d,\"remota2\":%d,"
//            "\"sync\":%s,\"canal\":%d,"
//            "\"ip\":\"%s\",\"bssid\":\"%s\",\"mac\":\"%s\",\"rssi\":%d}",
//            cfg.modulo, cfg.grupo, cfg.hostname,
//            lampada[0], lampada[1], lampRemota[0], lampRemota[1],
//            modoSync ? "true" : "false", WiFi.channel(),
//            WiFi.localIP().toString().c_str(), WiFi.BSSIDstr().c_str(),
//            WiFi.macAddress().c_str(), WiFi.RSSI());
//   server.send(200, "application/json", buf);
// }

// void httpLamp()
// {
//   if (!exigirAuth()) return;
//   if (!server.hasArg("n") || !server.hasArg("s"))
//   {
//     server.send(400, "text/plain", "uso: /lamp?n=1|2&s=on|off|toggle\n");
//     return;
//   }
//   int n = server.arg("n").toInt();
//   if (n < 1 || n > 2)
//   {
//     server.send(400, "text/plain", "parametro 'n' invalido (use 1 ou 2)\n");
//     return;
//   }
//   uint8_t i = (uint8_t)(n - 1);

//   int est = lerEstado(server.arg("s"));
//   if (est == -2)
//   {
//     server.send(400, "text/plain", "parametro 's' invalido (on|off|toggle)\n");
//     return;
//   }

//   bool alvo = (est == -1) ? !lampada[i] : (bool)est;
//   Serial.printf("[HTTP] /lamp n=%d s=%s\n", n, server.arg("s").c_str());
//   comandarLampada(i, alvo);
//   httpStatus();
// }

// void httpMaster()
// {
//   if (!exigirAuth()) return;
//   String s = server.hasArg("s") ? server.arg("s") : "toggle";
//   int est = lerEstado(s);
//   if (est == -2)
//   {
//     server.send(400, "text/plain", "parametro 's' invalido (on|off|toggle)\n");
//     return;
//   }

//   bool alvo;
//   if (est == -1)
//   {
//     bool algumAceso = lampada[0] || lampada[1] || lampRemota[0] || lampRemota[1];
//     alvo = !algumAceso;
//   }
//   else
//   {
//     alvo = (bool)est;
//   }

//   Serial.printf("[HTTP] /master s=%s\n", s.c_str());
//   comandarMaster(alvo);
//   httpStatus();
// }

// void httpSync()
// {
//   if (!exigirAuth()) return;
//   String s = server.hasArg("s") ? server.arg("s") : "on";
//   int est = lerEstado(s);

//   if (est == 1)
//   {
//     Serial.println("[HTTP] /sync on");
//     entrarEmSync();
//   }
//   else if (est == 0)
//   {
//     Serial.println("[HTTP] /sync off");
//     modoSync = false;
//     Serial.println("[SYNC] Sincronismo encerrado (via HTTP).");
//   }
//   else
//   {
//     server.send(400, "text/plain", "parametro 's' invalido (on|off)\n");
//     return;
//   }
//   httpStatus();
// }

// // --- Configuracao: GET devolve a atual; POST valida, salva e reinicia. ---
// void httpGetConfig()
// {
//   if (!exigirAuth()) return;
//   char buf[512];
//   snprintf(buf, sizeof(buf),
//            "{\"modulo\":%d,\"grupo\":%d,\"hostname\":\"%s\","
//            "\"canal\":%d,\"usarBssid\":%s,\"bssid\":\"%s\","
//            "\"msDebounce\":%u,\"msPressaoLonga\":%u,\"msTimeoutSync\":%u,"
//            "\"msHeartbeat\":%u,\"nRepeticoes\":%u,\"msBurstGap\":%u,"
//            "\"msWifiCheck\":%lu,\"msWifiReset\":%lu}",
//            cfg.modulo, cfg.grupo, cfg.hostname,
//            cfg.canal, cfg.usarBssid ? "true" : "false",
//            bssidParaStr(cfg.bssid).c_str(),
//            cfg.msDebounce, cfg.msPressaoLonga, cfg.msTimeoutSync,
//            cfg.msHeartbeat, cfg.nRepeticoes, cfg.msBurstGap,
//            (unsigned long)cfg.msWifiCheck, (unsigned long)cfg.msWifiReset);
//   server.send(200, "application/json", buf);
// }

// // Helper: le um inteiro do POST, com clamp entre [lo,hi] e valor de fallback.
// static long argInt(const char *nome, long lo, long hi, long fallback)
// {
//   if (!server.hasArg(nome)) return fallback;
//   long v = server.arg(nome).toInt();
//   if (v < lo) v = lo;
//   if (v > hi) v = hi;
//   return v;
// }

// void httpPostConfig()
// {
//   if (!exigirAuth()) return;
//   // Parte de uma copia da config atual e sobrescreve o que veio no POST.
//   Config nova = cfg;
//   nova.magic = CFG_MAGIC;

//   nova.modulo = (uint8_t)argInt("modulo", 1, 2, cfg.modulo);
//   nova.grupo  = (uint8_t)argInt("grupo",  1, 255, cfg.grupo);
//   nova.canal  = (uint8_t)argInt("canal",  0, 13, cfg.canal);

//   // Hostname: sanitiza (letras, numeros, hifen; 1..31 chars).
//   if (server.hasArg("hostname"))
//   {
//     String h = server.arg("hostname");
//     h.trim();
//     bool ok = h.length() >= 1 && h.length() <= 31;
//     for (size_t i = 0; ok && i < h.length(); i++)
//     {
//       char c = h[i];
//       ok = isAlphaNumeric(c) || c == '-';
//     }
//     if (!ok)
//     {
//       server.send(400, "text/plain", "hostname invalido");
//       return;
//     }
//     strncpy(nova.hostname, h.c_str(), sizeof(nova.hostname) - 1);
//     nova.hostname[sizeof(nova.hostname) - 1] = '\0';
//   }

//   // BSSID fixo (opcional).
//   nova.usarBssid = (argInt("usarBssid", 0, 1, cfg.usarBssid ? 1 : 0) == 1);
//   if (nova.usarBssid)
//   {
//     if (!server.hasArg("bssid") || !parseBssid(server.arg("bssid"), nova.bssid))
//     {
//       server.send(400, "text/plain", "bssid invalido");
//       return;
//     }
//   }

//   // Tempos.
//   nova.msDebounce     = (uint16_t)argInt("msDebounce",      10,   500,     cfg.msDebounce);
//   nova.msPressaoLonga = (uint16_t)argInt("msPressaoLonga",  300,  5000,    cfg.msPressaoLonga);
//   nova.msTimeoutSync  = (uint16_t)argInt("msTimeoutSync",   1000, 60000,   cfg.msTimeoutSync);
//   nova.msHeartbeat    = (uint16_t)argInt("msHeartbeat",     100,  5000,    cfg.msHeartbeat);
//   nova.nRepeticoes    = (uint8_t) argInt("nRepeticoes",     1,    10,      cfg.nRepeticoes);
//   nova.msBurstGap     = (uint16_t)argInt("msBurstGap",      0,    100,     cfg.msBurstGap);
//   nova.msWifiCheck    = (uint32_t)argInt("msWifiCheck",     1000, 60000,   cfg.msWifiCheck);
//   nova.msWifiReset    = (uint32_t)argInt("msWifiReset",     30000,3600000, cfg.msWifiReset);

//   if (!salvarConfig(nova))
//   {
//     server.send(500, "text/plain", "falha ao gravar NVS");
//     return;
//   }
//   cfg = nova;

//   Serial.println("[CFG] Nova configuracao salva. Reinicio agendado.");
//   server.send(200, "application/json", "{\"ok\":true}");

//   tReiniciar = millis() + 400;    // reinicia apos a resposta sair (aplica tudo)
// }

// void httpNaoEncontrado()
// {
//   server.send(404, "text/plain", "recurso nao encontrado\n");
// }

// /* ==========================================================================
//  *  PORTAL DE CONFIGURACAO WiFi (rotas ativas SOMENTE no modo AP)
//  * ========================================================================== */
// void configurarRotasPortal()
// {
//   server.on("/",          httpPortalRaiz);
//   server.on("/style.css", httpEstilo);        // reaproveita o CSS do app
//   server.on("/scan",      httpPortalScan);
//   server.on("/save",      HTTP_POST, httpPortalSave);
//   server.onNotFound(httpPortalCaptive);       // captive -> tudo cai no portal
// }

// void httpPortalRaiz()
// {
//   server.sendHeader("Cache-Control", "no-cache");
//   server.send_P(200, "text/html; charset=utf-8", WEB_PORTAL_HTML);
// }

// // Redireciona qualquer host/rota para o portal (dispara o "captive portal").
// void httpPortalCaptive()
// {
//   server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
//   server.send(302, "text/plain", "");
// }

// // Lista de redes. ?force=1 reinicia a varredura. Enquanto varre: {status:scanning}.
// void httpPortalScan()
// {
//   if (server.hasArg("force"))
//   {
//     WiFi.scanDelete();
//     WiFi.scanNetworks(true);
//     server.send(200, "application/json", "{\"status\":\"scanning\"}");
//     return;
//   }

//   int n = WiFi.scanComplete();
//   if (n == WIFI_SCAN_FAILED)         // nenhuma varredura iniciada ainda
//   {
//     WiFi.scanNetworks(true);
//     server.send(200, "application/json", "{\"status\":\"scanning\"}");
//     return;
//   }
//   if (n == WIFI_SCAN_RUNNING)
//   {
//     server.send(200, "application/json", "{\"status\":\"scanning\"}");
//     return;
//   }

//   String json = "{\"status\":\"ok\",\"nets\":[";
//   for (int i = 0; i < n; i++)
//   {
//     if (i) json += ",";
//     json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",";
//     json += "\"rssi\":"    + String(WiFi.RSSI(i)) + ",";
//     json += "\"lock\":"    + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 0 : 1) + "}";
//   }
//   json += "]}";
//   server.send(200, "application/json", json);
//   // Mantem os resultados; a UI pede ?force=1 para atualizar.
// }

// // Salva as credenciais WiFi e agenda reinicio para conectar na rede escolhida.
// void httpPortalSave()
// {
//   String ssid  = server.hasArg("ssid")  ? server.arg("ssid")  : "";
//   String senha = server.hasArg("senha") ? server.arg("senha") : "";
//   ssid.trim();

//   if (ssid.length() < 1 || ssid.length() > 32)
//   {
//     server.send(400, "text/plain", "SSID invalido (1..32).");
//     return;
//   }
//   if (senha.length() > 64)
//   {
//     server.send(400, "text/plain", "Senha muito longa (max 64).");
//     return;
//   }

//   Config nova = cfg;
//   strncpy(nova.wifiSsid,  ssid.c_str(),  sizeof(nova.wifiSsid)  - 1);
//   nova.wifiSsid[sizeof(nova.wifiSsid) - 1]   = '\0';
//   strncpy(nova.wifiSenha, senha.c_str(), sizeof(nova.wifiSenha) - 1);
//   nova.wifiSenha[sizeof(nova.wifiSenha) - 1] = '\0';

//   if (!salvarConfig(nova))
//   {
//     server.send(500, "text/plain", "Falha ao gravar NVS.");
//     return;
//   }
//   cfg = nova;

//   Serial.printf("[AP] Credenciais salvas para \"%s\". Reiniciando...\n", nova.wifiSsid);
//   server.send(200, "application/json", "{\"ok\":true}");
//   tReiniciar = millis() + 600;    // reinicia -> tenta conectar na rede nova
// }
