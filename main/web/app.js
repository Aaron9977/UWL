/* global WebSocket, document */

const WS_URL = `ws://${location.host || "192.168.4.1"}/ws`;
const API_STATUS_URL = `${location.origin}/api/status`;

let ws = null;
const gpioMap = new Map(); // pin -> {pin,dir,value}
let wsSeq = 1;

// BLE (Web Bluetooth)
const BLE_UUIDS = {
  // Must match main/uwl_ble_gatt.c
  svc: "108e3b72-4838-1c9a-0c4f-2f0d351a2f5f",
  ctrl: "118e3b72-4838-1c9a-0c4f-2f0d351a2f5f",
  state: "128e3b72-4838-1c9a-0c4f-2f0d351a2f5f",
};

let ble = {
  device: null,
  server: null,
  svc: null,
  ctrlChar: null,
  stateChar: null,
  rxBuf: "",
  seq: 1,
};

function setConn(up, text) {
  const el = document.getElementById("conn");
  el.classList.toggle("pill--up", up);
  el.classList.toggle("pill--down", !up);
  el.textContent = text;
}

function setBleConn(up, text) {
  const el = document.getElementById("bleConn");
  if (!el) return;
  el.classList.toggle("pill--up", up);
  el.classList.toggle("pill--down", !up);
  el.textContent = text;
}

function setPill(elId, up, text) {
  const el = document.getElementById(elId);
  if (!el) return;
  el.classList.toggle("pill--up", !!up);
  el.classList.toggle("pill--down", up === false);
  el.textContent = text;
}

// ---- Pin header definitions (ESP32‑C6 DevKitC‑1) ----
// Ref: official user guide pin header tables (J1/J3).
// https://documentation.espressif.com/esp-dev-kits/zh_CN/latest/esp32c6/esp32-c6-devkitc-1/user_guide.html#id9
const HEADER_PINS = {
  J1: [
    { pos: 1, name: "3V3", gpio: null, hint: "电源" },
    { pos: 2, name: "RST", gpio: null, hint: "复位" },
    { pos: 3, name: "GPIO4", gpio: 4, hint: "MTMS（strap）", strap: true },
    { pos: 4, name: "GPIO5", gpio: 5, hint: "MTDI（strap）", strap: true },
    { pos: 5, name: "GPIO6", gpio: 6, hint: "MTCK" },
    { pos: 6, name: "GPIO7", gpio: 7, hint: "MTDO" },
    { pos: 7, name: "GPIO0", gpio: 0, hint: "strap" , strap: true},
    { pos: 8, name: "GPIO1", gpio: 1, hint: "strap" , strap: true},
    { pos: 9, name: "GPIO8", gpio: 8, hint: "板载 RGB LED（strap）", strap: true, reserved: true },
    { pos: 10, name: "GPIO10", gpio: 10, hint: "" },
    { pos: 11, name: "GPIO11", gpio: 11, hint: "" },
    { pos: 12, name: "GPIO2", gpio: 2, hint: "" },
    { pos: 13, name: "GPIO3", gpio: 3, hint: "" },
    { pos: 14, name: "5V", gpio: null, hint: "电源" },
    { pos: 15, name: "GND", gpio: null, hint: "地" },
    { pos: 16, name: "NC", gpio: null, hint: "空" },
  ],
  J3: [
    { pos: 1, name: "GND", gpio: null, hint: "地" },
    { pos: 2, name: "TX", gpio: 16, hint: "U0TXD" },
    { pos: 3, name: "RX", gpio: 17, hint: "U0RXD" },
    { pos: 4, name: "GPIO15", gpio: 15, hint: "strap", strap: true },
    { pos: 5, name: "GPIO23", gpio: 23, hint: "" },
    { pos: 6, name: "GPIO22", gpio: 22, hint: "" },
    { pos: 7, name: "GPIO21", gpio: 21, hint: "" },
    { pos: 8, name: "GPIO20", gpio: 20, hint: "" },
    { pos: 9, name: "GPIO19", gpio: 19, hint: "" },
    { pos: 10, name: "GPIO18", gpio: 18, hint: "" },
    { pos: 11, name: "GPIO9", gpio: 9, hint: "strap", strap: true },
    { pos: 12, name: "GND", gpio: null, hint: "地" },
    { pos: 13, name: "GPIO13", gpio: 13, hint: "USB D+（禁用）", reserved: true },
    { pos: 14, name: "GPIO12", gpio: 12, hint: "USB D-（禁用）", reserved: true },
    { pos: 15, name: "GND", gpio: null, hint: "地" },
    { pos: 16, name: "NC", gpio: null, hint: "空" },
  ],
};

