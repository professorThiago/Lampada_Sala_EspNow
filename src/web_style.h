#pragma once
#include <Arduino.h>

/* ============================================================================
 *  web_style.h  —  Folha de estilo da interface (servida em /style.css)
 *  ----------------------------------------------------------------------------
 *  CSS isolado do HTML e do JS. Fica em flash (PROGMEM) e e enviado com
 *  Cache-Control longo: o navegador so baixa uma vez e reutiliza.
 *
 *  Correcao aplicada: o "botao" (knob) do interruptor de sincronismo estava
 *  quebrado no original -> a regra era  content:"absolute;..."  (aspas nao
 *  fechadas), o que engolia position/top/left. Agora e  content:"";  seguido
 *  das propriedades de posicionamento, como deve ser.
 * ========================================================================== */

static const char WEB_STYLE_CSS[] PROGMEM = R"CSS(
:root{
  --bg:#0b0d10; --surf:#161a20; --surf-2:#1e242c; --line:#272e38;
  --txt:#e8ecf1; --txt-dim:#7d8590;
  --warm:#ffc24b; --warm-hi:#fff4d6; --warm-deep:#ff9e2c;
  --on:#3fb950; --off:#f0623a;
  --r:22px;
}

*{ box-sizing:border-box; margin:0; padding:0; }
html{ height:100%; }

