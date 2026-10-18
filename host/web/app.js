/* ============================================================
   ESP32 信息节点 · 上位机面板逻辑
   数据来源：网关 SSE /api/events（断线自动降级为 1 Hz 轮询）
   ============================================================ */
'use strict';

const OUTPUT_PINS = [2, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33];
const INPUT_ONLY = [34, 35, 36, 39];

const $ = (id) => document.getElementById(id);

const state = {
  source: 'mock',
  snap: null,
  es: null,
  pollTimer: null,
  pins: new Map(),     // pin -> {root, lvl}
  lastScanTs: null,
  lastRawLen: -1,
};

/* ---------------- 工具 ---------------- */
const bytes = (n) => {
  if (n === undefined || n === null) return '—';
  if (n >= 1048576) return (n / 1048576).toFixed(2) + ' MB';
  if (n >= 1024) return (n / 1024).toFixed(1) + ' KB';
  return n + ' B';
};

const uptime = (s) => {
  if (s === undefined || s === null || s === '') return '—';
  s = Math.max(0, Math.floor(s));
  const d = Math.floor(s / 86400);
  const h = String(Math.floor((s % 86400) / 3600)).padStart(2, '0');
  const m = String(Math.floor((s % 3600) / 60)).padStart(2, '0');
  const sec = String(s % 60).padStart(2, '0');
  return (d ? d + 'd ' : '') + `${h}:${m}:${sec}`;
};

const rssiClass = (r) => (r >= -60 ? 'ok' : r >= -72 ? 'warn' : 'err');
// ESP32 开 WiFi 后最大连续块本来就只有空闲堆的 ~45%（约 110KB / 250KB），
// 所以 frag 的绿色区间是 ≤65%，不是 0 —— 早期按 ≤8% 判绿会把正常板子标红。
const fragClass = (f) => (f <= 65 ? 'ok' : f <= 80 ? 'warn' : 'err');
const heapClass = (h) => (h >= 150000 ? 'ok' : h >= 80000 ? 'warn' : 'err');

function setItems(el, items) {
  el.innerHTML = items
    .map((it) => {
      const cls = it.cls ? ` ${it.cls}` : '';
      const big = it.big ? ' big' : '';
      const title = it.title ? ` title="${String(it.title).replace(/"/g, '&quot;')}"` : '';
      return `<div class="item"><div class="k">${it.k}</div><div class="v${cls}${big}"${title}>${it.v ?? '—'}</div></div>`;
    })
    .join('');
}

