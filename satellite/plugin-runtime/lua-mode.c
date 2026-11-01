// The second language of lostos-js: Lua 5.4, for the plugins written for the Luna framework (docs/plugins-lua.md).
// A script whose first line is "--lua" is run here instead of the JavaScript engine; the packager of the server writes it so (the file stays main.js).
// A Lua plugin is a program that runs top to bottom and may wait (alice.sleep): so a request to the firmware is blocking here, it reads the descriptor
// until its answer comes (events of the firmware are dropped meanwhile; Lua plugins have no events). Included by lostos-js.c.
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

extern const char prelude_lua[];
extern const unsigned prelude_lua_len;

static const size_t kLuaHeap = 12u << 20;
static const int64_t kLuaIdleMs = 60000;   // a script that neither asks the firmware nor sleeps for this long is stopped (an endless loop)
static size_t lua_used;
static int64_t lua_alive;
static char lua_in[1 << 16];
static size_t lua_in_n;

static void *lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
  (void)ud;
  if (!ptr) osize = 0;
  if (nsize == 0) { free(ptr); lua_used -= osize; return NULL; }
  if (lua_used - osize + nsize > kLuaHeap) return NULL;
  void *r = realloc(ptr, nsize);
  if (r) lua_used += nsize - osize;
  return r;
}

// Events of the firmware that a script asks for with __next_event: "! button <action>" (a small queue, the oldest are dropped).
static char lua_ev[8][64];
static int lua_ev_n;
static bool lua_ev_wait;   // __next_event is waiting: lua_wait returns when an event came

static void lua_ev_push(const char *s, size_t n) {
  if (n < 10 || memcmp(s, "! button ", 9) != 0) return;
  if (lua_ev_n == 8) { memmove(lua_ev[0], lua_ev[1], 7 * 64); lua_ev_n--; }
  size_t l = n - 9 < 63 ? n - 9 : 63;
  memcpy(lua_ev[lua_ev_n], s + 9, l);
  lua_ev[lua_ev_n++][l] = 0;
}

// Reads what the firmware sent; returns the id and text of an answer line, or 0 when the line was something else (dropped).
static uint32_t lua_line(char *s, size_t n, bool *ok, const char **res, size_t *rl) {
  if (n >= 2 && s[0] == '!') return 0;
  char *sp = memchr(s, ' ', n);
  if (!sp) return 0;
  uint32_t id = (uint32_t)strtoul(s, NULL, 10);
  char *word = sp + 1, *sp2 = memchr(word, ' ', n - (size_t)(word - s));
  size_t wl = sp2 ? (size_t)(sp2 - word) : n - (size_t)(word - s);
  *ok = wl == 2 && !memcmp(word, "ok", 2);
  *res = sp2 ? sp2 + 1 : "";
  *rl = sp2 ? n - (size_t)(sp2 + 1 - s) : 0;
  return id;
}

// Waits until `until` (ms, -1: for the answer `want` only). Returns 1 when answer `want` came (its text in lua_res), 0 on timeout. The firmware closing ends the process.
static char lua_res[16384];
static size_t lua_res_n;
static bool lua_res_ok;

static int lua_wait(uint32_t want, int64_t until) {
  for (;;) {
    char *p;
    while ((p = memchr(lua_in, '\n', lua_in_n))) {
      size_t ln = (size_t)(p - lua_in);
      bool ok = false;
      const char *res = "";
      size_t rl = 0;
      lua_ev_push(lua_in, ln);
      uint32_t id = lua_line(lua_in, ln, &ok, &res, &rl);
      if (want && id == want) {
        lua_res_n = rl < sizeof lua_res - 1 ? rl : sizeof lua_res - 1;
        memcpy(lua_res, res, lua_res_n);
        lua_res_ok = ok;
        memmove(lua_in, p + 1, lua_in_n - ln - 1);
        lua_in_n -= ln + 1;
        return 1;
      }
      memmove(lua_in, p + 1, lua_in_n - ln - 1);
      lua_in_n -= ln + 1;
    }
    if (lua_ev_wait && lua_ev_n) return 2;
    if (lua_in_n >= sizeof lua_in - 1) lua_in_n = 0;               // a line that never ends
    int64_t left = until < 0 ? 5000 : until - now_ms();
    if (until >= 0 && left <= 0) return 0;
    struct pollfd pf = {CTL, POLLIN, 0};
    int r = poll(&pf, 1, left > 5000 ? 5000 : (int)left);
    if (r < 0 && errno != EINTR) exit(5);
    if (r > 0) {
      ssize_t got = recv(CTL, lua_in + lua_in_n, sizeof lua_in - 1 - lua_in_n, 0);
      if (got <= 0) exit(0);                                       // the firmware closed: the plugin is stopped
      lua_in_n += (size_t)got;
    }
    if (until < 0 && now_ms() - lua_alive > 30000) return 0;       // no answer for 30 s
  }
}

