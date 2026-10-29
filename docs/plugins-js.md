# Плагины на JavaScript (работают на колонке)

Плагин на JavaScript работает **на самой колонке**, а не на сервере. Сервер его хранит, проверяет подпись, ставит на колонку и передаёт команды ассистента. Своего процесса на сервере у такого плагина нет, поэтому серверу он ничего не стоит, сколько бы домов его ни поставили.

На колонке плагин запускает общая среда `lostos-js` (QuickJS-ng, около 670 КБ, лежит в `/data/lostos/lostos-js`) в той же песочнице, что и нативные плагины: свой пользователь, `chroot` в пустую папку, лимиты памяти и процессора, фильтр seccomp. Плагин не может открыть файл или сокет: всё, что он делает с колонкой и сетью, идёт запросом к прошивке, а прошивка сверяет его с разрешениями. Обычный плагин занимает **меньше 1 МБ** памяти колонки (радио: 840 КБ).

## Пакет

```
ru.example.radio/
  manifest.json
  README.md            описание для страницы плагина в панели
  device/main.js       код плагина (ES-модуль: можно await на верхнем уровне)
```

```json
{
 "id": "ru.example.radio", "name": "Радио", "version": "1.0.0", "author": "Вы", "api": "1",
 "description": "Одна строка для списка плагинов",
 "permissions": [],
 "side": "speaker",
 "min_firmware": "0.3.6",
 "device": { "runtime": "js", "binary": "device/main.js", "permissions": ["network.connect", "audio.play", "assistant.tools", "storage"] }
}
```

`entry` (скрипт для сервера) не нужен. `min_firmware: 0.3.6`: прошивки старше не умеют запускать JavaScript.

## API (`lostos`)

| Вызов | Разрешение | Что делает |
|---|---|---|
| `lostos.tool({name, description, parameters}, fn)` | `assistant.tools` | команда для ассистента: нейросеть сама решает, когда её вызвать и с какими аргументами. `fn(args)` возвращает строку, которую ассистент перескажет. `parameters`: `{имя: "описание"}` или `{имя: {type, description, optional}}` |
| `lostos.http(url, {method, headers, body, json})` | `network.connect` | HTTP(S)-запрос делает прошивка (её TLS и сертификаты). Ответ: `{status, ok, type, text, json()}`, тело до 256 КБ. Адреса самой колонки закрыты |
| `lostos.http.json(url, opts)` | `network.connect` | то же, сразу JSON; бросает ошибку, если статус не 2xx |
| `lostos.play(url, {title})`, `lostos.stop()` | `audio.play` | поток или файл играет плеер колонки (канал музыки) |
| `lostos.storage.get(key)`, `.set(key, value)` | `storage` | до 32 записей по 256 байт, переживают перезагрузку |
| `lostos.on("button" \| "media" \| "volume", fn)` | `events.subscribe` | события: `action`, `action_long`, `volume_up`, `volume_down`; `{source, playing, title}`; громкость 0..1 |
| `lostos.led(r, g, b, секунд)` | `device.led` | кольцо загорается цветом |
| `lostos.display(текст, секунд)` | `device.display` | текст на часах |
| `lostos.media("play" \| "pause" \| "next" …)` | `media.control` | управление музыкой |
| `lostos.volume()` | `device.read` | громкость 0..1 |
| `lostos.publish(тема, текст)` | `events.publish` | событие для Home Assistant |
| `lostos.log(...)`, `console.log(...)` | — | строка в журнале колонки |
| `setTimeout`, `setInterval`, `clearTimeout`, `lostos.sleep(ms)` | — | таймеры |

Пример: [server/plugins/user/ru.lostos.radio/device/main.js](../server/plugins/user/ru.lostos.radio/device/main.js).

## Лимиты

- 24 МБ памяти JavaScript на плагин (вся песочница 64 МБ адресного пространства).
- Код, который не отпускает управление дольше 5 секунд, прерывается; плагин продолжает работать, а вызов команды получает ошибку.
- До 4 HTTP-запросов одновременно, до 16 команд, скрипт до 1 МБ.
- Плагин, который падает трижды подряд или трижды просит запрещённое, выключается; это видно на его странице.

## Проверка на своём компьютере

Нужен собранный `lostos-js` (`sh satellite/plugin-runtime/build.sh`, для ПК: `... build x86_64-linux-musl`) и Python 3:

```bash
python3 satellite/plugin-runtime/run_plugin.py build/lostos-js-x86_64-linux-musl device/main.js play_radio '{"query":"jazz"}'
```

Скрипт отвечает плагину вместо прошивки: хранилище в памяти, настоящие HTTP-запросы, `play`/`stop` только печатаются. В конце печатает ответ команды.

## Как это устроено внутри

- Колонка (`satellite/src/plugins/host.cpp`): пакет с `"runtime": "js"` ставится командой `install <id> <url> <sha256> <разрешения> <база> js`; `main.js` кладётся в папку плагина, программой становится жёсткая ссылка на `/data/lostos/lostos-js`. Подпись пакета колонка проверяет сама (`trust.cpp`), как у нативных плагинов: хеш `main.js` должен быть в подписанной таблице файлов. Скрипт уходит в песочницу первым сообщением (`! script <байт>`), потому что сама песочница файлы не открывает.
- Команды ассистента: колонка пишет их в датчик `plugins_state` строками `id<TAB>tool<TAB><json>`, сервер (`plugins.py`, `device_report`) отдаёт их нейросети вместе с командами серверных плагинов. Вызов уходит на колонку командой `plugin call <id> <id вызова> <base64 json>`, ответ возвращается строкой `id<TAB>result<TAB><id вызова><TAB><json-строка>`.
- Среда (`satellite/plugin-runtime/`): `lostos-js.c` (цикл событий, протокол, таймеры, прерывание зависших задач) и `prelude.js` (API выше). Тесты: `test_runtime.py` (весь протокол, в том числе под `qemu-arm` для ARM-сборки).

## Сеть: что важно знать

Колонка ходит в интернет напрямую, без VPN компьютера. В некоторых сетях TLS-соединения к части хостингов (Hetzner, AWS) замирают после первых ~16 КБ ответа, а `api.telegram.org` недоступен совсем. Поэтому радио берёт каталог по обычному HTTP. Плагину, которому нужен заблокированный сервис, понадобится свой прокси.
