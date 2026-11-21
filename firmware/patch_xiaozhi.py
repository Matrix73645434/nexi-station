"""Превращает xiaozhi-esp32 в «Станцию Nexi» (применяется в CI к свежей копии xiaozhi).
Телефон с Nexi подключён по Bluetooth — разговор через него; нет — как обычно через xiaozhi.me по Wi-Fi."""
import json, shutil, sys
from pathlib import Path

root = Path(sys.argv[1])
here = Path(__file__).parent
shutil.copy(here / "nexi_station.h", root / "main" / "nexi_station.h")
shutil.copy(here / "nexi_ota.h", root / "main" / "nexi_ota.h")


def patch(rel, old, new, count=1):
    p = root / rel
    s = p.read_text(encoding="utf-8")
    n = s.count(old)
    if n != count:
        sys.exit(f"patch failed: {rel}: найдено {n} раз, ждали {count}: {old[:60]!r}")
    p.write_text(s.replace(old, new), encoding="utf-8")


app = "main/application.cc"
patch(app, '#include "websocket_protocol.h"\n', '#include "websocket_protocol.h"\n#include "nexi_station.h"\n#include "nexi_ota.h"\n')
# Bluetooth — сразу при включении, ещё до Wi-Fi (телефон может подключиться заранее)
patch(app, "    // Start network asynchronously\n    board.StartNetwork();", "    nexi::ble::Start();\n    nexi::tcp::Start();\n    nexi::alarm::Start();\n    nexi::ota::Start();\n    // Start network asynchronously\n    board.StartNetwork();")
# протокол: телефон по Bluetooth или xiaozhi
patch(app, "    if (ota_->HasMqttConfig()) {\n        protocol_ = std::make_unique<MqttProtocol>();",
      "    nexi::ble::Start();\n"
      "    if (nexi::g_proto_phone) {\n        protocol_ = std::make_unique<nexi::NexiBleProtocol>();\n"
      "    } else if (ota_->HasMqttConfig()) {\n        protocol_ = std::make_unique<MqttProtocol>();")
# перед каждым разговором: телефон рядом — через него
patch(app, "    if (!protocol_->IsAudioChannelOpened()) {\n        if (!protocol_->OpenAudioChannel()) {",
      "    NexiSelectProtocol();\n    if (!protocol_->IsAudioChannelOpened()) {\n        if (!protocol_->OpenAudioChannel()) {", count=2)
# не обновляться «по воздуху» на обычный Xiaozhi — иначе станция Nexi пропадёт
patch(app, "        if (ota_->HasNewVersion()) {", "        if (false && ota_->HasNewVersion()) {")
p = root / app
p.write_text(p.read_text(encoding="utf-8") + '''

// Станция Nexi: телефон подключён по Bluetooth — разговор через него, нет — через xiaozhi.me
void Application::NexiSelectProtocol() {
    if (protocol_ && protocol_->IsAudioChannelOpened()) return;
    bool want = nexi::link_up();
    if (want != nexi::g_proto_phone || !protocol_) {
        nexi::g_proto_phone = want;
        InitializeProtocol();
    }
}
''', encoding="utf-8")
patch("main/application.h", "    void InitializeProtocol();", "    void InitializeProtocol();\n    void NexiSelectProtocol();")

# Bluetooth LE (NimBLE) рядом с Wi-Fi
cfg_path = root / "main/boards/quandong/esp32-s3-spi-2.8/config.json"
cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
for b in cfg["builds"]:
    b.setdefault("sdkconfig_append", []).extend([
        "CONFIG_BT_ENABLED=y",
        "CONFIG_BT_NIMBLE_ENABLED=y",
        "CONFIG_BT_CONTROLLER_ENABLED=y",
        "CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y",
        "CONFIG_BT_NIMBLE_ROLE_BROADCASTER=y",
        "CONFIG_BT_NIMBLE_ROLE_CENTRAL=n",
        "CONFIG_BT_NIMBLE_ROLE_OBSERVER=n",
        "CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1",
        "CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=512",
        "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y",
        "CONFIG_BT_NIMBLE_NVS_PERSIST=n",
        "CONFIG_BT_NIMBLE_SECURITY_ENABLE=n",
        "CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT=24",
        "CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y",
    ])
cfg_path.write_text(json.dumps(cfg, ensure_ascii=False, indent=4), encoding="utf-8")
# звезда Nexi на экране вместо смайликов
shutil.copy(here / "nexi_star.h", root / "main" / "display" / "nexi_star.h")
lcd = "main/display/lcd_display.cc"
patch(lcd, '#include "lcd_display.h"\n', '#include "lcd_display.h"\n#include "nexi_star.h"\n')
patch(lcd, """    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;""",
      """    {   // Станция Nexi: всегда звезда Nexi
        DisplayLockGuard lock(this);
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }
        lv_image_set_src(emoji_image_, &nexi_star);
        emotion_uses_image_ = true;
        ShowEmotion();
        return true;
    }
    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;""")

