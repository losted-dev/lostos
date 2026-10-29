// lostos-js: runs a JavaScript plugin of the speaker (QuickJS-ng) inside the sandbox of plugins/sandbox.h.
//
// It is the static program of a JS plugin: the firmware starts it like any native plugin (chroot, its own user, rlimits, seccomp) and talks
// to it over fd 3 with the line protocol of plugins/host.h. The sandbox cannot open files, so the plugin's code comes over the same
// descriptor as the first message:   "! script <bytes>\n" followed by that many bytes of JavaScript (an ES module).
// Everything the script may do goes through that descriptor as a request the firmware checks against the permissions of the plugin;
// the API the script sees (globalThis.lostos) is built in prelude.js on top of four functions of this file:
//     __host.call(method, args) -> Promise<string>      one request, resolved by "<n> ok <result>", rejected by "<n> err <why>"
//     __host.timer(ms, repeat) -> id, __host.clear(id)   timers; when one is due, globalThis.__timer(id) runs
//     __host.now() -> ms since the start
// and events "! <name> <data>" from the firmware are handed to globalThis.__event(name, data).
// Limits of its own on top of the sandbox's: 24 MB of JavaScript heap, a job that runs for more than 5 s is interrupted.
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "quickjs.h"

extern const char prelude_js[];
extern const unsigned prelude_js_len;

enum { CTL = 3, MAX_TIMERS = 256, MAX_CALLS = 1024 };
static const size_t kHeap = 24u << 20, kStack = 256u << 10, kMaxScript = 1u << 20, kMaxIn = 4u << 20;
static const int64_t kJobMs = 5000;

static JSRuntime *rt;
static JSContext *ctx;
static int64_t t0, job_started = -1;   // -1: no JavaScript is running
static bool interrupted;                // the last exception came from the interrupt handler
static void aborted(void);

struct call { uint32_t id; JSValue resolve, reject; };
static struct call calls[MAX_CALLS];
static uint32_t next_call = 1;

struct timer { int32_t id; int64_t due, every; };
static struct timer timers[MAX_TIMERS];
static int32_t next_timer = 1;

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 - t0;
}

static int write_all(const char *p, size_t n) {
  while (n > 0) {
    ssize_t w = send(CTL, p, n, MSG_NOSIGNAL);
    if (w < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN) { struct pollfd pf = {CTL, POLLOUT, 0}; poll(&pf, 1, 1000); continue; }
      return -1;
    }
    p += w, n -= (size_t)w;
  }
  return 0;
}

// One request line; newlines in the arguments would split it, they become spaces.
static int send_request(uint32_t id, const char *method, const char *args, size_t alen) {
  char head[64];
  int hn = snprintf(head, sizeof head, "%u %s%s", id, method, alen ? " " : "");
  char *line = malloc((size_t)hn + alen + 1);
  if (!line) return -1;
  memcpy(line, head, (size_t)hn);
  for (size_t i = 0; i < alen; i++) line[hn + i] = (args[i] == '\n' || args[i] == '\r') ? ' ' : args[i];
  line[hn + alen] = '\n';
  int r = write_all(line, (size_t)hn + alen + 1);
  free(line);
  return r;
}

static void report(const char *what) {   // a line in the firmware's log
  send_request(0, "log", what, strlen(what) > 900 ? 900 : strlen(what));
}

static void report_exception(void) {
  interrupted = false;
  JSValue e = JS_GetException(ctx);
  const char *msg = JS_ToCString(ctx, e);
  JSValue stack = JS_IsError(ctx, e) ? JS_GetPropertyStr(ctx, e, "stack") : JS_UNDEFINED;
  const char *st = JS_IsUndefined(stack) ? NULL : JS_ToCString(ctx, stack);
  char buf[900];
  snprintf(buf, sizeof buf, "error: %s%s%s", msg ? msg : "?", st ? " | " : "", st ? st : "");
  report(buf);
  if (st) JS_FreeCString(ctx, st);
  if (msg) JS_FreeCString(ctx, msg);
  JS_FreeValue(ctx, stack);
  JS_FreeValue(ctx, e);
}