/* ---------------- 状态渲染 ---------------- */
function render(snap) {
  state.snap = snap;
  const dev = snap.device || {};
  const net = snap.net || {};
  const rt = snap.runtime || {};
  const io = snap.io || { gpio: {}, adc: {} };

  // 顶栏
  $('connDot').className = 'dot ' + (snap.connected ? 'on' : snap.source === 'none' ? '' : 'err');
  $('connState').textContent = snap.connected ? '已连接' : snap.source === 'none' ? '未连接' : '等待数据';
  $('connState').className = snap.connected ? 'ok' : 'warn';
  const srcNames = { serial: '串口', wifi: 'WiFi', mock: '模拟器', none: '未连接' };
  const srcDetail = snap.source === 'serial' ? ' ' + (snap.port || '') : snap.source === 'wifi' ? ' ' + (snap.host || '') : '';
  $('srcLabel').textContent = (srcNames[snap.source] || snap.source) + srcDetail;
  $('gwVersion').textContent = '网关 v' + (snap.gateway?.version || '—');
  $('gwUrl').textContent = location.host;
  $('topRssi').textContent = rt.rssi || net.rssi ? (rt.rssi ?? net.rssi) + ' dBm' : '—';
  $('topUptime').textContent = uptime(rt.up);

  // 芯片
  $('tagReset').textContent = dev.reset ? '复位 ' + dev.reset : '—';
  $('tagReset').className = 'tag' + (dev.reset === 'poweron' ? ' ok' : dev.reset && dev.reset !== 'sw' ? ' warn' : '');
  setItems($('cardDevice'), [
    { k: '型号', v: dev.model, big: true },
    { k: '固件', v: dev.fw },
    { k: '编译时间', v: dev.build, title: dev.build },
    { k: '核心 / 修订', v: dev.cores ? `${dev.cores} 核 · rev ${dev.rev}` : '—' },
    { k: '主频', v: dev.freq ? dev.freq + ' MHz' : '—' },
    { k: 'Flash', v: dev.flash_kb ? dev.flash_kb / 1024 + ' MB' : '—' },
    { k: 'PSRAM', v: dev.psram_kb === 0 ? '无' : dev.psram_kb ? dev.psram_kb / 1024 + ' MB' : '—', cls: 'dim' },
    { k: 'SDK', v: dev.sdk, title: dev.sdk },
    { k: 'MAC', v: dev.mac },
    { k: 'Chip ID', v: dev.chip_id },
    {
      k: '特性',
      v: (dev.features || []).join(' · '),
      cls: 'dim',
    },
  ]);

  // 网络
  const netStateMap = {
    connected: ['已连网', 'ok'], connecting: ['连接中', 'warn'], ap_listening: ['AP 热点', 'ok'],
    disconnected: ['已断开', 'err'], failed: ['连接失败', 'err'], no_ssid: ['找不到 SSID', 'err'], off: ['射频关闭', 'dim'],
  };
  const ns = netStateMap[net.state] || ['—', ''];
  $('tagNetState').textContent = ns[0];
  $('tagNetState').className = 'tag ' + ns[1];
  setItems($('cardNet'), [
    { k: '模式', v: net.mode ? String(net.mode).toUpperCase() : '—' },
    { k: 'SSID', v: net.ssid || '—', big: true },
    { k: 'IP', v: net.ip },
    { k: '网关', v: net.gw },
    { k: '掩码', v: net.mask },
    { k: 'DNS', v: net.dns },
    { k: '信道', v: net.ch || '—' },
    { k: 'RSSI', v: net.rssi ? net.rssi + ' dBm' : '—', cls: net.rssi ? rssiClass(net.rssi) : 'dim' },
    { k: 'BSSID', v: net.bssid || '—', cls: 'dim' },
    { k: '主机名', v: net.host || '—' },
  ]);

  // 运行时
  $('tagHeap').textContent = rt.heap ? bytes(rt.heap) : '—';
  $('tagHeap').className = 'tag' + (rt.heap ? ' ' + heapClass(rt.heap) : '');
  setItems($('cardRuntime'), [
    { k: '运行时长', v: uptime(rt.up) },
    { k: '空闲堆', v: bytes(rt.heap), cls: heapClass(rt.heap), big: true },
    { k: '历史最低', v: bytes(rt.heap_min) },
    { k: '最大块', v: bytes(rt.heap_max) },
    { k: '碎片度', v: rt.frag !== undefined ? rt.frag + ' %' : '—', cls: fragClass(rt.frag ?? 0) },
    { k: '芯片温度', v: rt.temp !== undefined ? rt.temp + ' ℃' : '—' },
    { k: '任务数', v: rt.tasks ?? '—' },
    { k: 'CPU', v: rt.cpu ? rt.cpu + ' MHz' : '—' },
  ]);

  drawSparks(snap.series || []);
  renderPins(io.gpio || {});
  renderAdc(io.adc || {});

  const scan = snap.scan || {};
  if (scan.ts !== undefined && scan.ts !== state.lastScanTs) {
    state.lastScanTs = scan.ts;
    renderScan(scan);
  }

  renderRaw(snap.raw || []);
}

/* ---------------- 迷你曲线 ---------------- */
const sparkBuf = {};
function drawSparks(series) {
  const pick = (key) => series.map((p) => p[key]).filter((v) => typeof v === 'number');
  spark('sparkHeap', pick('heap'), '#58a6ff', (v) => bytes(v));
  spark('sparkTemp', pick('temp'), '#ffa657', (v) => v.toFixed(1) + ' ℃');
  spark('sparkRssi', pick('rssi'), '#7ee787', (v) => v + ' dBm');
}