// __call(method, args) -> result | nil, why
static int l_call(lua_State *L) {
  size_t mn = 0, an = 0;
  const char *method = luaL_checklstring(L, 1, &mn), *args = luaL_optlstring(L, 2, "", &an);
  lua_alive = now_ms();
  uint32_t id = next_call++;
  if (!next_call) next_call = 1;
  if (send_request(id, method, args, an) != 0) exit(0);
  if (!lua_wait(id, -1)) {
    lua_pushnil(L);
    lua_pushliteral(L, "нет ответа");
    return 2;
  }
  if (lua_res_ok) {
    lua_pushlstring(L, lua_res, lua_res_n);
    return 1;
  }
  lua_pushnil(L);
  lua_pushlstring(L, lua_res, lua_res_n);
  return 2;
}

static int l_sleep(lua_State *L) {
  lua_Integer ms = luaL_checkinteger(L, 1);
  if (ms < 0) ms = 0;
  lua_alive = now_ms();
  lua_wait(0, now_ms() + ms);
  lua_alive = now_ms();
  return 0;
}

// __next_event(timeout ms) -> action | nil
static int l_next_event(lua_State *L) {
  lua_Integer ms = luaL_optinteger(L, 1, 1000);
  if (ms < 0) ms = 0;
  if (ms > 60000) ms = 60000;
  lua_alive = now_ms();
  if (!lua_ev_n) {
    lua_ev_wait = true;
    lua_wait(0, now_ms() + ms);
    lua_ev_wait = false;
  }
  lua_alive = now_ms();
  if (!lua_ev_n) return 0;
  lua_pushstring(L, lua_ev[0]);
  memmove(lua_ev[0], lua_ev[1], (size_t)(--lua_ev_n) * 64);
  return 1;
}

static int l_now(lua_State *L) {
  lua_pushinteger(L, now_ms());
  return 1;
}

static void lua_watch(lua_State *L, lua_Debug *d) {
  (void)d;
  if (now_ms() - lua_alive > kLuaIdleMs) luaL_error(L, "скрипт работал слишком долго без обращений к станции и был остановлен");
}

static int run_lua(char *script, size_t len) {
  lua_State *L = lua_newstate(lua_alloc, NULL);
  if (!L) return 4;
  static const luaL_Reg libs[] = {{"_G", luaopen_base}, {LUA_TABLIBNAME, luaopen_table}, {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},
                                  {LUA_UTF8LIBNAME, luaopen_utf8}, {LUA_COLIBNAME, luaopen_coroutine}, {NULL, NULL}};
  for (const luaL_Reg *l = libs; l->func; l++) {
    luaL_requiref(L, l->name, l->func, 1);
    lua_pop(L, 1);
  }
  static const char *gone[] = {"dofile", "loadfile", "collectgarbage", NULL};   // the sandbox has no files; the collector is ours
  for (int i = 0; gone[i]; i++) {
    lua_pushnil(L);
    lua_setglobal(L, gone[i]);
  }
  lua_pushcfunction(L, l_call), lua_setglobal(L, "__call");
  lua_pushcfunction(L, l_sleep), lua_setglobal(L, "__sleep");
  lua_pushcfunction(L, l_now), lua_setglobal(L, "__now");
  lua_pushcfunction(L, l_next_event), lua_setglobal(L, "__next_event");
  lua_alive = now_ms();
  lua_sethook(L, lua_watch, LUA_MASKCOUNT, 1000000);
  if (luaL_loadbufferx(L, prelude_lua, prelude_lua_len, "=prelude", "t") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
    char buf[600];
    snprintf(buf, sizeof buf, "error: prelude: %s", lua_tostring(L, -1));
    report(buf);
    return 4;
  }
  if (luaL_loadbufferx(L, script, len, "=main.lua", "t") != LUA_OK) {
    char buf[900];
    snprintf(buf, sizeof buf, "error: %s", lua_tostring(L, -1));
    report(buf);
    for (;;) lua_wait(0, now_ms() + 5000);
  }
  free(script);
  report("started");
  if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
    char buf[900];
    snprintf(buf, sizeof buf, "error: %s", lua_tostring(L, -1));
    report(buf);
  }
  for (;;) lua_wait(0, now_ms() + 5000);                          // finished: stays until the firmware stops it (a script that ends is not restarted over and over)
}