static int interrupt(JSRuntime *r, void *opaque) {
  (void)r, (void)opaque;
  if (job_started >= 0 && now_ms() - job_started > kJobMs) {   // a loop that never yields: interrupted, the plugin keeps running
    interrupted = true;
    return 1;
  }
  return 0;
}

static void rejection(JSContext *c, JSValue promise, JSValue reason, bool handled, void *opaque) {
  (void)promise, (void)opaque;
  if (handled) return;
  const char *msg = JS_ToCString(c, reason);
  char buf[600];
  snprintf(buf, sizeof buf, "unhandled rejection: %s", msg ? msg : "?");
  report(buf);
  if (msg) JS_FreeCString(c, msg);
}

static void run_jobs(void) {
  JSContext *c;
  for (;;) {
    job_started = now_ms();
    int r = JS_ExecutePendingJob(rt, &c);
    job_started = -1;
    if (r <= 0) {
      if (r < 0) {
        bool hung = interrupted;
        report_exception();
        if (hung) aborted();
        continue;                                                  // the other jobs still run
      }
      break;
    }
  }
}

static void call_global(const char *fn, int argc, JSValue *argv);

// After an interrupted job: the prelude answers the command that was running (see __aborted in prelude.js).
static void aborted(void) {
  static bool inside;
  if (inside) return;
  inside = true;
  call_global("__aborted", 0, NULL);
  inside = false;
}

static void call_global(const char *fn, int argc, JSValue *argv) {
  JSValue g = JS_GetGlobalObject(ctx), f = JS_GetPropertyStr(ctx, g, fn);
  if (JS_IsFunction(ctx, f)) {
    job_started = now_ms();
    JSValue r = JS_Call(ctx, f, g, argc, argv);
    job_started = -1;
    if (JS_IsException(r)) {
      bool hung = interrupted;
      report_exception();
      if (hung) aborted();
    }
    JS_FreeValue(ctx, r);
  }
  JS_FreeValue(ctx, f);
  JS_FreeValue(ctx, g);
  run_jobs();
}

// __host.call(method, args) -> Promise<string>
static JSValue host_call(JSContext *c, JSValue this_val, int argc, JSValue *argv) {
  (void)this_val;
  if (argc < 1) return JS_ThrowTypeError(c, "call(method, args)");
  const char *method = JS_ToCString(c, argv[0]);
  size_t alen = 0;
  const char *args = argc > 1 && !JS_IsUndefined(argv[1]) ? JS_ToCStringLen(c, &alen, argv[1]) : NULL;
  int slot = -1;
  for (int i = 0; i < MAX_CALLS; i++)
    if (!calls[i].id) { slot = i; break; }
  JSValue funcs[2], promise = JS_UNDEFINED;
  if (!method || slot < 0) {
    promise = JS_ThrowRangeError(c, "too many requests at once");
  } else {
    promise = JS_NewPromiseCapability(c, funcs);
    uint32_t id = next_call++;
    if (!next_call) next_call = 1;
    calls[slot] = (struct call){id, funcs[0], funcs[1]};
    if (send_request(id, method, args ? args : "", args ? alen : 0) != 0) exit(0);   // the firmware is gone
  }
  if (method) JS_FreeCString(c, method);
  if (args) JS_FreeCString(c, args);
  return promise;
}

static JSValue host_timer(JSContext *c, JSValue this_val, int argc, JSValue *argv) {
  (void)this_val;
  int64_t ms = 0;
  if (argc > 0) JS_ToInt64(c, &ms, argv[0]);
  if (ms < 0) ms = 0;
  bool repeat = argc > 1 && JS_ToBool(c, argv[1]);
  for (int i = 0; i < MAX_TIMERS; i++)
    if (!timers[i].id) {
      timers[i] = (struct timer){next_timer++, now_ms() + ms, repeat ? (ms < 10 ? 10 : ms) : 0};
      if (next_timer <= 0) next_timer = 1;
      return JS_NewInt32(c, timers[i].id);
    }
  return JS_ThrowRangeError(c, "too many timers");
}