// ---- User config (localStorage) ----
const CFG_KEY = "uwl_pin_cfg_v1";
function loadCfg() {
  try {
    const s = localStorage.getItem(CFG_KEY);
    return s ? JSON.parse(s) : {};
  } catch (_) {
    return {};
  }
}
function saveCfg(cfg) {
  localStorage.setItem(CFG_KEY, JSON.stringify(cfg || {}));
}
let pinCfg = loadCfg(); // { [pin:number]: {label, role} }

function getPinCfg(pin) {
  const v = pinCfg[String(pin)];
  return v && typeof v === "object" ? v : { label: "", role: "", enabled: false };
}

function isPinEnabled(pin) {
  const cfg = getPinCfg(pin);
  return !!cfg.enabled;
}

function render() {
  const list = document.getElementById("gpioList");
  if (!list) return;
  list.innerHTML = "";

  const pins = Array.from(gpioMap.keys()).sort((a, b) => a - b);
  for (const pin of pins) {
    const g = gpioMap.get(pin);
    const cfg = getPinCfg(g.pin);
    // Control page: only show enabled pins.
    if (document.body.classList.contains("page--control") && !cfg.enabled) continue;
    const item = document.createElement("div");
    item.className = "item";

    const meta = document.createElement("div");
    meta.className = "item__meta";

    const pinEl = document.createElement("div");
    pinEl.className = "item__pin";
    pinEl.textContent = cfg.label ? `${cfg.label}（GPIO${g.pin}）` : `GPIO${g.pin}`;

    const sub = document.createElement("div");
    sub.className = "item__sub";
    sub.textContent = `dir=${g.dir}, value=${g.value}`;

    meta.appendChild(pinEl);
    meta.appendChild(sub);

    const ctrl = document.createElement("div");
    ctrl.className = "toggle";

    if (g.dir === "out") {
      const sw = document.createElement("div");
      sw.className = "switch";
      sw.dataset.on = g.value ? "1" : "0";

      const knob = document.createElement("div");
      knob.className = "switch__knob";
      sw.appendChild(knob);

      sw.addEventListener("click", () => {
        const next = g.value ? 0 : 1;
        sendAny({ type: "gpio_set", pin: g.pin, value: next });
      });

      ctrl.appendChild(sw);
    } else {
      const pill = document.createElement("div");
      pill.className = "pill " + (g.value ? "pill--up" : "pill--down");
      pill.textContent = g.value ? "HIGH" : "LOW";
      ctrl.appendChild(pill);
    }

    item.appendChild(meta);
    item.appendChild(ctrl);
    list.appendChild(item);
  }
}

