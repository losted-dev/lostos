// The API of a JavaScript plugin of the speaker (globalThis.lostos), on top of the four functions of lostos-js.c (__host).
// Every call is a request to the firmware, which checks it against the permissions the plugin was given (docs/plugins-js.md).
(function () {
  const host = globalThis.__host;
  delete globalThis.__host;
  const call = (method, args) => host.call(method, args === undefined ? "" : String(args));
  const fmt = (a) => a.map((x) => (typeof x === "string" ? x : x instanceof Error ? x.stack || String(x) : JSON.stringify(x))).join(" ");

  // timers
  const timers = new Map();
  globalThis.__timer = (id) => {
    const t = timers.get(id);
    if (!t) return;
    if (!t.repeat) timers.delete(id);
    t.fn(...t.args);
  };
  globalThis.setTimeout = (fn, ms = 0, ...args) => { const id = host.timer(ms, false); timers.set(id, { fn, args, repeat: false }); return id; };
  globalThis.setInterval = (fn, ms = 0, ...args) => { const id = host.timer(ms, true); timers.set(id, { fn, args, repeat: true }); return id; };
  globalThis.clearTimeout = globalThis.clearInterval = (id) => { timers.delete(id); host.clear(id); };
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  // events of the speaker: "button" (action, action_long, volume_up, volume_down), "media" ({source, playing, title}), "volume" (0..1)
  const handlers = {};
  const subscribed = new Set();
  const parse = {
    media: (d) => { const [source, playing, ...t] = d.split(" "); return { source, playing: playing === "1", title: t.join(" ") }; },
    volume: (d) => Number(d),
  };
  function on(name, fn) {
    (handlers[name] = handlers[name] || []).push(fn);
    if (["button", "media", "volume"].includes(name) && !subscribed.has(name)) {
      subscribed.add(name);
      call("subscribe", name).catch((e) => log("subscribe " + name + ": " + e.message));
    }
  }

  // commands for the voice assistant: the server's language model decides when to call them and with which arguments
  const tools = new Map();
  function tool(decl, fn) {
    if (!decl || !/^[a-z][a-z0-9_]{1,40}$/.test(decl.name || "")) throw new TypeError("tool: name of latin letters, digits and _");
    const props = {}, required = [];
    for (const [k, v] of Object.entries(decl.parameters || {})) {
      const p = typeof v === "string" ? { type: "string", description: v } : { ...v };
      if (!p.optional) required.push(k);
      delete p.optional;
      props[k] = p;
    }
    const schema = { type: "object", properties: props, required };
    tools.set(decl.name, fn);
    return call("tool", JSON.stringify({ name: decl.name, description: String(decl.description || ""), parameters: schema }));
  }
  const inflight = new Set();
  let running = null;          // the call whose synchronous part runs right now (an interrupted loop leaves it set)
  async function runTool(c) {
    let result;
    inflight.add(c.id);
    try {
      const fn = tools.get(c.name);
      if (!fn) throw new Error("нет такой команды: " + c.name);
      running = c.id;
      const pending = fn(c.args || {});
      running = null;
      result = await pending;
      result = result === undefined ? "готово" : typeof result === "string" ? result : JSON.stringify(result);
    } catch (e) {
      result = "Ошибка: " + (e && e.message ? e.message : String(e));
    }
    running = null;
    if (!inflight.delete(c.id)) return;                     // already answered by __aborted
    await call("toolresult", JSON.stringify({ id: c.id, result: result.slice(0, 4000) }));
  }
  // lostos-js.c calls this after a job was interrupted (an endless loop): such an error cannot be caught in JavaScript, so the call that
  // was running is answered here, otherwise the assistant would wait for it until its own timeout
  globalThis.__aborted = () => {
    const id = running !== null ? running : inflight.size === 1 ? [...inflight][0] : null;
    running = null;
    if (id === null) return;
    inflight.delete(id);
    call("toolresult", JSON.stringify({ id, result: "Ошибка: команда работала слишком долго и была остановлена" })).catch(() => {});
  };

  globalThis.__event = (name, data) => {
    if (name === "tool") return runTool(JSON.parse(data));
    const v = parse[name] ? parse[name](data) : data;
    for (const fn of handlers[name] || []) {
      try {
        const r = fn(v);
        if (r && typeof r.catch === "function") r.catch((e) => log(e));
      } catch (e) {
        log(e);
      }
    }
  };

  // HTTP(S): the firmware makes the request (its TLS, its certificates); the body comes back as text, up to 256 KB
  async function http(url, opts = {}) {
    const headers = { ...(opts.headers || {}) };
    let body = opts.body;
    if (opts.json !== undefined) {
      body = JSON.stringify(opts.json);
      headers["Content-Type"] = headers["Content-Type"] || "application/json";
    }
    const r = JSON.parse(await call("http", JSON.stringify({ method: opts.method || (body !== undefined ? "POST" : "GET"), url: String(url), headers, body: body === undefined ? "" : String(body) })));
    return { status: r.status, ok: r.status >= 200 && r.status < 300, type: r.type || "", text: r.body, json: () => JSON.parse(r.body) };
  }
  http.json = async (url, opts) => {
    const r = await http(url, opts);
    if (!r.ok) throw new Error(`HTTP ${r.status} от ${String(url).split("/")[2] || url}`);
    return r.json();
  };

  function log(...a) { call("log", fmt(a)).catch(() => {}); }
  globalThis.console = { log, info: log, warn: log, error: log };

  globalThis.lostos = Object.freeze({
    log, on, tool, http, sleep,
    led: (r, g, b, seconds = 3) => call("led", `${r | 0} ${g | 0} ${b | 0} ${seconds | 0}`),
    display: (text, seconds = 3) => call("display", `${seconds | 0} ${text}`),
    media: (cmd) => call("media", cmd),
    play: (url, opts = {}) => call("play", JSON.stringify({ url: String(url), title: String(opts.title || "") })),
    stop: () => call("stop"),
    volume: async () => Number(await call("volume")),
    publish: (topic, text = "") => call("publish", `${topic} ${text}`),
    storage: Object.freeze({
      get: async (key) => { const v = await call("get", key); return v === "" ? null : v; },
      set: (key, value) => call("set", `${key} ${value}`),
    }),
  });
})();