static JSValue host_clear(JSContext *c, JSValue this_val, int argc, JSValue *argv) {
  (void)this_val;
  int32_t id = 0;
  if (argc > 0) JS_ToInt32(c, &id, argv[0]);
  for (int i = 0; i < MAX_TIMERS; i++)
    if (timers[i].id == id && id) timers[i].id = 0;
  return JS_UNDEFINED;
}

static JSValue host_now(JSContext *c, JSValue this_val, int argc, JSValue *argv) {
  (void)this_val, (void)argc, (void)argv;
  return JS_NewInt64(c, now_ms());
}

static void answer(uint32_t id, bool ok, const char *res, size_t len) {
  for (int i = 0; i < MAX_CALLS; i++) {
    if (calls[i].id != id) continue;
    JSValue v = ok ? JS_NewStringLen(ctx, res, len) : JS_NewError(ctx);
    if (!ok) JS_SetPropertyStr(ctx, v, "message", JS_NewStringLen(ctx, res, len));
    JSValue r = JS_Call(ctx, ok ? calls[i].resolve : calls[i].reject, JS_UNDEFINED, 1, &v);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, calls[i].resolve);
    JS_FreeValue(ctx, calls[i].reject);
    calls[i].id = 0;
    run_jobs();
    return;
  }
}

static void line_in(char *s, size_t n) {
  if (n >= 2 && s[0] == '!' && s[1] == ' ') {                     // "! <event> <data>"
    char *name = s + 2, *sp = memchr(name, ' ', n - 2);
    size_t nl = sp ? (size_t)(sp - name) : n - 2;
    JSValue a[2] = {JS_NewStringLen(ctx, name, nl), sp ? JS_NewStringLen(ctx, sp + 1, n - (size_t)(sp + 1 - s)) : JS_NewString(ctx, "")};
    call_global("__event", 2, a);
    JS_FreeValue(ctx, a[0]);
    JS_FreeValue(ctx, a[1]);
    return;
  }
  char *sp = memchr(s, ' ', n);                                    // "<n> ok <result>" | "<n> err <why>"
  if (!sp) return;
  uint32_t id = (uint32_t)strtoul(s, NULL, 10);
  char *word = sp + 1, *sp2 = memchr(word, ' ', n - (size_t)(word - s));
  size_t wl = sp2 ? (size_t)(sp2 - word) : n - (size_t)(word - s);
  const char *res = sp2 ? sp2 + 1 : "";
  size_t rl = sp2 ? n - (size_t)(sp2 + 1 - s) : 0;
  if (id) answer(id, wl == 2 && !memcmp(word, "ok", 2), res, rl);
}

static void fire_timers(void) {
  int64_t t = now_ms();
  for (int i = 0; i < MAX_TIMERS; i++) {
    if (!timers[i].id || timers[i].due > t) continue;
    int32_t id = timers[i].id;
    if (timers[i].every) timers[i].due = t + timers[i].every;
    else timers[i].id = 0;
    JSValue a = JS_NewInt32(ctx, id);
    call_global("__timer", 1, &a);
  }
}

static int next_timeout(void) {
  int64_t best = -1, t = now_ms();
  for (int i = 0; i < MAX_TIMERS; i++)
    if (timers[i].id && (best < 0 || timers[i].due < best)) best = timers[i].due;
  if (best < 0) return -1;
  return best <= t ? 0 : (int)(best - t > 60000 ? 60000 : best - t);
}

// Reads exactly n bytes (the script) after the first line.
static int read_exact(char *dst, size_t n, char *have, size_t *have_n) {
  size_t got = *have_n < n ? *have_n : n;
  memcpy(dst, have, got);
  memmove(have, have + got, *have_n - got);
  *have_n -= got;
  while (got < n) {
    ssize_t r = recv(CTL, dst + got, n - got, 0);
    if (r <= 0) return -1;
    got += (size_t)r;
  }
  return 0;
}

static JSValue eval(const char *src, size_t n, const char *name, int flags) {
  job_started = now_ms();
  JSValue v = JS_Eval(ctx, src, n, name, flags);
  job_started = -1;
  return v;
}