body{
  background:radial-gradient(120% 80% at 50% -10%, #12161c 0%, var(--bg) 60%);
  background-attachment:fixed;          /* gradiente fixo mesmo com a pagina rolando */
  color:var(--txt);
  font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
  -webkit-font-smoothing:antialiased;
  min-height:100dvh;                    /* min, nao altura fixa -> cresce com o conteudo */
  display:flex;
  justify-content:center;
  align-items:flex-start;               /* alinha ao topo -> conteudo alto rola normalmente */
  padding:clamp(16px,4vw,28px);
}

.app{ width:100%; max-width:440px; display:flex; flex-direction:column; gap:18px; }

/* ---------- Cabecalho ---------- */
header{ display:flex; align-items:baseline; justify-content:space-between; gap:12px; padding:4px 4px 0; }
.titulo{ display:flex; flex-direction:column; gap:2px; }
.titulo .k{ font-size:13px; color:var(--txt-dim); letter-spacing:.02em; }
.titulo .m{ font-size:34px; font-weight:300; line-height:1; letter-spacing:-.02em; }
.titulo .m b{ font-weight:600; }
.conn{ display:flex; align-items:center; gap:8px; font-size:13px; color:var(--txt-dim); }
.dot{ width:9px; height:9px; border-radius:50%; background:var(--off); transition:.3s; }
.conn.ok   .dot{ background:var(--on); box-shadow:0 0 10px 1px #3fb95088; }
.conn.demo .dot{ background:var(--warm); }

/* ---------- Lampadas ---------- */
.lamps{ display:grid; grid-template-columns:1fr 1fr; gap:14px; }
@media(max-width:360px){ .lamps{ grid-template-columns:1fr; } }

.lamp{
  position:relative; border:1px solid var(--line); border-radius:var(--r);
  background:linear-gradient(180deg,#12161c,#0f1318);
  padding:22px 18px 18px; cursor:pointer; overflow:hidden;
  transition:border-color .25s, transform .06s, background .3s;
  -webkit-tap-highlight-color:transparent; user-select:none;
}
.lamp:active{ transform:scale(.985); }
.lamp .glow{
  position:absolute; inset:-40% -20% auto -20%; height:150%;
  background:radial-gradient(60% 55% at 50% 0%, #ffb43a55, transparent 70%);
  opacity:0; transition:opacity .35s; pointer-events:none;
}
.lamp.on{ border-color:#4a3c1f; background:linear-gradient(180deg,#20180a,#140f07); }
.lamp.on .glow{ opacity:1; }

.bulb{ width:56px; height:56px; display:block; margin:2px auto 14px; position:relative; z-index:1; }
.bulb .glass{ transition:.35s; }
.bulb .fil{ stroke:#3a4049; transition:.35s; }
.lamp.on .bulb .glass{ fill:var(--warm); filter:drop-shadow(0 0 14px #ffb43a); }
.lamp.on .bulb .fil{ stroke:var(--warm-hi); }

.lamp .row{ display:flex; align-items:center; justify-content:space-between; position:relative; z-index:1; }
.lamp .name{ font-size:15px; font-weight:500; }
.lamp .state{ font-size:12px; color:var(--txt-dim); font-variant-numeric:tabular-nums; }
.lamp.on .state{ color:var(--warm); }

/* ---------- Acoes gerais ---------- */
.master{ display:grid; grid-template-columns:1fr 1fr; gap:12px; }
.btn{
  border:1px solid var(--line); background:var(--surf); color:var(--txt);
  border-radius:16px; padding:15px 12px; font-size:15px; font-weight:500;
  cursor:pointer; display:flex; align-items:center; justify-content:center; gap:9px;
  transition:background .2s, border-color .2s, transform .06s;
  -webkit-tap-highlight-color:transparent;
}
.btn:active{ transform:scale(.98); }
.btn svg{ width:18px; height:18px; }
.btn.acender{ border-color:#4a3c1f; }
.btn.acender:hover{ background:#1c1710; }
.btn.acender svg{ color:var(--warm); }
.btn.apagar:hover{ background:#191d23; }

/* ---------- Sincronismo ---------- */
.sync{
  display:flex; align-items:center; gap:14px;
  border:1px solid var(--line); background:var(--surf); border-radius:18px; padding:15px 16px;
}
.sync .info{ flex:1; min-width:0; }
.sync .info .t{ font-size:15px; font-weight:500; }
.sync .info .d{ font-size:12.5px; color:var(--txt-dim); margin-top:2px; line-height:1.35; }

.switch{
  flex:none; width:52px; height:30px; border-radius:16px; background:#2a313b;
  position:relative; cursor:pointer; transition:.25s; border:1px solid var(--line);
}
.switch::after{
  content:""; position:absolute; top:3px; left:3px; width:22px; height:22px;
  border-radius:50%; background:#c7ccd3; transition:.25s;
}
.switch.on{ background:#2b5e33; border-color:#357a3f; }
.switch.on::after{ left:25px; background:#eafff0; }

/* ---------- Paineis recolhiveis (rede + configuracoes) ---------- */
.painel{ border:1px solid var(--line); border-radius:16px; background:var(--surf); overflow:hidden; }
.painel > summary{
  padding:14px 16px; font-size:13px; font-weight:500; color:var(--txt-dim);
  cursor:pointer; list-style:none; text-align:center; transition:background .2s;
}
.painel > summary:hover{ background:var(--surf-2); }
.painel > summary::-webkit-details-marker{ display:none; }

/* ---------- Grade de estatisticas de rede ---------- */
.net-grid{
  display:grid; grid-template-columns:1fr; gap:10px; padding:0 16px 16px;
  font-size:12px; color:var(--txt-dim);
}
.net-grid div{ display:flex; justify-content:space-between; border-bottom:1px dashed var(--line); padding-bottom:4px; }
.net-grid div:last-child{ border-bottom:none; padding-bottom:0; }
.net-grid b{ color:var(--txt); font-weight:500; font-family:monospace; font-size:13px; }

/* ---------- Formulario de configuracoes ---------- */
.form{ padding:4px 16px 16px; display:flex; flex-direction:column; gap:14px; }
.form .grupo-campos{ display:flex; flex-direction:column; gap:10px; }
.form .subtitulo{
  font-size:12px; font-weight:600; color:var(--warm); text-transform:uppercase;
  letter-spacing:.06em; margin-top:6px;
}
.campo{ display:flex; flex-direction:column; gap:5px; }
.campo > label{ font-size:12.5px; color:var(--txt-dim); }
.campo .dica{ font-size:11px; color:#5f6672; }

.campo input[type=text],
.campo input[type=number],
.campo select{
  width:100%; background:var(--bg); color:var(--txt);
  border:1px solid var(--line); border-radius:10px; padding:10px 12px;
  font-size:14px; font-family:inherit; transition:border-color .2s;
}
.campo input:focus, .campo select:focus{ outline:none; border-color:var(--warm-deep); }
.campo input[type=text]{ font-family:monospace; }

/* Linha com switch (usar BSSID fixo) */
.campo-inline{ display:flex; align-items:center; justify-content:space-between; gap:12px; }
.campo-inline label{ font-size:13px; color:var(--txt); }

/* Duas colunas para os tempos (economiza altura) */
.grade-2{ display:grid; grid-template-columns:1fr 1fr; gap:10px; }
@media(max-width:360px){ .grade-2{ grid-template-columns:1fr; } }

.btn-salvar{
  margin-top:4px; width:100%; border:1px solid #357a3f; background:#2b5e33; color:#eafff0;
  border-radius:12px; padding:14px; font-size:15px; font-weight:600; cursor:pointer;
  transition:background .2s, transform .06s;
}
.btn-salvar:hover{ background:#31703b; }
.btn-salvar:active{ transform:scale(.99); }
.btn-salvar:disabled{ opacity:.5; cursor:default; }

.cfg-msg{ font-size:12.5px; text-align:center; min-height:16px; color:var(--txt-dim); }
.cfg-msg.ok{ color:var(--on); }
.cfg-msg.erro{ color:var(--off); }

/* ---------- Rodape ---------- */
footer{ display:flex; justify-content:center; gap:18px; color:var(--txt-dim); font-size:12px; padding:2px 0 4px; }
footer span{ display:inline-flex; align-items:center; gap:6px; }
footer b{ color:var(--txt); font-weight:500; font-variant-numeric:tabular-nums; }

.flash{ animation:flash .4s ease; }
@keyframes flash{ 0%{ filter:brightness(1.4); } 100%{ filter:brightness(1); } }
@media(prefers-reduced-motion:reduce){ *{ transition:none !important; animation:none !important; } }
)CSS";
