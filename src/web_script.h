#pragma once
#include <Arduino.h>

/* ============================================================================
 *  web_script.h  —  Comportamento da pagina (servido em "/app.js")
 *  ----------------------------------------------------------------------------
 *  Toda a logica de UI: leitura de estado (/status), comandos (/lamp,/master,
 *  /sync) e o painel de configuracoes (/config GET+POST). Mantem o "modo demo"
 *  do original: se nao houver ESP respondendo, a pagina simula tudo localmente
 *  para dar para visualizar o layout no navegador.
 * ========================================================================== */

static const char WEB_SCRIPT_JS[] PROGMEM = R"JS(
"use strict";

/* ---- Estado corrente da UI (espelha o /status do modulo) ---- */
const S = {
  modulo:1, grupo:1, hostname:"-",
  lamp1:0, lamp2:0, remota1:0, remota2:0,
  sync:false, canal:0,
  ip:"-", bssid:"-", mac:"-", rssi:0
};
let demo = false;          // vira true na primeira falha de rede
let usarBssid = false;     // switch do formulario de configuracao

/* ============================ CAMADA DE REDE ============================== */
async function api(path, opt){
  if(demo) return mock(path, opt);
  let r;
  try{
    r = await fetch(path, Object.assign({cache:"no-store", credentials:"same-origin"}, opt||{}));
  }catch(e){
    demo = true;                       // falha de REDE (sem ESP) -> preview/demo
    return mock(path, opt);
  }
  // Erros HTTP (ex.: 401 de autenticacao) NAO ligam o modo demo.
  if(!r.ok) throw new Error("HTTP " + r.status);
  return await r.json();
}

/* Simulador local, so para pre-visualizar a interface fora do ESP32 */
function mock(path){
  const u = new URL(path, location.origin);
  const q = u.searchParams;
  if(u.pathname === "/lamp"){
    const i = q.get("n")==="2" ? "lamp2":"lamp1";
    const s = q.get("s");
    S[i] = s==="toggle" ? (S[i]?0:1) : (s==="on"?1:0);
  }else if(u.pathname === "/master"){
    const on = q.get("s")==="on";
    S.lamp1 = S.lamp2 = on?1:0;
    S.remota1 = S.remota2 = on?1:0;
  }else if(u.pathname === "/sync"){
    S.sync = q.get("s")==="on";
  }else if(u.pathname === "/config"){
    return {
      modulo:1, grupo:1, hostname:"interruptor-demo",
      canal:11, usarBssid:true, bssid:"B0:1F:8C:54:47:03",
      msDebounce:50, msPressaoLonga:1000, msTimeoutSync:7000, msHeartbeat:500,
      nRepeticoes:3, msBurstGap:8, msWifiCheck:5000, msWifiReset:300000
    };
  }
  S.canal = S.canal || 6;
  S.hostname = "interruptor-demo";
  S.ip = "192.168.1.100 (demo)";
  S.bssid = "00:11:22:33:44:55";
  S.mac = "AA:BB:CC:DD:EE:FF";
  S.rssi = -55;
  return {...S};
}

/* ============================== RENDER ==================================== */
function render(d){
  Object.assign(S, d, {sync: d.sync===true || d.sync==="true"});

  document.getElementById("modNum").textContent = S.modulo;
  paintLamp(0, S.lamp1);
  paintLamp(1, S.lamp2);

  const sw = document.getElementById("syncSw");
  sw.classList.toggle("on", !!S.sync);
  document.getElementById("syncDesc").textContent = S.sync
    ? "Ligado — toques espelham no outro módulo."
    : "Espelha os toques neste e no outro módulo.";

  document.getElementById("canal").textContent  = S.canal;
  document.getElementById("remota").textContent = (S.remota1? "1":"–") + (S.remota2? " 2":"");

  document.getElementById("infoHost").textContent  = S.hostname || "–";
  document.getElementById("infoGrupo").textContent = S.grupo != null ? S.grupo : "–";
  document.getElementById("infoIp").textContent    = S.ip || "–";
  document.getElementById("infoBssid").textContent = S.bssid || "–";
  document.getElementById("infoMac").textContent   = S.mac || "–";
  document.getElementById("infoRssi").textContent  = S.rssi ? S.rssi + " dBm" : "–";

  const c = document.getElementById("conn");
  c.className = "conn " + (demo ? "demo":"ok");
  document.getElementById("connTxt").textContent = demo ? "modo demo" : "conectado";
}

function paintLamp(idx, on){
  const el = document.getElementById("lamp"+idx);
  const was = el.classList.contains("on");
  el.classList.toggle("on", !!on);
  el.querySelector(".state").textContent = on ? "on" : "off";
  if(!!on !== was){ el.classList.remove("flash"); void el.offsetWidth; el.classList.add("flash"); }
}