int main(void) {
  setenv("TZ", "UTC0", 1);   // musl would open /etc/localtime otherwise, and the sandbox kills a process that opens a file
  t0 = 0;
  t0 = now_ms();
  static char in[1 << 16];
  size_t in_n = 0;
  char *nl;
  while (!(nl = memchr(in, '\n', in_n))) {                         // the first message: "! script <bytes>"
    ssize_t r = recv(CTL, in + in_n, sizeof in - in_n, 0);
    if (r <= 0 || in_n + (size_t)r >= sizeof in) return 1;
    in_n += (size_t)r;
  }
  *nl = 0;
  unsigned long len = 0;
  if (sscanf(in, "! script %lu", &len) != 1 || len == 0 || len > kMaxScript) return 2;
  size_t rest = in_n - (size_t)(nl + 1 - in);
  memmove(in, nl + 1, rest);
  in_n = rest;
  char *script = malloc(len + 1);
  if (!script || read_exact(script, len, in, &in_n) != 0) return 3;
  script[len] = 0;

  rt = JS_NewRuntime();
  JS_SetMemoryLimit(rt, kHeap);
  JS_SetMaxStackSize(rt, kStack);
  JS_SetInterruptHandler(rt, interrupt, NULL);
  JS_SetHostPromiseRejectionTracker(rt, rejection, NULL);
  ctx = JS_NewContext(rt);
  JSValue g = JS_GetGlobalObject(ctx), host = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, host, "call", JS_NewCFunction(ctx, host_call, "call", 2));
  JS_SetPropertyStr(ctx, host, "timer", JS_NewCFunction(ctx, host_timer, "timer", 2));
  JS_SetPropertyStr(ctx, host, "clear", JS_NewCFunction(ctx, host_clear, "clear", 1));
  JS_SetPropertyStr(ctx, host, "now", JS_NewCFunction(ctx, host_now, "now", 0));
  JS_SetPropertyStr(ctx, g, "__host", host);
  JS_FreeValue(ctx, g);

  JSValue v = eval(prelude_js, prelude_js_len, "<prelude>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(v)) { report_exception(); return 4; }
  JS_FreeValue(ctx, v);
  JSValue module = eval(script, len, "main.js", JS_EVAL_TYPE_MODULE);   // a module: top-level await, import of nothing else
  free(script);
  if (JS_IsException(module)) {
    report_exception();
    module = JS_UNDEFINED;
  }
  run_jobs();
  bool started = false;                                            // "started" once the module has run to its end (its awaits included)

  static char *buf;
  size_t cap = 1 << 16, n = 0;
  buf = malloc(cap);
  if (in_n) { memcpy(buf, in, in_n); n = in_n; }
  for (;;) {
    char *p;
    while ((p = memchr(buf, '\n', n))) {
      size_t ln = (size_t)(p - buf);
      line_in(buf, ln);
      memmove(buf, p + 1, n - ln - 1);
      n -= ln + 1;
    }
    fire_timers();
    if (!started && (!JS_IsPromise(module) || JS_PromiseState(ctx, module) != JS_PROMISE_PENDING)) {
      started = true;
      if (JS_IsPromise(module) && JS_PromiseState(ctx, module) == JS_PROMISE_REJECTED) {
        JSValue why = JS_PromiseResult(ctx, module);
        JS_Throw(ctx, why);
        report_exception();
      } else {
        report("started");
      }
      JS_FreeValue(ctx, module);
      module = JS_UNDEFINED;
    }
    struct pollfd pf = {CTL, POLLIN, 0};
    int r = poll(&pf, 1, next_timeout());
    if (r < 0 && errno != EINTR) return 5;
    if (r > 0) {
      if (n + 65536 > cap) {
        if (cap >= kMaxIn) return 6;                               // a message that never ends
        cap *= 2;
        buf = realloc(buf, cap);
        if (!buf) return 7;
      }
      ssize_t got = recv(CTL, buf + n, cap - n, 0);
      if (got <= 0) return 0;                                      // the firmware closed: the plugin is stopped
      n += (size_t)got;
    }
  }
}