function renderHeaders() {
  const hdrJ1 = document.getElementById("hdrJ1");
  const hdrJ3 = document.getElementById("hdrJ3");
  if (!hdrJ1 || !hdrJ3) return;
  hdrJ1.innerHTML = "";
  hdrJ3.innerHTML = "";

  const renderOne = (root, headerName, pins) => {
    for (const p of pins) {
      const row = document.createElement("div");
      row.className = "pinRow";

      const left = document.createElement("div");
      left.className = "badge";
      left.textContent = `${headerName}-${p.pos}`;

      const meta = document.createElement("div");
      meta.className = "pinMeta";

      const title = document.createElement("div");
      title.className = "pinName";

      const hint = document.createElement("div");
      hint.className = "pinHint";

      const ctrl = document.createElement("div");
      ctrl.className = "toggle";

      if (typeof p.gpio !== "number") {
        title.textContent = p.name;
        hint.textContent = p.hint || "";
        const b = document.createElement("div");
        b.className = "badge";
        b.textContent = "非GPIO";
        ctrl.appendChild(b);
        row.classList.add("pinRow--inactive");
      } else {
        const g = gpioMap.get(p.gpio);
        const cfg = getPinCfg(p.gpio);
        title.textContent = cfg.label ? `${cfg.label}（${p.name}）` : p.name;
        hint.textContent = [p.hint, cfg.role ? `用途=${cfg.role}` : ""].filter(Boolean).join(" · ");

        // Enable toggle (UI-only)
        const en = document.createElement("div");
        en.className = "switch";
        en.dataset.on = cfg.enabled ? "1" : "0";
        const enKnob = document.createElement("div");
        enKnob.className = "switch__knob";
        en.appendChild(enKnob);
        en.addEventListener("click", (ev) => {
          ev.stopPropagation();
          cfg.enabled = !cfg.enabled;
          pinCfg[String(p.gpio)] = cfg;
          saveCfg(pinCfg);
          render();
          renderHeaders();
        });

        if (p.reserved) {
          const b = document.createElement("div");
          b.className = "badge badge--warn";
          b.textContent = "保留/不建议";
          ctrl.appendChild(b);
        } else if (p.strap) {
          const b = document.createElement("div");
          b.className = "badge badge--warn";
          b.textContent = "strap";
          ctrl.appendChild(b);
        }

        if (!g) {
          const b = document.createElement("div");
          b.className = "badge";
          b.textContent = "固件未启用";
          ctrl.appendChild(b);
          row.classList.add("pinRow--inactive");
        } else if (g.dir === "out") {
          const sw = document.createElement("div");
          sw.className = "switch";
          sw.dataset.on = g.value ? "1" : "0";
          const knob = document.createElement("div");
          knob.className = "switch__knob";
          sw.appendChild(knob);
          sw.addEventListener("click", () => {
            const next = g.value ? 0 : 1;
            // Only allow control if enabled in UI
            if (cfg.enabled) sendAny({ type: "gpio_set", pin: g.pin, value: next });
          });
          ctrl.appendChild(sw);
        } else {
          const pill = document.createElement("div");
          pill.className = "pill " + (g.value ? "pill--up" : "pill--down");
          pill.textContent = g.value ? "HIGH" : "LOW";
          ctrl.appendChild(pill);
        }

        ctrl.appendChild(en);

        // Click anywhere to configure alias/role
        row.addEventListener("click", (ev) => {
          // avoid toggle click opening dialog twice
          if (ev.target && (ev.target.classList.contains("switch") || ev.target.classList.contains("switch__knob"))) return;
          openPinDialog(p.gpio, p.name);
        });

        if (!cfg.enabled) row.classList.add("pinRow--inactive");
      }

      meta.appendChild(title);
      meta.appendChild(hint);

      row.appendChild(left);
      row.appendChild(meta);
      row.appendChild(ctrl);
      root.appendChild(row);
    }
  };

  renderOne(hdrJ1, "J1", HEADER_PINS.J1);
  renderOne(hdrJ3, "J3", HEADER_PINS.J3);
}

function applyState(msg) {
  if (Array.isArray(msg.gpios)) {
    for (const g of msg.gpios) {
      gpioMap.set(g.pin, { pin: g.pin, dir: g.dir, value: g.value });
    }
    render();
    renderHeaders();
  }
}

function applyChanged(msg) {
  if (typeof msg.pin !== "number") return;
  const prev = gpioMap.get(msg.pin) || { pin: msg.pin, dir: msg.dir || "in", value: 0 };
  gpioMap.set(msg.pin, { ...prev, dir: msg.dir || prev.dir, value: msg.value ? 1 : 0 });
  render();
  renderHeaders();
}

function send(obj) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  // Keep Wi‑Fi WS command format aligned with BLE:
  // gpio_set -> {"t":"s","p":X,"v":0|1,"i":id}
  // gpio_get -> {"t":"g","p":X,"i":id}
  // state    -> {"t":"state","i":id}
  // gpio_list-> {"t":"l","i":id}
  let payload = obj;
  if (obj && typeof obj === "object" && typeof obj.type === "string") {
    const id = wsSeq++;
    if (obj.type === "gpio_set") payload = { t: "s", p: obj.pin, v: obj.value ? 1 : 0, i: id };
    else if (obj.type === "gpio_get") payload = { t: "g", p: obj.pin, i: id };
    else if (obj.type === "state") payload = { t: "state", i: id };
    else if (obj.type === "gpio_list") payload = { t: "l", i: id };
    else payload = { ...obj, id };
  }
  ws.send(JSON.stringify(payload));
}

function sendAny(obj) {
  // Prefer WS for realtime UI; BLE is optional for direct GATT control.
  send(obj);
  if (bleIsConnected() && obj && obj.type === "gpio_set") {
    void bleWrite(obj);
  }
}

// ---- Pin config dialog ----
let dlgPin = null;
function openPinDialog(pin, pinName) {
  const dlg = document.getElementById("pinDlg");
  if (!dlg) return;
  dlgPin = pin;
  const t = document.getElementById("dlgPinTitle");
  const inp = document.getElementById("dlgLabel");
  const role = document.getElementById("dlgRole");
  const cfg = getPinCfg(pin);
  if (t) t.textContent = `${pinName} / GPIO${pin}`;
  if (inp) inp.value = cfg.label || "";
  if (role) role.value = cfg.role || "";
  dlg.showModal();
}

