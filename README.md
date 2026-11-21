# Станция Nexi

Умная колонка с ассистентом **Nexi** на плате LCDWIKI ES3C28P (ESP32-S3, экран 2.8").
Прошивка — [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) с патчем Nexi (`firmware/patch_xiaozhi.py`):
связь с приложением Nexi на телефоне по Wi‑Fi/Bluetooth, слово «Некси», будильник, свой экран и настройки.

## Прошивка
1. Скачай `nexi-station-es3c28p.bin` из [Releases](../../releases/tag/station).
2. Открой в Chrome https://espressif.github.io/esptool-js/ → Connect → выбери порт колонки.
3. Файл с адреса `0x0` → Program. Стирать (Erase) не обязательно — Wi‑Fi и настройки сохранятся.

## Сборка
GitHub Actions (`.github/workflows/build.yml`) собирает прошивку при каждом изменении в `firmware/` и выкладывает её в релиз `station`.

## Файлы
- `firmware/patch_xiaozhi.py` — превращает xiaozhi-esp32 в Станцию Nexi
- `firmware/nexi_station.h` — связь с телефоном (BLE + TCP), будильник
- `firmware/nexi_ui.h` — экран: звезда, настройки, первая настройка касаний
- `firmware/nexi_star.h`, `make_star.py` — картинка звезды
