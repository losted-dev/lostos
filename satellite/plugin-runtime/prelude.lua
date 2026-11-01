-- The API of a Lua plugin of the speaker (docs/plugins-lua.md): the `alice`, `device`, `register` and `event` of the Luna framework (MIT, nyakokitsu/luna; the shapes here follow its
-- Lua host, src/gateway.rs) as far as this speaker can do them, plus a small `lostos` table. Built on four functions of lua-mode.c: __call, __sleep, __now, __next_event.
-- A call that cannot be done here raises an error that names it; alice.is_available(name) says false for what the speaker has not.
local call, sleep, now, next_event = __call, __sleep, __now, __next_event
__call, __sleep, __now, __next_event = nil, nil, nil, nil

local function ask(method, args)
  local r, why = call(method, args or "")
  if r == nil then error(why or "ошибка", 3) end
  return r
end

local function unsupported(what)
  return function() error(what .. ": на станции lostOS этого нет", 2) end
end

-- device: read-only facts about this speaker (the names of Luna, snake_case and the old PascalCase ones)
local info = {}
for k, v in (call("device", "") or ""):gmatch("(%w+)=(%S+)") do info[k] = v end
local ring, logo = tonumber(info.ring) or 0, tonumber(info.logo) or 0
local has_clock = info.clock == "1"
local features = { "audio.volume.alsa", "audio.play.aplay", "input.evdev", "microphone.capture" }
if ring > 0 then features[#features + 1] = "led.ring.rgb" end
if logo > 0 then features[#features + 1] = "led.logo" end
if has_clock then features[#features + 1] = "display.clock.segment.v1" end
local facts = {
  model = info.model or "", architecture = "aarch64", features = features, led_count = ring + logo,
  has_ring = ring > 0, ring_count = ring, has_logo = logo > 0, logo_count = logo,
  has_clock = has_clock, has_mute_button = true, has_microphone = info.microphone == "1", room = nil,
}
facts.HasRing, facts.RingCount, facts.HasLogo, facts.LogoCount, facts.HasClock, facts.HasMuteButton, facts.HasMicrophone =
  facts.has_ring, facts.ring_count, facts.has_logo, facts.logo_count, facts.has_clock, facts.has_mute_button, facts.has_microphone
device = setmetatable({}, { __index = facts, __newindex = function() error("device только для чтения", 2) end })
event = ""   -- the argument of a manual start in Luna; here a plugin runs as a service

-- the light of the speaker is one colour for the whole ring: a list of colours becomes their mean
local function colour(list)
  if type(list) ~= "table" or #list == 0 then error("нужен список цветов вида \"#RRGGBB\"", 3) end
  local r, g, b = 0, 0, 0
  for _, c in ipairs(list) do
    local rr, gg, bb = tostring(c):match("^#(%x%x)(%x%x)(%x%x)$")
    if not rr then error("цвет \"" .. tostring(c) .. "\" не вида #RRGGBB", 3) end
    r, g, b = r + tonumber(rr, 16), g + tonumber(gg, 16), b + tonumber(bb, 16)
  end
  local n = #list
  return r // n, g // n, b // n
end
local function light(list)
  local r, g, b = colour(list)
  ask("led", string.format("%d %d %d 30", r, g, b))
end
local function dark() ask("led", "0 0 0 1") end
local function zone(fn, is_there)
  return setmetatable({ is_available = function() return is_there end }, { __call = function(_, a) return fn(a) end })
end

local has_light = ring > 0 or logo > 0
-- alice.set(colors) / alice.set.ring / alice.set.logo, alice.off() / alice.off.ring / alice.off.logo, and the same under alice.led
local set = zone(light, has_light)
set.ring, set.logo = zone(light, ring > 0), zone(light, logo > 0)
local off = zone(dark, has_light)
off.ring, off.logo = zone(dark, ring > 0), zone(dark, logo > 0)

local available = {
  led = has_light, volume = true, audio = true, microphone = false, bluetooth = false, display = false, sensors = false,
  clock = has_clock, input = true, network = true, system = false, commands = false,
}
local function is_available(name)
  name = tostring(name)
  if name == "exec" then error("exec has no is_available probe", 2) end
  if name:match("^bluetooth") or name:match("^ipc") or name:match("^sensors") then return false end
  local v = available[name]
  if v == nil then error("unknown speaker API \"" .. name .. "\"", 2) end
  return v
end

local pressed_code = { action = 582, action_long = 582, volume_up = 115, volume_down = 114 }   -- the key codes of the speaker (evdev)
local subscribed = false

-- http.get / http.post: the firmware makes the request (permission network.connect); {status, body}
local function esc(s) return (tostring(s):gsub('[\\"]', "\\%0"):gsub("\n", "\\n")) end
local function host_allowed(url)
  local scheme, host = tostring(url):match("^(%a+)://([^/:?#]+)")
  if not host then error("invalid URL", 3) end
  host = host:lower()
  local cfg = __LUNA or { allow = {}, insecure = false }
  if scheme ~= "https" and not (scheme == "http" and cfg.insecure) then error("only https URLs are allowed", 3) end
  for _, rule in ipairs(cfg.allow) do
    if rule == host or (rule:sub(1, 2) == "*." and host:sub(-#rule + 1) == rule:sub(2)) then return end
  end
  error("host " .. host .. " is not in [network].allow", 3)
end
local function http_request(method, url, body)
  host_allowed(url)
  local out = ask("http", string.format('{"method":"%s","url":"%s","headers":{},"body":"%s"}', method, esc(url), esc(body or "")))
  local status = tonumber(out:match('"status"%s*:%s*(%d+)')) or 0
  local text = out:match('"body"%s*:%s*"(.*)"%s*}') or ""
  text = text:gsub("\\n", "\n"):gsub('\\"', '"'):gsub("\\\\", "\\")
  return { status = status, body = text }
end

alice = {
  is_available = is_available,
  model = function() return facts.model end,
  sleep = function(ms)
    ms = math.floor(tonumber(ms) or 0)
    if ms > 60000 then error("sleep is limited to 60 seconds", 2) end
    sleep(ms)
  end,
  set = set, off = off,
  led = { set = set, off = off, is_available = function() return has_light end },
  play = unsupported("alice.play (световые шаблоны)"),
  volume = {
    set = function(percent)
      local p = tonumber(percent)
      if not p or p < 0 or p > 100 then error("volume must be 0..100", 2) end
      ask("media", "volume " .. math.floor(p))
    end,
    get = function()
      local p = math.floor((tonumber(ask("volume")) or 0) * 100 + 0.5)
      return string.format("Simple mixer control 'Master',0\n  Mono: Playback %d [%d%%] [on]\n", p, p)   -- amixer's text, as Luna returns it
    end,
    is_available = function() return true end,
  },
  audio = {
    play = function(file) ask("playfile", tostring(file)) end,        -- a WAV in the folder of the plugin
    is_available = function() return true end,
  },
  clock = {
    available = function() return has_clock end,
    time = function(text, brightness) ask("display", "30 " .. tostring(text)) end,
    off = function() ask("segments", "0"); ask("display", "1 ") end,
    -- segments({A1 = 255, B1 = 255, ...}): the letter is the segment (A..G), the number the digit (1..4; 0..3 on a Lite 2); the levels are 0..255. The frame holds a minute.
    segments = function(levels)
      if type(levels) ~= "table" then error("нужна таблица сегментов вида {A1 = 255}", 2) end
      local base = (facts.model or ""):find("plum", 1, true) and 0 or 1
      local lv = {}
      for i = 1, 28 do lv[i] = 0 end
      for name, value in pairs(levels) do
        local letter, digit = tostring(name):upper():match("^([A-G])(%d)$")
        local v = tonumber(value)
        if not letter or not v or v < 0 or v > 255 then error("сегмент \"" .. tostring(name) .. "\": имя вида A1, яркость 0..255", 2) end
        local d = tonumber(digit) - base
        if d < 0 or d > 3 then error("сегмент \"" .. tostring(name) .. "\": такой цифры нет", 2) end
        lv[d * 7 + (letter:byte() - 64)] = math.floor(v)
      end
      local hex = {}
      for i = 1, 28 do hex[i] = string.format("%02x", lv[i]) end
      ask("segments", "60000 " .. table.concat(hex))
    end,
    is_available = function() return has_clock end,
  },
  input = {
    devices = function() return "N: Name=\"speaker keys\"\n" end,
    -- waits for the next press of a key of the speaker; {device, type, code, value, time_ms} like an evdev event, nil when the time is out
    next_event = function(timeout_ms)
      timeout_ms = math.floor(tonumber(timeout_ms) or 0)
      if timeout_ms > 60000 then error("input timeout is limited to 60 seconds", 2) end
      if not subscribed then ask("subscribe", "button"); subscribed = true end
      local action = next_event(timeout_ms)
      if not action then return nil end
      return { device = "speaker", type = "EV_KEY", code = pressed_code[action] or 0, value = action == "action_long" and 2 or 1, time_ms = now(), action = action }
    end,
    is_available = function() return true end,
  },
  http = {
    get = function(url) return http_request("GET", url, nil) end,
    post = function(url, body)
      if #tostring(body or "") > 1048576 then error("request body exceeds 1 MiB", 2) end
      return http_request("POST", url, body)
    end,
    is_available = function() return true end,
  },
}

-- the page of a plugin: its files travel with the package to the server, which shows them in a tab of the panel (/luna/web/<id>/...)
register = { customweb = function(path)
  path = tostring(path)
  if path:sub(1, 1) == "/" or path:find("..", 1, true) or not (path:match("%.html?$")) then error("custom web file must have .html or .htm extension", 2) end
  return "/luna/web/" .. (info.plugin or "") .. "/" .. path
end }

function print(...)
  local t = table.pack(...)
  for i = 1, t.n do t[i] = tostring(t[i]) end
  call("log", table.concat(t, " ", 1, t.n))
end

-- lostos: what the speaker offers beyond Luna's names
lostos = {
  log = print,
  sleep = alice.sleep,
  now = function() return now() end,
  media = function(cmd) ask("media", cmd) end,
  volume = function() return tonumber(ask("volume")) end,
  storage = {
    get = function(key) local v = ask("get", key) return v ~= "" and v or nil end,
    set = function(key, value) ask("set", key .. " " .. tostring(value)) end,
  },
}
