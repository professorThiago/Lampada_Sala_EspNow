#pragma once
#include <Arduino.h>

/* ============================================================================
 *  web_portal.h  —  Portal de configuracao WiFi (ativo so no modo AP)
 *  ----------------------------------------------------------------------------
 *  Pagina servida em "/" quando o modulo sobe como SoftAP (falha de conexao ou
 *  BOOT segurado por 10s). Reaproveita /style.css do app e traz apenas o
 *  necessario: varredura de redes (/scan), selecao e envio das credenciais
 *  (/save). Sem controle de lampada nem ESP-NOW aqui.
 * ========================================================================== */

static const char WEB_PORTAL_HTML[] PROGMEM = R"PORTAL(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
  <meta name="theme-color" content="#0b0d10">
  <title>Configurar Wi-Fi</title>
  <link rel="stylesheet" href="/style.css">
  <style>
    .toolbar{ display:flex; align-items:center; justify-content:space-between; gap:10px; }
    .link{ background:none; border:none; color:var(--warm); font-size:13px; cursor:pointer; padding:6px 2px; font-family:inherit; }
    .netlist{ display:flex; flex-direction:column; gap:8px; max-height:38dvh; overflow-y:auto; margin-top:2px; }
    .net{
      display:flex; align-items:center; justify-content:space-between; gap:10px;
      padding:11px 13px; border:1px solid var(--line); border-radius:12px;
      background:var(--bg); cursor:pointer; transition:border-color .2s;
    }
    .net:hover{ border-color:var(--warm-deep); }
    .net .ss{ font-size:14px; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
    .net .rs{ font-size:12px; color:var(--txt-dim); flex:none; display:flex; gap:6px; align-items:center; }
    .mostrar{ display:flex; align-items:center; gap:8px; font-size:12px; color:var(--txt-dim); cursor:pointer; }
    .mostrar input{ width:auto; }
  </style>
</head>
<body>
<div class="app">

  <header>
    <div class="titulo">
      <span class="k">interruptor</span>
      <span class="m">Configurar&nbsp;<b>Wi-Fi</b></span>
    </div>
  </header>

  <div class="painel">
    <div class="form">

      <div class="toolbar">
        <div class="subtitulo" style="margin:0">Redes disponíveis</div>
        <button class="link" onclick="scan(true)">Atualizar</button>
      </div>
      <div id="nets" class="netlist"></div>
      <div class="dica" id="hint">Procurando redes…</div>

      <div class="campo">
        <label for="ssid">Rede (SSID)</label>
        <input type="text" id="ssid" maxlength="32" placeholder="nome da rede">
      </div>

      <div class="campo">
        <label for="senha">Senha</label>
        <input type="password" id="senha" maxlength="64" placeholder="senha da rede">
        <label class="mostrar"><input type="checkbox" onclick="mostrarSenha(this)"> mostrar senha</label>
      </div>

      <button class="btn-salvar" id="btnSalvar" onclick="salvar()">Salvar e conectar</button>
      <div class="cfg-msg" id="msg"></div>
      <div class="dica" style="text-align:center">
        O aparelho vai reiniciar e tentar conectar. Se falhar, este portal volta.
      </div>

    </div>
  </div>

</div>

<script>
"use strict";

function escapeHtml(s){
  return (s||"").replace(/[&<>"']/g, c => ({
    "&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;","'":"&#39;"
  }[c]));
}
function msg(t, tipo){
  const e = document.getElementById("msg");
  e.textContent = t;
  e.className = "cfg-msg" + (tipo ? " "+tipo : "");
}
function sinal(r){ return r >= -60 ? "forte" : r >= -72 ? "médio" : "fraco"; }
function mostrarSenha(cb){ document.getElementById("senha").type = cb.checked ? "text" : "password"; }

let aguardando = false;
async function scan(force){
  try{
    const r = await fetch("/scan" + (force ? "?force=1" : ""), {cache:"no-store"});
    const d = await r.json();
    if(d.status !== "ok"){                       // ainda varrendo -> tenta de novo
      document.getElementById("hint").textContent = "Procurando redes…";
      if(!aguardando){ aguardando = true; setTimeout(()=>{ aguardando = false; scan(false); }, 1500); }
      return;
    }
    render(d.nets || []);
  }catch(e){
    document.getElementById("hint").textContent = "Falha ao buscar redes.";
  }
}

function render(nets){
  nets.sort((a,b) => b.rssi - a.rssi);
  const vistos = new Set(), uniq = [];        // remove SSIDs repetidos (fica o mais forte)
  nets.forEach(n => { if(n.ssid && !vistos.has(n.ssid)){ vistos.add(n.ssid); uniq.push(n); } });

  const box = document.getElementById("nets");
  box.innerHTML = "";
  uniq.forEach(n => {
    const el = document.createElement("div");
    el.className = "net";
    el.innerHTML = `<span class="ss">${escapeHtml(n.ssid)}</span>` +
                   `<span class="rs">${n.lock ? "🔒 " : ""}${sinal(n.rssi)}</span>`;
    el.onclick = () => {
      document.getElementById("ssid").value = n.ssid;
      document.getElementById("senha").focus();
    };
    box.appendChild(el);
  });
  document.getElementById("hint").textContent =
    uniq.length ? "Toque numa rede para selecionar." : "Nenhuma rede encontrada.";
}

async function salvar(){
  const ssid = document.getElementById("ssid").value.trim();
  if(!ssid) return msg("Informe o nome da rede (SSID).", "erro");

  const p = new URLSearchParams();
  p.set("ssid", ssid);
  p.set("senha", document.getElementById("senha").value);

  document.getElementById("btnSalvar").disabled = true;
  msg("Salvando…");
  try{
    const r = await fetch("/save", {
      method:"POST",
      headers:{"Content-Type":"application/x-www-form-urlencoded"},
      body:p.toString()
    });
    if(!r.ok) throw new Error((await r.text()) || ("HTTP " + r.status));
    msg("Salvo! Reiniciando e conectando a “" + ssid + "”…", "ok");
  }catch(e){
    document.getElementById("btnSalvar").disabled = false;
    msg("Erro: " + e.message, "erro");
  }
}

scan(false);
</script>
</body>
</html>
)PORTAL";