function spark(id, data, color, fmt) {
  const cv = $(id);
  if (!cv) return;
  const dpr = window.devicePixelRatio || 1;
  const w = cv.clientWidth || 200;
  const h = 46;
  if (cv.width !== w * dpr || cv.height !== h * dpr) {
    cv.width = w * dpr;
    cv.height = h * dpr;
  }
  const ctx = cv.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);

  // 底线
  ctx.strokeStyle = 'rgba(255,255,255,.05)';
  ctx.beginPath();
  ctx.moveTo(0, h - 0.5);
  ctx.lineTo(w, h - 0.5);
  ctx.stroke();

  if (data.length < 2) {
    ctx.fillStyle = '#4a5666';
    ctx.font = '11px ui-monospace, monospace';
    ctx.fillText('采样中…', 2, h / 2);
    return;
  }

  let min = Math.min(...data);
  let max = Math.max(...data);
  if (max - min < 1e-6) { max += 1; min -= 1; }
  const pad = (max - min) * 0.15;
  min -= pad; max += pad;

  const x = (i) => (i / (data.length - 1)) * (w - 2) + 1;
  const y = (v) => h - 4 - ((v - min) / (max - min)) * (h - 10);

  // 渐变填充
  const grad = ctx.createLinearGradient(0, 0, 0, h);
  grad.addColorStop(0, hexA(color, 0.28));
  grad.addColorStop(1, hexA(color, 0));
  ctx.beginPath();
  ctx.moveTo(x(0), y(data[0]));
  data.forEach((v, i) => ctx.lineTo(x(i), y(v)));
  ctx.lineTo(x(data.length - 1), h);
  ctx.lineTo(x(0), h);
  ctx.closePath();
  ctx.fillStyle = grad;
  ctx.fill();

  // 折线
  ctx.beginPath();
  data.forEach((v, i) => (i ? ctx.lineTo(x(i), y(v)) : ctx.moveTo(x(i), y(v))));
  ctx.strokeStyle = color;
  ctx.lineWidth = 1.6;
  ctx.stroke();

  // 末值
  const last = data[data.length - 1];
  ctx.fillStyle = color;
  ctx.beginPath();
  ctx.arc(x(data.length - 1), y(last), 2.4, 0, Math.PI * 2);
  ctx.fill();

  ctx.fillStyle = '#8b98a9';
  ctx.font = '10px ui-monospace, monospace';
  ctx.fillText(fmt(last), 1, 10);
}

function hexA(hex, a) {
  const n = parseInt(hex.slice(1), 16);
  return `rgba(${(n >> 16) & 255},${(n >> 8) & 255},${n & 255},${a})`;
}

/* ---------------- IO ---------------- */
function renderPins(gpio) {
  const box = $('pinGrid');
  const pins = Object.keys(gpio).map(Number).sort((a, b) => a - b);
  if (state.pins.size !== pins.length) {
    box.innerHTML = '';
    state.pins.clear();
    pins.forEach((p) => {
      const inputOnly = INPUT_ONLY.includes(p);
      const el = document.createElement('div');
      el.className = 'pin' + (inputOnly ? ' input-only' : '');
      el.innerHTML = `<div class="p">GPIO ${p}${inputOnly ? ' · in' : ''}</div><div class="lvl lo">0</div>`;
      if (!inputOnly) {
        el.addEventListener('click', () => togglePin(p));
        el.title = '点击切换电平';
      }
      box.appendChild(el);
      state.pins.set(p, { el, lvl: el.querySelector('.lvl') });
    });
  }
  pins.forEach((p) => {
    const ref = state.pins.get(p);
    if (!ref) return;
    const v = gpio[p] ? 1 : 0;
    ref.lvl.textContent = v ? 'HIGH' : 'LOW';
    ref.lvl.className = 'lvl ' + (v ? 'hi' : 'lo');
    ref.el.classList.toggle('on', !!v);
  });
}

async function togglePin(pin) {
  const cur = state.snap?.io?.gpio?.[pin] ? 1 : 0;
  await sendCommand(`gpio.set ${pin} ${cur ? 0 : 1}`, { compact: true });
}

function renderAdc(adc) {
  const box = $('adcBox');
  const keys = Object.keys(adc).sort((a, b) => Number(a) - Number(b));
  if (!keys.length) {
    box.innerHTML = '<div class="row"><div class="p">ADC</div><div class="val dim">无采样</div></div>';
    return;
  }
  box.innerHTML = keys
    .map((k) => {
      const d = adc[k] || {};
      const raw = typeof d === 'object' ? d.raw : d;
      const mv = typeof d === 'object' ? d.mv : null;
      const pct = Math.max(0, Math.min(100, (raw / 4095) * 100));
      return `<div class="row">
        <div class="p">ADC1 · GPIO ${k}</div>
        <div class="val">${raw} <span class="dim">/ 4095${mv !== null ? ` · ${mv} mV` : ''}</span></div>
        <div class="bar"><i style="width:${pct}%"></i></div>
      </div>`;
    })
    .join('');
}

