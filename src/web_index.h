#pragma once
#include <Arduino.h>

/* ============================================================================
 *  web_index.h  —  Estrutura da pagina (servida em "/")
 *  ----------------------------------------------------------------------------
 *  Somente a marcacao. O estilo vem de /style.css (web_style.h) e o
 *  comportamento de /app.js (web_script.h). Assim o HTML fica legivel e cada
 *  responsabilidade mora em um arquivo. Todos os caminhos sao relativos, entao
 *  a pagina funciona tanto por IP quanto pelo hostname mDNS.
 * ========================================================================== */

static const char WEB_INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
  <meta name="theme-color" content="#0b0d10">
  <title>Interruptor</title>
  <link rel="stylesheet" href="/style.css">
</head>
<body>
<div class="app">

  <!-- Cabecalho -->
  <header>
    <div class="titulo">
      <span class="k">interruptor</span>
      <span class="m">Módulo&nbsp;<b id="modNum">–</b></span>
    </div>
    <div class="conn" id="conn"><span class="dot"></span><span id="connTxt">conectando…</span></div>
  </header>

  <!-- Lampadas -->
  <div class="lamps">
    <div class="lamp" id="lamp0" onclick="toggleLamp(1)">
      <div class="glow"></div>
      <svg class="bulb" viewBox="0 0 24 24" fill="none">
        <path class="glass" d="M12 2.5a6.5 6.5 0 0 0-3.8 11.8c.6.45.9.9.9 1.6V17h5.8v-1.1c0-.7.3-1.15.9-1.6A6.5 6.5 0 0 0 12 2.5Z" fill="#171b21"/>
        <path class="fil" d="M10 9.5c.8 1 3.2 1 4 0" stroke-width="1.1" stroke-linecap="round"/>
        <path d="M9.4 19h5.2M10 21h4" stroke="#39414c" stroke-width="1.3" stroke-linecap="round"/>
      </svg>
      <div class="row">
        <span class="name">Lâmpada 1</span>
        <span class="state">off</span>
      </div>
    </div>

    <div class="lamp" id="lamp1" onclick="toggleLamp(2)">
      <div class="glow"></div>
      <svg class="bulb" viewBox="0 0 24 24" fill="none">
        <path class="glass" d="M12 2.5a6.5 6.5 0 0 0-3.8 11.8c.6.45.9.9.9 1.6V17h5.8v-1.1c0-.7.3-1.15.9-1.6A6.5 6.5 0 0 0 12 2.5Z" fill="#171b21"/>
        <path class="fil" d="M10 9.5c.8 1 3.2 1 4 0" stroke-width="1.1" stroke-linecap="round"/>
        <path d="M9.4 19h5.2M10 21h4" stroke="#39414c" stroke-width="1.3" stroke-linecap="round"/>
      </svg>
      <div class="row">
        <span class="name">Lâmpada 2</span>
        <span class="state">off</span>
      </div>
    </div>
  </div>

  <!-- Acoes gerais -->
  <div class="master">
    <button class="btn acender" onclick="master('on')">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><path d="M9 18h6M10 21h4"/><path d="M12 3a6 6 0 0 0-4 10.5c.6.6 1 1.2 1 2V17h6v-1.5c0-.8.4-1.4 1-2A6 6 0 0 0 12 3Z" fill="currentColor" fill-opacity=".18"/></svg>
      Acender tudo
    </button>
    <button class="btn apagar" onclick="master('off')">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><path d="M9 18h6M10 21h4"/><path d="M12 3a6 6 0 0 0-4 10.5c.6.6 1 1.2 1 2V17h6v-1.5c0-.8.4-1.4 1-2A6 6 0 0 0 12 3Z"/></svg>
      Apagar tudo
    </button>
  </div>

  <!-- Sincronismo -->
  <div class="sync">
    <div class="info">
      <div class="t">Sincronismo</div>
      <div class="d" id="syncDesc">Espelha os toques neste e no outro módulo.</div>
    </div>
    <div class="switch" id="syncSw" onclick="toggleSync()"></div>
  </div>

  <!-- Estatisticas de rede -->
  <details class="painel">
    <summary>Estatísticas de rede</summary>
    <div class="net-grid">
      <div><span>Hostname</span><b id="infoHost">–</b></div>
      <div><span>Par / grupo</span><b id="infoGrupo">–</b></div>
      <div><span>IP local</span><b id="infoIp">–</b></div>
      <div><span>BSSID (roteador)</span><b id="infoBssid">–</b></div>
      <div><span>MAC (módulo)</span><b id="infoMac">–</b></div>
      <div><span>Sinal (RSSI)</span><b id="infoRssi">–</b></div>
    </div>
  </details>

  <!-- Configuracoes (salvas em Preferences / NVS) -->
  <details class="painel" id="painelCfg">
    <summary>⚙ Configurações</summary>
    <div class="form">

      <div class="grupo-campos">
        <div class="subtitulo">Identidade do módulo</div>

        <div class="campo">
          <label for="cfgModulo">Módulo (dentro do par)</label>
          <select id="cfgModulo">
            <option value="1">1</option>
            <option value="2">2</option>
          </select>
          <span class="dica">Um par tem exatamente um módulo 1 e um módulo 2.</span>
        </div>

        <div class="campo">
          <label for="cfgGrupo">Par / grupo (1–255)</label>
          <input type="number" id="cfgGrupo" min="1" max="255" step="1">
          <span class="dica">Só conversam módulos com o MESMO grupo. Use um número diferente para cada par na rede.</span>
        </div>

        <div class="campo">
          <label for="cfgHostname">Hostname (.local)</label>
          <input type="text" id="cfgHostname" maxlength="31" placeholder="interruptor-sala1">
          <span class="dica">Letras, números e hífen. Acesso via http://&lt;hostname&gt;.local</span>
        </div>
      </div>

      <div class="grupo-campos">
        <div class="subtitulo">Rede / ESP-NOW</div>

        <div class="campo">
          <label for="cfgCanal">Canal WiFi (0 = automático, 1–13)</label>
          <input type="number" id="cfgCanal" min="0" max="13" step="1">
          <span class="dica">O ESP-NOW só cruza no mesmo canal. Fixe o canal do seu AP nos dois módulos.</span>
        </div>

        <div class="campo campo-inline">
          <label for="cfgUsarBssid">Prender a um AP fixo (BSSID)</label>
          <div class="switch" id="cfgBssidSw" onclick="toggleBssid()"></div>
        </div>

        <div class="campo">
          <label for="cfgBssid">BSSID do AP</label>
          <input type="text" id="cfgBssid" maxlength="17" placeholder="B0:1F:8C:54:47:03">
        </div>
      </div>

      <div class="grupo-campos">
        <div class="subtitulo">Tempos (ms)</div>
        <div class="grade-2">
          <div class="campo">
            <label for="cfgDebounce">Debounce</label>
            <input type="number" id="cfgDebounce" min="10" max="500" step="5">
          </div>
          <div class="campo">
            <label for="cfgPressaoLonga">Pressão longa</label>
            <input type="number" id="cfgPressaoLonga" min="300" max="5000" step="50">
          </div>
          <div class="campo">
            <label for="cfgTimeoutSync">Timeout sync</label>
            <input type="number" id="cfgTimeoutSync" min="1000" max="60000" step="500">
          </div>
          <div class="campo">
            <label for="cfgHeartbeat">Heartbeat</label>
            <input type="number" id="cfgHeartbeat" min="100" max="5000" step="50">
          </div>
          <div class="campo">
            <label for="cfgRepeticoes">Repetições (burst)</label>
            <input type="number" id="cfgRepeticoes" min="1" max="10" step="1">
          </div>
          <div class="campo">
            <label for="cfgBurstGap">Gap do burst</label>
            <input type="number" id="cfgBurstGap" min="0" max="100" step="1">
          </div>
          <div class="campo">
            <label for="cfgWifiCheck">Checagem WiFi</label>
            <input type="number" id="cfgWifiCheck" min="1000" max="60000" step="500">
          </div>
          <div class="campo">
            <label for="cfgWifiReset">Reset s/ WiFi</label>
            <input type="number" id="cfgWifiReset" min="30000" max="3600000" step="10000">
          </div>
        </div>
      </div>

      <button class="btn-salvar" id="btnSalvar" onclick="salvarConfig()">Salvar e reiniciar</button>
      <div class="cfg-msg" id="cfgMsg"></div>
    </div>
  </details>

  <!-- Rodape -->
  <footer>
    <span>canal <b id="canal">–</b></span>
    <span>outro módulo <b id="remota">–</b></span>
  </footer>

</div>

<script src="/app.js"></script>
</body>
</html>
)HTML";