const dlgSave = document.getElementById("dlgSave");
if (dlgSave) dlgSave.addEventListener("click", () => {
  if (typeof dlgPin !== "number") return;
  const inp = document.getElementById("dlgLabel");
  const role = document.getElementById("dlgRole");
  const next = {
    label: (inp && inp.value ? inp.value.trim() : ""),
    role: (role && role.value ? role.value : ""),
    enabled: !!getPinCfg(dlgPin).enabled,
  };
  pinCfg[String(dlgPin)] = next;
  saveCfg(pinCfg);
  render();
  renderHeaders();
});

function bleIsSupported() {
  return typeof navigator !== "undefined" && !!navigator.bluetooth;
}

function bleIsConnected() {
  return !!(ble.device && ble.device.gatt && ble.device.gatt.connected && ble.ctrlChar && ble.stateChar);
}

async function bleWrite(obj) {
  if (!bleIsConnected()) return;
  // v2 short-form for smaller BLE payload:
  // gpio_set -> {"t":"s","p":X,"v":0|1,"i":id}
  // gpio_get -> {"t":"g","p":X,"i":id}
  // state    -> {"t":"state","i":id}  (prefer STATE read for full snapshot)
  let payload = obj;
  if (obj && typeof obj === "object" && typeof obj.type === "string") {
    const id = ble.seq++;
    if (obj.type === "gpio_set") payload = { t: "s", p: obj.pin, v: obj.value ? 1 : 0, i: id };
    else if (obj.type === "gpio_get") payload = { t: "g", p: obj.pin, i: id };
    else if (obj.type === "state") payload = { t: "state", i: id };
    else if (obj.type === "gpio_list") payload = { t: "l", i: id };
    else payload = { ...obj, id };
  }

  const text = JSON.stringify(payload);
  const data = new TextEncoder().encode(text);
  if (ble.ctrlChar.writeValueWithoutResponse) {
    await ble.ctrlChar.writeValueWithoutResponse(data);
  } else {
    await ble.ctrlChar.writeValue(data);
  }
}

function handleJsonMessage(msg) {
  if (!msg || typeof msg.type !== "string") return;
  if (msg.type === "state") applyState(msg);
  else if (msg.type === "gpio_changed") applyChanged(msg);
  else if (msg.type === "gpio") applyChanged({ type: "gpio_changed", pin: msg.pin, value: msg.value, dir: "in" });
  else if (msg.type === "resp") {
    // eslint-disable-next-line no-console
    console.debug("BLE resp:", msg);
  } else if (msg.type === "err") {
    // eslint-disable-next-line no-console
    console.warn("BLE err:", msg);
    const code = msg.code ? String(msg.code) : "ERR";
    setPill("bleStat", false, `BLE: ${code}`);
  }
  else if (msg.type === "status") {
    setPill("wifiSta", null, `Wi‑Fi STA: ${typeof msg.sta_count === "number" ? msg.sta_count : "—"}`);
    setPill("wsClients", null, `WS 客户端: ${typeof msg.ws_clients === "number" ? msg.ws_clients : "—"}`);
    const bleText = (msg.ble_connected ? "已连接" : "未连接") + (msg.ble_notify ? "·notify" : "");
    setPill("bleStat", msg.ble_connected ? true : false, `BLE: ${bleText}`);
  }
}

function onBleNotification(ev) {
  try {
    const v = ev.target.value;
    const text = new TextDecoder().decode(v.buffer ? v.buffer : v);
    // Most notifications are small (gpio_changed). If MTU is small, we may get partial JSON; buffer and retry.
    ble.rxBuf += text;
    try {
      const msg = JSON.parse(ble.rxBuf);
      ble.rxBuf = "";
      handleJsonMessage(msg);
    } catch (_) {
      if (ble.rxBuf.length > 4096) ble.rxBuf = "";
    }
  } catch (_) {
    // ignore
  }
}

async function bleReadState() {
  if (!bleIsConnected()) return;
  const v = await ble.stateChar.readValue();
  const text = new TextDecoder().decode(v.buffer ? v.buffer : v);
  const msg = JSON.parse(text);
  handleJsonMessage(msg);
}

function setBleButtons(connected) {
  const btnC = document.getElementById("btnBleConnect");
  const btnD = document.getElementById("btnBleDisconnect");
  const btnR = document.getElementById("btnBleReadState");
  if (btnC) btnC.disabled = connected;
  if (btnD) btnD.disabled = !connected;
  if (btnR) btnR.disabled = !connected;
}