/* ---------------- 扫描表 ---------------- */
function renderScan(scan) {
  const body = $('scanBody');
  const aps = (scan.aps || []).slice().sort((a, b) => b.rssi - a.rssi);
  if (!aps.length) {
    body.innerHTML = '<tr><td colspan="4" class="empty">未扫到 AP</td></tr>';
    return;
  }
  body.innerHTML = aps
    .map((a) => {
      const level = a.rssi >= -60 ? 4 : a.rssi >= -72 ? 3 : a.rssi >= -85 ? 2 : 1;
      const cls = level >= 4 ? 'a' : level >= 2 ? 'b' : 'c';
      const bars = [1, 2, 3, 4].map((i) => `<i class="${i <= level ? cls : ''}" style="height:${3 + i * 2}px"></i>`).join('');
      return `<tr>
        <td title="${a.bssid || ''}">${a.ssid || '<span class="dim">&lt;hidden&gt;</span>'}</td>
        <td><span class="sig">${bars}</span>${a.rssi}</td>
        <td>${a.ch}</td>
        <td>${a.enc || '—'}</td>
      </tr>`;
    })
    .join('');
}

/* ---------------- 原始流 ---------------- */
function renderRaw(raw) {
  const box = $('rawOut');
  if (!raw.length) {
    box.innerHTML = '等待设备数据…';
    return;
  }
  const atBottom = box.scrollTop + box.clientHeight >= box.scrollHeight - 24;
  box.innerHTML = raw
    .slice(-40)
    .map((row) => {
      const d = row.data;
      if (typeof d === 'string') return `<span class="l l-none">${esc(d)}</span>`;
      const t = d.t || '?';
      return `<span class="l l-${t}">${esc(JSON.stringify(d))}</span>`;
    })
    .join('');
  $('lineStats').textContent = `${state.snap?.stats?.lines ?? 0} 行 · 错误 ${state.snap?.stats?.errors ?? 0}`;
  if (atBottom) box.scrollTop = box.scrollHeight;
}

const esc = (s) => String(s).replace(/[&<>]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));

/* ---------------- 连接 ---------------- */
function selectSource(src) {
  state.source = src;
  document.querySelectorAll('#srcSeg .seg').forEach((b) => b.classList.toggle('active', b.dataset.src === src));
  $('fieldSerial').classList.toggle('hide', src !== 'serial');
  $('fieldBaud').classList.toggle('hide', src !== 'serial');
  $('fieldWifi').classList.toggle('hide', src !== 'wifi');
}

async function loadPorts() {
  try {
    const r = await fetch('/api/ports').then((x) => x.json());
    const sel = $('portSelect');
    if (!r.ports.length) {
      sel.innerHTML = '<option value="">（未发现串口）</option>';
      return;
    }
    sel.innerHTML = r.ports
      .map(
        (p) =>
          `<option value="${p.device}"${p.likely_esp32 ? ' selected' : ''}>${p.device} · ${p.description || '未知设备'}${p.likely_esp32 ? ' ★' : ''}</option>`
      )
      .join('');
  } catch (e) {
    console.warn('枚举串口失败', e);
  }
}

async function connect() {
  const src = state.source;
  const body = { source: src };
  if (src === 'serial') {
    body.port = $('portSelect').value;
    body.baud = Number($('baudSelect').value);
    if (!body.port) { $('ackOut').textContent = '没有可用串口：请插上 ESP32 后点「刷新」'; return; }
  }
  if (src === 'wifi') {
    body.host = $('wifiHost').value.trim();
    if (!body.host) { $('ackOut').textContent = '请填设备 IP，例如 192.168.1.66'; return; }
  }
  $('ackOut').textContent = `正在连接 ${src} …`;
  try {
    const r = await fetch('/api/connect', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    }).then((x) => x.json());
    $('ackOut').textContent = JSON.stringify(r, null, 2);
    startStream();
  } catch (e) {
    $('ackOut').textContent = '连接失败：' + e.message;
  }
}

async function disconnect() {
  try {
    await fetch('/api/disconnect', { method: 'POST' });
    $('ackOut').textContent = '已断开';
  } catch (e) {
    $('ackOut').textContent = '断开失败：' + e.message;
  }
}

