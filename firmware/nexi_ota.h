// Станция Nexi: обновление прошивки «по воздуху» с GitHub (Matrix73645434/nexi-station, релиз «station»).
// Раз в 6 часов (и через 2 минуты после включения) колонка читает ota.json; если версия новее — качает
// nexi-station-ota.bin во второй слот памяти (ota_0/ota_1) и перезагружается. Новая прошивка, которая не
// дожила до подключения к Wi-Fi, откатывается загрузчиком на старую (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE).
#pragma once

#include <cstdlib>
#include <cstring>
#include <string>

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if __has_include("nexi_version.h")
#include "nexi_version.h"
#endif
#ifndef NEXI_FW_VERSION
#define NEXI_FW_VERSION 0
#endif

namespace nexi {
namespace ota {

inline constexpr const char* TAG = "NexiOTA";
inline constexpr const char* MANIFEST = "https://github.com/Matrix73645434/nexi-station/releases/download/station/ota.json";
inline volatile int progress = -1;   // -1 — не обновляемся, 0..100 — качаем

inline bool wifi_up() {
    esp_netif_t* n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip{};
    return n && esp_netif_is_netif_up(n) && esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr != 0;
}

struct Body { std::string s; };
inline esp_err_t on_http(esp_http_client_event_t* e) {
    if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data) {
        auto* b = static_cast<Body*>(e->user_data);
        if (b->s.size() < 4096) b->s.append((const char*)e->data, e->data_len);
    }
    return ESP_OK;
}

inline esp_http_client_config_t http_cfg(const char* url) {
    esp_http_client_config_t c{};
    c.url = url;
    c.crt_bundle_attach = esp_crt_bundle_attach;
    c.timeout_ms = 20000;
    c.buffer_size = 4096;
    c.buffer_size_tx = 4096;          // ссылки на файлы GitHub после переадресации очень длинные
    c.max_redirection_count = 6;
    c.keep_alive_enable = true;
    return c;
}

// {"version": 12, "url": "https://…/nexi-station-ota.bin"}
inline bool fetch_manifest(int& ver, std::string& url) {
    Body body;
    auto c = http_cfg(MANIFEST);
    c.event_handler = on_http;
    c.user_data = &body;
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return false;
    esp_err_t err = esp_http_client_perform(h);
    int code = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (err != ESP_OK || code != 200) { ESP_LOGW(TAG, "manifest: %s, HTTP %d", esp_err_to_name(err), code); return false; }
    cJSON* j = cJSON_Parse(body.s.c_str());
    if (!j) return false;
    cJSON* v = cJSON_GetObjectItem(j, "version");
    cJSON* u = cJSON_GetObjectItem(j, "url");
    bool ok = cJSON_IsNumber(v) && cJSON_IsString(u);
    if (ok) { ver = v->valueint; url = u->valuestring; }
    cJSON_Delete(j);
    return ok;
}

inline void status(const char* s) {
    auto* d = Board::GetInstance().GetDisplay();
    if (d) d->SetChatMessage("system", s);
}

inline bool install(const std::string& url, int ver) {
    ESP_LOGI(TAG, "update %d -> %d: %s", NEXI_FW_VERSION, ver, url.c_str());
    auto c = http_cfg(url.c_str());
    esp_https_ota_config_t oc{};
    oc.http_config = &c;
    esp_https_ota_handle_t h = nullptr;
    if (esp_https_ota_begin(&oc, &h) != ESP_OK || !h) { ESP_LOGE(TAG, "begin failed"); return false; }
    int total = esp_https_ota_get_image_size(h);
    progress = 0;
    int shown = -1;
    esp_err_t err;
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int got = esp_https_ota_get_image_len_read(h);
        if (total > 0) progress = got * 100 / total;
        if (progress / 5 != shown / 5) {
            shown = progress;
            char b[64];
            snprintf(b, sizeof(b), "Обновляюсь… %d%%", progress);
            status(b);
        }
    }
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        ESP_LOGE(TAG, "download failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(h);
        progress = -1;
        status("");
        return false;
    }
    if (esp_https_ota_finish(h) != ESP_OK) { ESP_LOGE(TAG, "finish failed (битый файл?)"); progress = -1; status(""); return false; }
    status("Обновился! Перезагружаюсь…");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return true;
}

inline void task(void*) {
    // новая прошивка проработала минуту и подключилась к Wi-Fi — значит рабочая, откат больше не нужен
    for (int i = 0; i < 120 && !wifi_up(); i++) vTaskDelay(pdMS_TO_TICKS(1000));
    vTaskDelay(pdMS_TO_TICKS(60000));
    {
        const esp_partition_t* run = esp_ota_get_running_partition();
        esp_ota_img_states_t st;
        if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY && wifi_up()) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "firmware %d marked valid", NEXI_FW_VERSION);
        }
    }
    ESP_LOGI(TAG, "Nexi firmware version %d", NEXI_FW_VERSION);
    vTaskDelay(pdMS_TO_TICKS(60000));
    int fails = 0;
    while (true) {
        int wait_min = 6 * 60;
        if (NEXI_FW_VERSION > 0 && wifi_up()) {
            int ver = 0;
            std::string url;
            if (fetch_manifest(ver, url)) {
                fails = 0;
                if (ver > NEXI_FW_VERSION) {
                    // ждём, пока колонка свободна: не говорит, не слушает, будильник не звенит
                    auto& app = Application::GetInstance();
                    for (int i = 0; i < 720 && (app.GetDeviceState() != kDeviceStateIdle || alarm::ringing); i++) vTaskDelay(pdMS_TO_TICKS(5000));
                    if (app.GetDeviceState() == kDeviceStateIdle && !alarm::ringing && !install(url, ver)) wait_min = 30;
                }
            } else if (++fails < 4) wait_min = 15;   // сеть/GitHub не ответили — попробуем скоро
        }
        vTaskDelay(pdMS_TO_TICKS((uint32_t)wait_min * 60 * 1000));
    }
}

inline void Start() {
    static bool started = false;
    if (started) return;
    started = true;
    xTaskCreatePinnedToCore(task, "nexi_ota", 8192, nullptr, 3, nullptr, 0);
}

}  // namespace ota
}  // namespace nexi
