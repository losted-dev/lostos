"use strict";
// The wizard of the lostOS installer. Nothing from a station or a server is put into the page as HTML without esc().
const $ = (s, r = document) => r.querySelector(s);
const esc = (t) => String(t ?? "").replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
const TOKEN = document.querySelector('meta[name="token"]').content;
async function api(path, body) {
  const r = await fetch("/api/" + path, { method: "POST", headers: { "X-Token": TOKEN, "Content-Type": "application/json" }, body: JSON.stringify(body || {}) });
  if (!r.ok) throw new Error("HTTP " + r.status);
  return r.json();
}
let toastT;
function toast(t) { const e = $("#toast"); e.textContent = t; e.classList.add("show"); clearTimeout(toastT); toastT = setTimeout(() => e.classList.remove("show"), 2600); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const ICON = {
  speaker: '<path d="M7 3h10a1.5 1.5 0 0 1 1.5 1.5v15A1.5 1.5 0 0 1 17 21H7a1.5 1.5 0 0 1-1.5-1.5v-15A1.5 1.5 0 0 1 7 3zM12 8a1.5 1.5 0 1 0 0 .01M12 12.5a3.5 3.5 0 1 0 0 .01"/>',
  cloud: '<path d="M7 18a4 4 0 0 1-.6-8A5.5 5.5 0 0 1 17 8.7 4.7 4.7 0 0 1 17 18z"/>',
  air: '<path d="M5.5 17.5A8.5 8.5 0 1 1 18.5 17.5M8.6 14.6A4.6 4.6 0 1 1 15.4 14.6"/><path d="M12 14.5l4 5.5H8z"/>',
  spotify: '<circle cx="12" cy="12" r="9.5"/><path d="M7 9.2c3.4-1 7.2-.7 10.2 1M7.6 12.4c2.8-.8 5.8-.5 8.3.9M8.2 15.4c2.2-.6 4.4-.4 6.4.6"/>',
  pc: '<path d="M3 5h18v11H3zM8 20h8M12 16v4"/>', chip: '<path d="M7 7h10v10H7zM10 3v4M14 3v4M10 17v4M14 17v4M3 10h4M3 14h4M17 10h4M17 14h4"/>',
};
// the reasons of the installer come in English (they are also its terminal output): the wizard says them in Russian
function ru(reason) {
  const R = [[/rootkeeper/, "Прошивка этой колонки не запускает /data/quasar/rootkeeper.sh при старте: так установщик её подключить не может"],
    [/not a Yandex Station/, "Это не Станция Яндекса"], [/not root/, "adb без root-прав"], [/not supported yet/, "Эта модель пока не поддерживается"],
    [/unauthorized|offline|not usable/, "adb недоступен: на колонке разрешите отладку"], [/libraries of Yandex/, "На колонке нет библиотек Яндекса, они нужны lostOS"]];
  const hit = R.find(([re]) => re.test(reason || ""));
  return hit ? hit[1] : reason;
}
const svg = (k) => `<span class="ico"><svg viewBox="0 0 24 24">${ICON[k]}</svg></span>`;

// The address of Home Assistant that the speaker forwards: "192.168.1.5:8123", or an address as it is written in a browser (http://192.168.1.5, https://192.168.1.5:8123): the port of the scheme is taken when there is none.
// A bare address without a port is not guessed (8123 or 80?): the person is asked.
// A server that the lostOS team does not know: the person is told what such a server can do and has to agree (the speaker lets it hear what is said after the wake word and control it).
function srvBanner(i) {
  if (i.banned) return `<div class="warnbox ban"><b>Сервер заблокирован</b><p>Этот сервер в списке заблокированных lostOS: ${esc(i.banned_why || "причина не указана")}. Колонку к нему подключить нельзя</p></div>`;
  if (i.known) return `<p class="msg ok">✓ Известный сервер: ${esc(i.known_name)}</p>`;
  const why = i.list === "ok" ? "Этот сервер не входит в список проверенных lostOS." : "Список проверенных серверов получить не удалось, поэтому этот адрес проверить нельзя.";
  return `<div class="warnbox"><b>Осторожно: чужой сервер</b><p>${why} Сервер слышит всё, что вы говорите колонке после слова активации, управляет ею и может запускать на ней программы (только подписанные). Указывайте свой сервер или такой, которому вы доверяете.</p>
    <label class="chk2"><input type="checkbox" id="srvtrust" ${S.srvTrust ? "checked" : ""}> Я понимаю риск и доверяю этому серверу</label></div>`;
}
function normHA(v) {
  v = (v || "").trim();
  if (!v) return { ok: true, value: "" };
  const m = /^(?:(https?):\/\/)?(\d{1,3}(?:\.\d{1,3}){3})(?::(\d{1,5}))?(?:\/.*)?$/i.exec(v);
  if (!m) return { ok: false, error: "Нужен адрес вида 192.168.1.5:8123" };
  const port = m[3] || (m[1] ? (m[1].toLowerCase() === "https" ? "443" : "80") : "");
  if (!port) return { ok: false, error: "Укажите порт: 192.168.1.5:8123 (или :80, если Home Assistant открывается без порта в адресной строке)" };
  if (+port < 1 || +port > 65535 || m[2].split(".").some((x) => +x > 255)) return { ok: false, error: "Адрес или порт не похожи на настоящие" };
  return { ok: true, value: m[2] + ":" + port };
}
const S = { step: 0, env: null, stations: [], sel: "", recv: null, srvType: "url", srvUrl: "", srvInfo: null, ha: "", srvTrust: false, installing: false, installOk: false, link: null, linkTimer: 0, job: null };
const STEPS = [
  { id: "welcome", name: "Начало", title: "Установка lostOS", sub: "Свой голосовой ассистент на вашей Яндекс Станции: ваш сервер, ваши ключи, никакого облака Яндекса." },
  { id: "station", name: "Колонка", title: "Выберите колонку", sub: "Ищем Станции в вашей сети, к которым открыт adb." },
  { id: "server", name: "Сервер", title: "Где будет жить сервер", sub: "Сервер отвечает на голос, ходит в нейросеть и открывает панель управления." },
  { id: "receivers", name: "Приёмники", title: "Что поставить на колонку", sub: "AirPlay и Spotify Connect: нужен один, оба или ни одного." },
  { id: "install", name: "Установка", title: "Устанавливаю", sub: "Это занимает меньше минуты. Штатная система Яндекса остаётся на месте." },
  { id: "link", name: "Связь", title: "Связь с сервером", sub: "Колонка находит сервер, а вы подтверждаете, что это ваша колонка." },
  { id: "done", name: "Готово", title: "Всё готово", sub: "" },
];
// the receivers that the archive can put on the chosen station (none for a model whose programs are not in the archive yet)
const receiversOf = () => (S.stations.find((s) => s.serial === S.sel) || {}).receivers || [];
const visible = () => STEPS.filter((s) => (s.id !== "link" || S.srvType === "url") && (s.id !== "receivers" || receiversOf().length));
const cur = () => visible()[S.step];

function renderSteps() {
  $("#steps").innerHTML = visible().map((s, i) => `<li class="${i === S.step ? "on" : i < S.step ? "past" : ""}"><i>${i < S.step ? "✓" : i + 1}</i>${esc(s.name)}</li>`).join("");
}
function paint() {
  const st = cur();
  renderSteps();
  $("#tag").textContent = (S.srvInfo && S.srvInfo.tagline) || "User-friendly оболочка для станций";    // before a server is chosen the same line as the panel has; then what the server says about itself
  $("#title").textContent = st.id === "done" ? (S.srvType === "url" ? "Всё готово" : "Колонка установлена") : st.title;
  $("#sub").textContent = st.sub;
  $("#back").style.visibility = S.step === 0 || st.id === "install" && S.installing ? "hidden" : "visible";
  const next = $("#next");
  next.disabled = false;
  next.textContent = "Далее";
  RENDER[st.id](st, next);
}
async function go(d) { S.step = Math.max(0, Math.min(visible().length - 1, S.step + d)); clearInterval(S.linkTimer); paint(); }
$("#back").onclick = () => go(-1);
$("#next").onclick = () => NEXT[cur().id] ? NEXT[cur().id]() : go(1);

const RENDER = {
  welcome(st, next) {
    $("#body").innerHTML = `<div class="card"><div class="row">${svg("speaker")}<div class="k"><b>Что понадобится</b><small>Станция с root-доступом и включённым adb по сети. Компьютер и колонка в одной сети.</small></div></div></div>
      <div class="card"><div class="row">${svg("cloud")}<div class="k"><b>Сервер lostOS</b><small>Свой на VDS (адрес и HTTPS), на этом компьютере или, позже, прямо в колонке.</small></div></div></div>
      <p class="hint">Версия установщика: <span id="vv"></span>. Ничего не стирается: вернуть всё как было можно этим же установщиком (<code>--uninstall</code>).</p>`;
    next.textContent = "Начать";
    api("env").then((e) => { S.env = e; $("#ver").textContent = e.version ? "lostOS " + e.version : ""; if ($("#vv")) $("#vv").textContent = e.version || "—"; if (!e.payload) { $("#body").insertAdjacentHTML("beforeend", '<p class="msg bad">Рядом с установщиком нет папки satellite: распакуйте архив целиком.</p>'); next.disabled = true; } });
  },
  station(st, next) {
    const list = S.stations.map((s) => `<div class="card opt ${s.installable ? "" : "off"} ${S.sel === s.serial ? "sel" : ""}" data-sel="${esc(s.serial)}"><div class="row">${svg("speaker")}<div class="k"><b>${esc(s.name || "Устройство")}</b><small>${esc(s.serial)}${s.installable ? "" : " · " + esc(ru(s.reason))}</small></div>${s.installable ? `<span class="chip ${s.state === "stock Yandex" ? "" : "ok"}">${s.state === "stock Yandex" ? "штатная система" : /running/.test(s.state) ? "lostOS работает" : "lostOS установлена"}${/older installation/.test(s.state) ? ", обновление" : ""}</span>` : '<span class="chip bad">нельзя</span>'}</div></div>`).join("");
    const adbMsg = S.env && !S.env.adb ? `<div class="card"><div class="row"><div class="k"><b>Нужна программа adb</b><small>Это часть Android platform-tools от Google (условия: developer.android.com/studio/terms). Скачаю её рядом с установщиком.</small></div><button class="btn small acc" id="getadb">Скачать</button></div></div>` : "";
    $("#body").innerHTML = `${adbMsg}<div id="slist">${list || '<p class="hint">Пока ничего не найдено. Нажмите «Найти колонки» или введите адрес вручную.</p>'}</div>
      <div class="row" style="margin-top:14px"><button class="btn small acc" id="scan">Найти колонки</button><input class="field" id="manip" placeholder="IP колонки, например 192.168.1.50" style="flex:1"><button class="btn small" id="addip">Добавить</button></div>
      <p class="hint">Если колонка не находится: на колонке должен быть включён adb по сети (порт 5555).</p>`;
    next.disabled = !S.stations.some((s) => s.serial === S.sel && s.installable);
    $("#scan").onclick = () => doScan([]);
    $("#addip").onclick = () => { const v = $("#manip").value.trim(); if (v) doScan([v]); };
    const g = $("#getadb"); if (g) g.onclick = async () => { g.disabled = true; g.textContent = "Скачиваю…"; const r = await api("adb"); S.env = await api("env"); toast(r.ok ? "adb готов" : "Не получилось скачать adb"); paint(); };
    document.querySelectorAll("[data-sel]").forEach((e) => e.onclick = () => { const s = S.stations.find((x) => x.serial === e.dataset.sel); if (s && s.installable) { S.sel = s.serial; S.recv = null; paint(); } });
    if (!S.stations.length && S.env && S.env.adb && !S.scanned) doScan([]);
  },
  server(st, next) {
    const T = (id, icon, title, sub, off) => `<div class="card opt ${off ? "off" : ""} ${S.srvType === id ? "sel" : ""}" data-type="${id}"><div class="row">${svg(icon)}<div class="k"><b>${title}</b><small>${sub}</small></div>${off ? '<span class="chip warn">скоро</span>' : ""}</div></div>`;
    $("#body").innerHTML = T("url", "cloud", "Сервер по адресу", "Ваш сервер lostOS в интернете (например, на VDS). Колонка подключится к нему сама.") +
      (S.srvType === "url" ? `<div style="margin:10px 4px 4px"><div class="row"><input class="field" id="surl" placeholder="https://адрес-сервера" value="${esc(S.srvUrl)}" style="flex:1"><button class="btn small acc" id="scheck">Проверить</button></div><div id="sres"></div>
        <div class="sect">Home Assistant дома (по желанию)</div><input class="field" id="sha" placeholder="IP:порт, например 192.168.1.5:8123" value="${esc(S.ha)}"><p class="msg" id="shamsg"></p><p class="hint">Колонка пробросит этот адрес вашей домашней сети серверу: так ассистент управляет умным домом, а Home Assistant не открывается в интернет.</p></div>` : "") +
      T("local", "pc", "Сервер на этом компьютере", "Для дома, без интернета и домена: установщик сам поставит маленький сервер (нужен Python 3.11+) и подключит к нему колонку.") +
      (S.srvType === "local" ? `<div style="margin:10px 4px 4px"><p class="msg">Сервер будет жить в вашей папке пользователя и запускаться при входе в систему. Связь колонки с ним идёт внутри домашней сети без шифрования: пользуйтесь им дома, в сети, которой доверяете.</p>
        <div class="sect">Home Assistant дома (по желанию)</div><input class="field" id="sha" placeholder="IP:порт, например 192.168.1.5:8123" value="${esc(S.ha)}"><p class="msg" id="shamsg"></p></div>` : "") +
      T("speaker", "chip", "Сервер в колонке", "Лёгкий сервер прямо на Станции, компьютер не нужен. В разработке.", true);
    document.querySelectorAll("[data-type]").forEach((e) => e.onclick = () => { if (!e.classList.contains("off")) { S.srvType = e.dataset.type; S.srvInfo = null; paint(); } });
    const upd = () => { if (S.srvType === "local") { next.disabled = !normHA(S.ha).ok; return; } next.disabled = !(S.srvType === "url" && S.srvInfo && S.srvInfo.secure && !S.srvInfo.banned && (S.srvInfo.known || S.srvTrust) && normHA(S.ha).ok); };
    if (S.srvType === "url") {
      const showRes = () => { const r = $("#sres"); if (!S.srvInfo) { r.innerHTML = ""; return; } const i = S.srvInfo; r.innerHTML = i.secure ? `<p class="msg ok">✓ Сервер lostOS ${esc(i.version || "")} · ${esc(i.url)}</p>` + (i.plain ? '<p class="msg">Домашний сервер без шифрования: связь видна всем в вашей сети Wi-Fi, поэтому используйте его только дома</p>' : "") + srvBanner(i) : `<p class="msg bad">Сервер найден (${esc(i.url)}), но без HTTPS. Из интернета колонка подключается только к HTTPS-серверу: поставьте перед ним Caddy или nginx с сертификатом. Адрес без шифрования годится только внутри домашней сети (192.168.x.y)</p>`; };
      $("#scheck").onclick = async () => { S.srvUrl = $("#surl").value.trim(); $("#sres").innerHTML = '<p class="msg"><span class="spin"></span>Проверяю…</p>'; const r = await api("server", { url: S.srvUrl }); S.srvInfo = r.ok ? r.server : null; S.srvTrust = false; if (!r.ok) $("#sres").innerHTML = `<p class="msg bad">${esc(r.error)}</p>`; else showRes(); upd(); };
      const haMsg = () => { const r = normHA(S.ha); const m = $("#shamsg"); if (m) { m.textContent = r.ok ? (r.value && r.value !== S.ha ? "Будет записан адрес " + r.value : "") : r.error; m.className = "msg" + (r.ok ? "" : " bad"); } };
      $("#sha").oninput = () => { S.ha = $("#sha").value.trim(); haMsg(); upd(); };
      haMsg();
      $("#sres").onchange = (e) => { if (e.target && e.target.id === "srvtrust") { S.srvTrust = e.target.checked; upd(); } };
      $("#surl").oninput = () => { S.srvUrl = $("#surl").value.trim(); S.srvInfo = null; S.srvTrust = false; $("#sres").innerHTML = ""; upd(); };
      showRes();
    }
    if (S.srvType === "local") {
      const haMsg = () => { const r = normHA(S.ha); const m = $("#shamsg"); if (m) { m.textContent = r.ok ? (r.value && r.value !== S.ha ? "Будет записан адрес " + r.value : "") : r.error; m.className = "msg" + (r.ok ? "" : " bad"); } };
      $("#sha").oninput = () => { S.ha = $("#sha").value.trim(); haMsg(); upd(); };
      haMsg();
    }
    upd();
    next.textContent = receiversOf().length ? "Далее" : "Установить";
  },
  receivers(st, next) {
    const list = receiversOf();
    if (!S.recv) S.recv = Object.fromEntries(list.map((r) => [r.id, true]));
    $("#body").innerHTML = list.map((r) => `<div class="card opt ${S.recv[r.id] ? "sel" : ""}" data-rid="${esc(r.id)}"><div class="row">${svg(/airplay/.test(r.id) ? "air" : "spotify")}<div class="k"><b>${esc(r.name)}</b><small>${esc(r.about)}</small></div>
      <span class="chip ${S.recv[r.id] ? "ok" : ""}">${S.recv[r.id] ? "поставить" : "не ставить"}${r.installed ? ", уже стоит" : ""}</span></div></div>`).join("") +
      `<p class="hint">Нажмите на приёмник, чтобы включить или отключить его. После установки их можно выключить в панели сервера, в «Плагины». Если сейчас пропустите, добавить можно, запустив установщик ещё раз.</p>`;
    document.querySelectorAll("[data-rid]").forEach((e) => e.onclick = () => { S.recv[e.dataset.rid] = !S.recv[e.dataset.rid]; paint(); });
    next.textContent = "Установить";
  },
  install(st, next) {
    $("#body").innerHTML = '<pre class="log" id="log"></pre><p class="msg" id="imsg"></p>';
    next.disabled = !S.installOk;
    next.textContent = "Далее";
    if (!S.installing && !S.installOk) startInstall();
    else drawLog();
  },
  link(st, next) {
    const l = S.link || {};
    const done = l.state === "confirmed";
    $("#body").innerHTML = `<div class="link"><div class="rings ${done ? "done" : ""}"><i></i><i></i><i></i><img src="/icon.png" alt=""></div>
      <div class="sect" style="margin-bottom:0">Идентификатор станции</div><div class="idbox ${l.id ? "copy" : ""}" id="idbox" title="Нажмите, чтобы скопировать">${esc(l.id ? l.id.replace(/(.{5})/g, "$1 ").trim() : "…")}</div>${l.id ? '<p class="hint" style="margin:6px 0 0;text-align:center">нажмите на идентификатор, чтобы скопировать</p>' : ""}</div>
      <div class="chk"><div class="${l.id ? "ok" : "on"}"><i>${l.id ? "✓" : "…"}</i>Колонка запустила lostOS и создала свой ключ</div>
      <div class="${l.state === "pending" || done ? "ok" : l.id ? "on" : ""}"><i>${l.state === "pending" || done ? "✓" : "…"}</i>Колонка нашла сервер и ждёт подтверждения</div>
      <div class="${done ? "ok" : l.state === "pending" ? "on" : ""}"><i>${done ? "✓" : "…"}</i>Вы подтвердили станцию на сервере</div></div>
      ${done ? '<p class="msg ok">Связь установлена: сервер и колонка узнали друг друга.</p>' : `<p class="hint">Откройте панель сервера, впишите этот идентификатор и нажмите кнопку Алисы на колонке.</p><p><button class="btn small acc" id="opensrv">Открыть сервер</button></p>`}`;
    next.disabled = !done;
    next.textContent = "Далее";
    const ib = $("#idbox");
    if (ib && S.link && S.link.id) ib.onclick = async () => { try { await navigator.clipboard.writeText(S.link.id); toast("Идентификатор скопирован"); } catch (e) { const r = document.createRange(); r.selectNodeContents(ib); const s = getSelection(); s.removeAllRanges(); s.addRange(r); toast("Выделено: скопируйте вручную"); } };   // the copy is the plain 20 digits, without the spaces of the display
    const o = $("#opensrv"); if (o) o.onclick = () => api("open", { url: S.setupLink || (S.srvInfo ? S.srvInfo.url : S.srvUrl) });
    clearInterval(S.linkTimer);
    if (!done) S.linkTimer = setInterval(async () => { const r = await api("link", { serial: S.sel }); const was = S.link && S.link.state; S.link = r; if (r.state !== was || !was) paint(); }, 2500);
  },
  done(st, next) {
    const url = S.srvInfo ? S.srvInfo.url : S.srvUrl;
    $("#body").innerHTML = `<div class="card"><div class="row"><div class="k"><b>Колонка</b><small>lostOS установлена и запущена${S.link && S.link.id ? "; идентификатор " + esc(S.link.id) : ""}</small></div><span class="chip ok">готово</span></div></div>
      ${S.srvType === "url" || S.srvType === "local" ? `<div class="card"><div class="row"><div class="k"><b>Сервер</b><small>${esc(url)}</small></div><button class="btn small acc" id="opensrv">Открыть панель</button></div></div>
      <p class="hint">Дальше в панели сервера: ключ нейросети, Spotify и умный дом. Подсказки будут на каждом шаге.</p>` : ""}`;
    next.textContent = "Закрыть";
    const o = $("#opensrv"); if (o) o.onclick = () => api("open", { url: S.setupLink || url });
  },
};
const NEXT = {
  welcome: () => go(1),
  done: async () => { await api("quit").catch(() => {}); $("#body").innerHTML = '<p class="hint">Мастер закрыт, это окно можно закрыть.</p>'; $("#next").disabled = true; },
  install: () => go(1),
};

async function doScan(ips) {
  const b = $("#scan"); if (b) { b.disabled = true; b.innerHTML = '<span class="spin"></span>Ищу…'; }
  S.scanned = true;
  try {
    if (!S.env || !S.env.adb) { const r = await api("adb"); S.env = await api("env"); if (!r.ok) { toast("Нужен adb"); paint(); return; } }
    const r = await api("scan", { ips });
    S.stations = (r.stations || []).filter((x) => x);
    if (!S.stations.some((s) => s.serial === S.sel)) S.sel = (S.stations.find((s) => s.installable) || {}).serial || "";
    if (!S.stations.length) toast("Колонки не найдены");
  } catch (e) { toast("Ошибка поиска"); }
  paint();
}

let logFrom = 0, logLines = [];
function drawLog() { const el = $("#log"); if (!el) return; el.innerHTML = logLines.map((l) => `<div class="${/^==/.test(l) ? "h" : /FAILED|not enough|не подходит|ошибка|missing|corrupted/i.test(l) ? "e" : ""}">${esc(l)}</div>`).join(""); el.scrollTop = el.scrollHeight; }
async function startInstall() {
  S.installing = true; S.installOk = false; logFrom = 0; logLines = [];
  paint();
  const forwards = S.ha && normHA(S.ha).ok && normHA(S.ha).value ? ["ha=" + normHA(S.ha).value] : [];
  const receivers = receiversOf().map((r) => r.id).filter((id) => !S.recv || S.recv[id]);
  const r = await api("install", { serial: S.sel, type: S.srvType, url: S.srvInfo ? S.srvInfo.url : S.srvUrl, forwards, receivers });
  if (!r.ok) { S.installing = false; $("#imsg").className = "msg bad"; $("#imsg").textContent = r.error || "Не получилось начать"; return; }
  for (;;) {
    await sleep(700);
    let j;
    try { j = await api("job", { from: logFrom }); } catch (e) { continue; }          // one lost request must not stop the log for good
    logLines.push(...(j.lines || [])); logFrom = j.next; drawLog();
    if (j.done) {
      S.installing = false; S.installOk = j.ok;
      const m = $("#imsg");
      if (j.ok && S.srvType === "local" && j.result) { S.srvUrl = j.result.server || ""; S.srvInfo = { url: S.srvUrl, plain: true }; S.setupLink = j.result.setup || ""; }
      if (j.ok) { S.link = j.result && j.result.id ? { id: j.result.id, state: j.result.state } : null; m.className = "msg ok"; m.textContent = "Установка завершена."; $("#next").disabled = false; }
      else { m.className = "msg bad"; m.innerHTML = 'Установка не удалась, подробности выше. <button class="btn small acc" id="retry">Повторить</button>'; $("#retry").onclick = () => startInstall(); }
      $("#back").style.visibility = "visible";
      break;
    }
  }
}
setInterval(() => api("ping").catch(() => {}), 10000);
paint();