/* ---------------- 命令 ---------------- */
async function sendCommand(cmd, opt = {}) {
  if (!cmd || !cmd.trim()) return null;
  if (!opt.silent) $('ackOut').textContent = `→ ${cmd}\n等待设备响应…`;
  try {
    const r = await fetch('/api/cmd', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ cmd, timeout: cmd.startsWith('scan') ? 8 : 5 }),
    }).then((x) => x.json());
    if (!opt.silent) {
      const ack = r.ack;
      const head = ack ? (ack.t === 'err' ? `✗ ${ack.code}：${ack.msg}` : '✓ ack') : '（无 ack：该命令不返回确认）';
      const detail = opt.compact ? JSON.stringify(ack?.data ?? ack) : JSON.stringify(ack ?? r, null, 2);
      $('ackOut').textContent = `→ ${cmd}\n${head}\n${detail}`;
    }
    return r;
  } catch (e) {
    if (!opt.silent) $('ackOut').textContent = `→ ${cmd}\n发送失败：${e.message}`;
    return null;
  }
}

/* ---------------- 数据流 ---------------- */
function startStream() {
  stopStream();
  try {
    const es = new EventSource('/api/events');
    state.es = es;
    es.addEventListener('snapshot', (ev) => {
      try { render(JSON.parse(ev.data)); } catch (e) { console.warn(e); }
    });
    es.onerror = () => {
      // 网关中途重启/代理断开：退回轮询，仍然可用
      if (state.es === es) {
        es.close();
        state.es = null;
        startPolling();
      }
    };
  } catch (e) {
    startPolling();
  }
}

function startPolling() {
  if (state.pollTimer) return;
  const tick = async () => {
    try {
      const snap = await fetch('/api/status').then((r) => r.json());
      render(snap);
    } catch (e) { /* 网关没起来，下一轮再试 */ }
  };
  tick();
  state.pollTimer = setInterval(tick, 1000);
}

function stopStream() {
  if (state.es) { state.es.close(); state.es = null; }
  if (state.pollTimer) { clearInterval(state.pollTimer); state.pollTimer = null; }
}

/* ---------------- 导出 ---------------- */
function exportSnapshot() {
  if (!state.snap) return;
  const blob = new Blob([JSON.stringify(state.snap, null, 2)], { type: 'application/json' });
  const a = document.createElement('a');
  const ts = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
  a.href = URL.createObjectURL(blob);
  a.download = `esp32-snapshot-${ts}.json`;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 2000);
}

/* ---------------- 事件绑定 ---------------- */
document.querySelectorAll('#srcSeg .seg').forEach((b) => {
  b.addEventListener('click', () => selectSource(b.dataset.src));
});
$('btnRefreshPorts').addEventListener('click', loadPorts);
$('btnConnect').addEventListener('click', connect);
$('btnDisconnect').addEventListener('click', disconnect);
$('btnScan').addEventListener('click', () => sendCommand('scan'));
$('btnExport').addEventListener('click', exportSnapshot);
$('btnSend').addEventListener('click', () => {
  const v = $('cmdInput').value;
  sendCommand(v);
  $('cmdInput').value = '';
});
$('cmdInput').addEventListener('keydown', (e) => {
  if (e.key === 'Enter') {
    sendCommand(e.target.value);
    e.target.value = '';
  }
});

/* 启动 */
(async function boot() {
  await loadPorts();
  // 页面打开时展示网关当前状态，若已连上则直接开流
  try {
    const snap = await fetch('/api/status').then((r) => r.json());
    render(snap);
    if (snap.gateway?.source && snap.gateway.source !== 'none') {
      selectSource(snap.gateway.source);
      if (snap.gateway.source_info?.port) {
        const sel = $('portSelect');
        if (![...sel.options].some((o) => o.value === snap.gateway.source_info.port)) {
          sel.insertAdjacentHTML('afterbegin', `<option>${snap.gateway.source_info.port}</option>`);
        }
        sel.value = snap.gateway.source_info.port;
      }
      if (snap.gateway.source_info?.host) $('wifiHost').value = snap.gateway.source_info.host;
    }
  } catch (e) { /* 忽略 */ }
  startStream();
  setInterval(() => { if (!state.snap) startPolling(); }, 3000);
})();
