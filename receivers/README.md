# Приёмники lostOS: AirPlay 2 и Spotify Connect

Программы системных плагинов `ru.lostos.airplay` и `ru.lostos.spotify` для колонок с 32-битной системой (armhf, glibc 2.30: Станция Мини 3, Лайт 2, Лайт «Micro»). Установщик кладёт на колонку те, которые выбрал владелец, в `/data/lostos/plugins/<id>/`. Запускает и сторожит их сама lostos (`src/plugins/system*.cpp`).

- `armhf/ru.lostos.airplay/`: shairport-sync (AirPlay 2, GPL-3.0), nqptp (GPL-3.0), avahi-daemon (LGPL-2.1) и их библиотеки (`libs/`).
- `armhf/ru.lostos.spotify/`: librespot (MIT).
- `armhf/audio/`: эквалайзер (`satellite/alsa/lostoseq.c`, собранный под колонку) и два коротких звука переключателя AirPlay.

Файлы взяты с работающей Мини 3 как есть. Для Станции Миди (aarch64) пока нет сборки, там установщик приёмники не предлагает.