/* ============================== ACOES ==================================== */
// Em caso de erro (ex.: login cancelado) o /status seguinte ressincroniza a UI.
async function toggleLamp(n){ try{ render(await api(`/lamp?n=${n}&s=toggle`)); }catch(e){ refresh(); } }
async function master(s){     try{ render(await api(`/master?s=${s}`)); }catch(e){ refresh(); } }
async function toggleSync(){  try{ render(await api(`/sync?s=${S.sync?"off":"on"}`)); }catch(e){ refresh(); } }
async function refresh(){     try{ render(await api("/status")); }catch(e){} }

/* ========================= CONFIGURACOES =================================
 * Mapa explicito  id-do-campo -> nome-do-parametro  esperado pelo firmware.
 * (Preferimos um mapa claro a transformar strings "por esperteza".)         */
const PARAMS = {
  cfgModulo:"modulo",  cfgGrupo:"grupo",  cfgCanal:"canal",
  cfgDebounce:"msDebounce",   cfgPressaoLonga:"msPressaoLonga",
  cfgTimeoutSync:"msTimeoutSync", cfgHeartbeat:"msHeartbeat",
  cfgRepeticoes:"nRepeticoes",    cfgBurstGap:"msBurstGap",
  cfgWifiCheck:"msWifiCheck",     cfgWifiReset:"msWifiReset"
};

function setBssidSwitch(estado){
  usarBssid = !!estado;
  document.getElementById("cfgBssidSw").classList.toggle("on", usarBssid);
  document.getElementById("cfgBssid").disabled = !usarBssid;
}
function toggleBssid(){ setBssidSwitch(!usarBssid); }

async function carregarConfig(){
  const c = await api("/config");
  document.getElementById("cfgModulo").value   = c.modulo;
  document.getElementById("cfgHostname").value = c.hostname;
  document.getElementById("cfgBssid").value    = c.bssid || "";
  setBssidSwitch(c.usarBssid);

  const map = {
    cfgGrupo:c.grupo, cfgCanal:c.canal,
    cfgDebounce:c.msDebounce, cfgPressaoLonga:c.msPressaoLonga,
    cfgTimeoutSync:c.msTimeoutSync, cfgHeartbeat:c.msHeartbeat,
    cfgRepeticoes:c.nRepeticoes, cfgBurstGap:c.msBurstGap,
    cfgWifiCheck:c.msWifiCheck, cfgWifiReset:c.msWifiReset
  };
  for(const id in map) document.getElementById(id).value = map[id];
}

function msg(texto, tipo){
  const el = document.getElementById("cfgMsg");
  el.textContent = texto;
  el.className = "cfg-msg" + (tipo ? " "+tipo : "");
}