# слово «Некси» вместо «Hi ESP»: команда MultiNet7. Китайская модель «Некси» почти не слышала (в китайском нет «кс»),
# английская разбирает звуки как есть: NEXY ≈ «нек-си». Несколько вариантов через «;».
(root / "nexi_sdkconfig.txt").write_text("\n".join([
    "CONFIG_WAKE_WORD_DISABLED=n",
    "CONFIG_USE_ESP_WAKE_WORD=n",
    "CONFIG_USE_AFE_WAKE_WORD=n",
    "CONFIG_USE_CUSTOM_WAKE_WORD=y",
    'CONFIG_CUSTOM_WAKE_WORD="NEXY;NECK SEE;NIXIE;NEK SEE;NEXEE"',
    'CONFIG_CUSTOM_WAKE_WORD_DISPLAY="Некси"',
    "CONFIG_CUSTOM_WAKE_WORD_THRESHOLD=15",
    "CONFIG_SR_MN_CN_NONE=y",
    "CONFIG_SR_MN_EN_MULTINET7_QUANT=y",
]) + "\n", encoding="utf-8")
patch("scripts/build.py", """        sdkconfig_append = _merge_sdkconfig_options(
            sdkconfig_append,
            user_options,
        )""", """        _nexi = Path("nexi_sdkconfig.txt")
        if _nexi.exists():  # Станция Nexi: своё слово-активатор поверх выбранного
            user_options.extend(l.strip() for l in _nexi.read_text(encoding="utf-8").splitlines() if l.strip())
            validation_symbols = [v for v in validation_symbols if v[1] != "--wake-word"]
        sdkconfig_append = _merge_sdkconfig_options(
            sdkconfig_append,
            user_options,
        )""")
patch("scripts/build_default_assets.py", """            "commands": [
                {
                    "command": custom_wake_word_config['wake_word'],
                    "text": custom_wake_word_config['display'],
                    "action": "wake"
                }
            ]""", """            "commands": [
                {
                    "command": _w.strip(),
                    "text": custom_wake_word_config['display'],
                    "action": "wake"
                }
                for _w in custom_wake_word_config['wake_word'].split(';') if _w.strip()
            ]""")
patch("main/audio/wake_words/custom_wake_word.cc", """        commands_.push_back({CONFIG_CUSTOM_WAKE_WORD, CONFIG_CUSTOM_WAKE_WORD_DISPLAY, "wake"});""", """        {
            std::string all = CONFIG_CUSTOM_WAKE_WORD;
            size_t a = 0;
            while (a <= all.size()) {
                size_t b = all.find(';', a);
                if (b == std::string::npos) b = all.size();
                std::string w = all.substr(a, b - a);
                while (!w.empty() && w.front() == ' ') w.erase(0, 1);
                while (!w.empty() && w.back() == ' ') w.pop_back();
                if (!w.empty()) commands_.push_back({w, CONFIG_CUSTOM_WAKE_WORD_DISPLAY, "wake"});
                a = b + 1;
            }
        }""")

# сенсорный экран и кнопка настроек колонки
_board = "main/boards/quandong/esp32-s3-spi-2.8/quandong_s3_dev_board.cc"
shutil.copy(here / "nexi_ui.h", root / "main" / "nexi_ui.h")
# экран вертикально (240×320), как у телефона: микрофон сверху
patch("main/boards/quandong/esp32-s3-spi-2.8/config.h", """#define DISPLAY_WIDTH   320
#define DISPLAY_HEIGHT  240
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y true
#define DISPLAY_SWAP_XY true
""", """#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  320
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY false
""")
patch(_board, "        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);", "        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);\n        nexi::ui::lcd = panel;")
patch(_board, '#include "config.h"', '#include "config.h"\n#include "nexi_ui.h"')
patch(_board, """        GetBacklight()->SetBrightness(100);
    }""", """        GetBacklight()->RestoreBrightness();
        nexi::ui::Init(codec_i2c_bus_);
    }""")

print("xiaozhi → станция Nexi (Bluetooth): ok")

# обновление по воздуху (nexi_ota.h): HTTPS-клиент и OTA из ESP-IDF
patch("main/CMakeLists.txt", "                        app_update\n", "                        app_update\n                        esp_http_client\n                        esp_https_ota\n")