async function bleConnect() {
  if (!bleIsSupported()) {
    setBleConn(false, "BLE: 浏览器不支持");
    return;
  }
  setBleConn(false, "BLE: 选择设备…");
  try {
    const device = await navigator.bluetooth.requestDevice({
      filters: [{ services: [BLE_UUIDS.svc] }],
      optionalServices: [BLE_UUIDS.svc],
    });
    ble.device = device;
    ble.device.addEventListener("gattserverdisconnected", () => {
      setBleConn(false, "BLE: 已断开");
      setBleButtons(false);
    });

    setBleConn(false, "BLE: 连接中…");
    ble.server = await device.gatt.connect();
    ble.svc = await ble.server.getPrimaryService(BLE_UUIDS.svc);
    ble.ctrlChar = await ble.svc.getCharacteristic(BLE_UUIDS.ctrl);
    ble.stateChar = await ble.svc.getCharacteristic(BLE_UUIDS.state);

    await ble.stateChar.startNotifications();
    ble.stateChar.addEventListener("characteristicvaluechanged", onBleNotification);

    setBleConn(true, `BLE: 已连接(${device.name || "device"})`);
    setBleButtons(true);
    await bleReadState();
  } catch (e) {
    setBleConn(false, "BLE: 连接失败");
    setBleButtons(false);
    // console for debugging if needed
    // eslint-disable-next-line no-console
    console.warn("BLE connect failed:", e);
  }
}

function bleDisconnect() {
  try {
    if (ble.device && ble.device.gatt && ble.device.gatt.connected) ble.device.gatt.disconnect();
  } catch (_) {
    // ignore
  }
}

function connect() {
  setConn(false, "WS: 连接中…");
  ws = new WebSocket(WS_URL);

  ws.onopen = () => {
    setConn(true, "WS: 已连接");
    send({ type: "state" });
  };

  ws.onclose = () => {
    setConn(false, "WS: 已断开，重连中…");
    setTimeout(connect, 800);
  };

  ws.onerror = () => {
    setConn(false, "WS: 错误");
  };

  ws.onmessage = (ev) => {
    try {
      const msg = JSON.parse(ev.data);
      handleJsonMessage(msg);
    } catch (_) {
      // ignore
    }
  };
}

const btnRefresh = document.getElementById("btnRefresh");
if (btnRefresh) btnRefresh.addEventListener("click", () => {
  send({ type: "state" });
  if (bleIsConnected()) void bleReadState();
});

const btnCfgExport = document.getElementById("btnCfgExport");
if (btnCfgExport) btnCfgExport.addEventListener("click", async () => {
  try {
    const text = JSON.stringify(pinCfg || {}, null, 2);
    await navigator.clipboard.writeText(text);
    alert("已复制配置到剪贴板");
  } catch (_) {
    alert("复制失败：请手动长按选择复制（或允许剪贴板权限）");
  }
});

const btnCfgImport = document.getElementById("btnCfgImport");
if (btnCfgImport) btnCfgImport.addEventListener("click", () => {
  const text = prompt("粘贴配置 JSON（将覆盖本机配置）");
  if (!text) return;
  try {
    const obj = JSON.parse(text);
    pinCfg = obj || {};
    saveCfg(pinCfg);
    render();
    renderHeaders();
    alert("导入成功");
  } catch (_) {
    alert("JSON 格式不正确");
  }
});

const btnBleConnect = document.getElementById("btnBleConnect");
if (btnBleConnect) btnBleConnect.addEventListener("click", () => {
  void bleConnect();
});
const btnBleDisconnect = document.getElementById("btnBleDisconnect");
if (btnBleDisconnect) btnBleDisconnect.addEventListener("click", () => {
  bleDisconnect();
});
const btnBleReadState = document.getElementById("btnBleReadState");
if (btnBleReadState) btnBleReadState.addEventListener("click", () => {
  void bleReadState();
});

setBleConn(false, bleIsSupported() ? "BLE: 未连接" : "BLE: 不支持");
setBleButtons(false);
connect();
renderHeaders();

const btnEnableFwPins = document.getElementById("btnEnableFwPins");
if (btnEnableFwPins) btnEnableFwPins.addEventListener("click", () => {
  // Enable only pins that firmware currently exposes in state snapshot
  for (const pin of gpioMap.keys()) {
    const cfg = getPinCfg(pin);
    cfg.enabled = true;
    pinCfg[String(pin)] = cfg;
  }
  saveCfg(pinCfg);
  render();
  renderHeaders();
});