async function salvarConfig(){
  const hostname = document.getElementById("cfgHostname").value.trim();
  if(!/^[a-zA-Z0-9-]{1,31}$/.test(hostname)){
    return msg("Hostname inválido (use letras, números e hífen).", "erro");
  }
  const bssid = document.getElementById("cfgBssid").value.trim();
  if(usarBssid && !/^([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$/.test(bssid)){
    return msg("BSSID inválido (formato AA:BB:CC:DD:EE:FF).", "erro");
  }

  const p = new URLSearchParams();
  for(const id in PARAMS) p.set(PARAMS[id], document.getElementById(id).value);
  p.set("hostname",  hostname);
  p.set("usarBssid", usarBssid ? "1" : "0");
  p.set("bssid",     bssid);

  const btn = document.getElementById("btnSalvar");
  btn.disabled = true;
  msg("Salvando…");

  try{
    const r = await fetch("/config", {
      method:"POST",
      headers:{"Content-Type":"application/x-www-form-urlencoded"},
      body:p.toString()
    });
    if(!r.ok){ const t = await r.text(); throw new Error(t || "falha"); }
    msg(`Salvo. Reiniciando… reconecte em http://${hostname}.local`, "ok");
  }catch(e){
    btn.disabled = false;
    msg("Erro ao salvar: " + e.message, "erro");
  }
}

/* =============================== BOOT ==================================== */
// A config e carregada SO quando o painel e aberto -> a pagina carrega sem
// pedir senha; o Digest so aparece ao controlar ou ao abrir Configuracoes.
let cfgCarregada = false;
const painelCfg = document.getElementById("painelCfg");
if(painelCfg){
  painelCfg.addEventListener("toggle", function(){
    if(this.open && !cfgCarregada){
      msg("Carregando…");
      carregarConfig()
        .then(()=>{ cfgCarregada = true; msg(""); })
        .catch(()=>{ msg("Faça login para ver/alterar a configuração.", "erro"); });
    }
  });
}

refresh();
setInterval(refresh, 1500);   // reflete botoes fisicos e o outro modulo
)JS";




// #pragma once
// #include <Arduino.h>

// /* ============================================================================
//  *  web_script.h  —  Comportamento da pagina (servido em "/app.js")
//  *  ----------------------------------------------------------------------------
//  *  Toda a logica de UI: leitura de estado (/status), comandos (/lamp,/master,
//  *  /sync) e o painel de configuracoes (/config GET+POST). Mantem o "modo demo"
//  *  do original: se nao houver ESP respondendo, a pagina simula tudo localmente
//  *  para dar para visualizar o layout no navegador.
//  * ========================================================================== */

// static const char WEB_SCRIPT_JS[] PROGMEM = R"JS(
// "use strict";

// /* ---- Estado corrente da UI (espelha o /status do modulo) ---- */
// const S = {
//   modulo:1, grupo:1, hostname:"-",
//   lamp1:0, lamp2:0, remota1:0, remota2:0,
//   sync:false, canal:0,
//   ip:"-", bssid:"-", mac:"-", rssi:0
// };
// let demo = false;          // vira true na primeira falha de rede
// let usarBssid = false;     // switch do formulario de configuracao

// /* ============================ CAMADA DE REDE ============================== */
// async function api(path, opt){
//   if(demo) return mock(path, opt);
//   try{
//     const r = await fetch(path, Object.assign({cache:"no-store"}, opt||{}));
//     if(!r.ok) throw 0;
//     return await r.json();
//   }catch(e){
//     demo = true;             // sem ESP -> passa a simular
//     return mock(path, opt);
//   }
// }

// /* Simulador local, so para pre-visualizar a interface fora do ESP32 */
// function mock(path){
//   const u = new URL(path, location.origin);
//   const q = u.searchParams;
//   if(u.pathname === "/lamp"){
//     const i = q.get("n")==="2" ? "lamp2":"lamp1";
//     const s = q.get("s");
//     S[i] = s==="toggle" ? (S[i]?0:1) : (s==="on"?1:0);
//   }else if(u.pathname === "/master"){
//     const on = q.get("s")==="on";
//     S.lamp1 = S.lamp2 = on?1:0;
//     S.remota1 = S.remota2 = on?1:0;
//   }else if(u.pathname === "/sync"){
//     S.sync = q.get("s")==="on";
//   }else if(u.pathname === "/config"){
//     return {
//       modulo:1, grupo:1, hostname:"interruptor-demo",
//       canal:11, usarBssid:true, bssid:"B0:1F:8C:54:47:03",
//       msDebounce:50, msPressaoLonga:1000, msTimeoutSync:7000, msHeartbeat:500,
//       nRepeticoes:3, msBurstGap:8, msWifiCheck:5000, msWifiReset:300000
//     };
//   }
//   S.canal = S.canal || 6;
//   S.hostname = "interruptor-demo";
//   S.ip = "192.168.1.100 (demo)";
//   S.bssid = "00:11:22:33:44:55";
//   S.mac = "AA:BB:CC:DD:EE:FF";
//   S.rssi = -55;
//   return {...S};
// }

// /* ============================== RENDER ==================================== */
// function render(d){
//   Object.assign(S, d, {sync: d.sync===true || d.sync==="true"});

//   document.getElementById("modNum").textContent = S.modulo;
//   paintLamp(0, S.lamp1);
//   paintLamp(1, S.lamp2);

//   const sw = document.getElementById("syncSw");
//   sw.classList.toggle("on", !!S.sync);
//   document.getElementById("syncDesc").textContent = S.sync
//     ? "Ligado — toques espelham no outro módulo."
//     : "Espelha os toques neste e no outro módulo.";

//   document.getElementById("canal").textContent  = S.canal;
//   document.getElementById("remota").textContent = (S.remota1? "1":"–") + (S.remota2? " 2":"");

//   document.getElementById("infoHost").textContent  = S.hostname || "–";
//   document.getElementById("infoGrupo").textContent = S.grupo != null ? S.grupo : "–";
//   document.getElementById("infoIp").textContent    = S.ip || "–";
//   document.getElementById("infoBssid").textContent = S.bssid || "–";
//   document.getElementById("infoMac").textContent   = S.mac || "–";
//   document.getElementById("infoRssi").textContent  = S.rssi ? S.rssi + " dBm" : "–";

//   const c = document.getElementById("conn");
//   c.className = "conn " + (demo ? "demo":"ok");
//   document.getElementById("connTxt").textContent = demo ? "modo demo" : "conectado";
// }

// function paintLamp(idx, on){
//   const el = document.getElementById("lamp"+idx);
//   const was = el.classList.contains("on");
//   el.classList.toggle("on", !!on);
//   el.querySelector(".state").textContent = on ? "on" : "off";
//   if(!!on !== was){ el.classList.remove("flash"); void el.offsetWidth; el.classList.add("flash"); }
// }

// /* ============================== ACOES ==================================== */
// async function toggleLamp(n){ render(await api(`/lamp?n=${n}&s=toggle`)); }
// async function master(s){     render(await api(`/master?s=${s}`)); }
// async function toggleSync(){  render(await api(`/sync?s=${S.sync?"off":"on"}`)); }
// async function refresh(){     render(await api("/status")); }

// /* ========================= CONFIGURACOES =================================
//  * Mapa explicito  id-do-campo -> nome-do-parametro  esperado pelo firmware.
//  * (Preferimos um mapa claro a transformar strings "por esperteza".)         */
// const PARAMS = {
//   cfgModulo:"modulo",  cfgGrupo:"grupo",  cfgCanal:"canal",
//   cfgDebounce:"msDebounce",   cfgPressaoLonga:"msPressaoLonga",
//   cfgTimeoutSync:"msTimeoutSync", cfgHeartbeat:"msHeartbeat",
//   cfgRepeticoes:"nRepeticoes",    cfgBurstGap:"msBurstGap",
//   cfgWifiCheck:"msWifiCheck",     cfgWifiReset:"msWifiReset"
// };

// function setBssidSwitch(estado){
//   usarBssid = !!estado;
//   document.getElementById("cfgBssidSw").classList.toggle("on", usarBssid);
//   document.getElementById("cfgBssid").disabled = !usarBssid;
// }
// function toggleBssid(){ setBssidSwitch(!usarBssid); }

// async function carregarConfig(){
//   const c = await api("/config");
//   document.getElementById("cfgModulo").value   = c.modulo;
//   document.getElementById("cfgHostname").value = c.hostname;
//   document.getElementById("cfgBssid").value    = c.bssid || "";
//   setBssidSwitch(c.usarBssid);

//   const map = {
//     cfgGrupo:c.grupo, cfgCanal:c.canal,
//     cfgDebounce:c.msDebounce, cfgPressaoLonga:c.msPressaoLonga,
//     cfgTimeoutSync:c.msTimeoutSync, cfgHeartbeat:c.msHeartbeat,
//     cfgRepeticoes:c.nRepeticoes, cfgBurstGap:c.msBurstGap,
//     cfgWifiCheck:c.msWifiCheck, cfgWifiReset:c.msWifiReset
//   };
//   for(const id in map) document.getElementById(id).value = map[id];
// }

// function msg(texto, tipo){
//   const el = document.getElementById("cfgMsg");
//   el.textContent = texto;
//   el.className = "cfg-msg" + (tipo ? " "+tipo : "");
// }

// async function salvarConfig(){
//   const hostname = document.getElementById("cfgHostname").value.trim();
//   if(!/^[a-zA-Z0-9-]{1,31}$/.test(hostname)){
//     return msg("Hostname inválido (use letras, números e hífen).", "erro");
//   }
//   const bssid = document.getElementById("cfgBssid").value.trim();
//   if(usarBssid && !/^([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$/.test(bssid)){
//     return msg("BSSID inválido (formato AA:BB:CC:DD:EE:FF).", "erro");
//   }

//   const p = new URLSearchParams();
//   for(const id in PARAMS) p.set(PARAMS[id], document.getElementById(id).value);
//   p.set("hostname",  hostname);
//   p.set("usarBssid", usarBssid ? "1" : "0");
//   p.set("bssid",     bssid);

//   const btn = document.getElementById("btnSalvar");
//   btn.disabled = true;
//   msg("Salvando…");

//   try{
//     const r = await fetch("/config", {
//       method:"POST",
//       headers:{"Content-Type":"application/x-www-form-urlencoded"},
//       body:p.toString()
//     });
//     if(!r.ok){ const t = await r.text(); throw new Error(t || "falha"); }
//     msg(`Salvo. Reiniciando… reconecte em http://${hostname}.local`, "ok");
//   }catch(e){
//     btn.disabled = false;
//     msg("Erro ao salvar: " + e.message, "erro");
//   }
// }

// /* =============================== BOOT ==================================== */
// refresh();
// carregarConfig();
// setInterval(refresh, 1500);   // reflete botoes fisicos e o outro modulo
// )JS";
