// Станция Nexi: плата связывается с телефоном по Bluetooth LE.
// Телефон с Nexi рядом и подключён — разговор через него (его «мозг» и голос), нет — через xiaozhi.me по Wi-Fi.
//
// Обмен — те же сообщения, что у xiaozhi по WebSocket (JSON и пакеты Opus), только по BLE:
//   сервис 6e657869-0001-4e78-a000-000000000001
//   RX  ...0002 — телефон → плата (write without response)
//   TX  ...0003 — плата → телефон (notify)
// Каждый кусок: 1 байт заголовка (биты 0-1: 0 — JSON, 1 — Opus; бит 7 — «продолжение следует») + данные.
#pragma once

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <sys/time.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <lwip/sockets.h>

#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "audio_service.h"
#include "board.h"
#include "display.h"
#include "protocol.h"
#include "settings.h"
#include "ssid_manager.h"
#include "application.h"
#include "assets/lang_config.h"

#define NEXI_TAG "NexiStation"

namespace nexi {

inline bool g_proto_phone = false;  // сейчас выбран протокол «телефон» (по Bluetooth или Wi-Fi)
// всё, что пришло от телефона (по любому каналу): служебное «nexi» разбираем сами, остальное — разговору
inline void dispatch(int type, const uint8_t* data, size_t len);

// ------------------------------------------------------------------ BLE-периферия
namespace ble {

inline std::atomic<uint16_t> conn{BLE_HS_CONN_HANDLE_NONE};
inline std::atomic<uint16_t> mtu{23};
inline uint16_t tx_handle = 0;
inline uint8_t own_addr_type = 0;
inline bool started = false;
inline std::mutex send_mu;
// входящие куски собираем в целое сообщение
inline std::vector<uint8_t> rx_buf;
inline int rx_type = -1;
inline void (*on_message)(int type, const uint8_t* data, size_t len) = nullptr;
inline void (*on_link)(bool up) = nullptr;

// 128-битные UUID в NimBLE пишутся задом наперёд
inline const ble_uuid128_t SVC = BLE_UUID128_INIT(0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0, 0x78, 0x4e, 0x01, 0x00, 0x69, 0x78, 0x65, 0x6e);
inline const ble_uuid128_t RX = BLE_UUID128_INIT(0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0, 0x78, 0x4e, 0x01, 0x00, 0x69, 0x78, 0x65, 0x6e);
inline const ble_uuid128_t TX = BLE_UUID128_INIT(0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0, 0x78, 0x4e, 0x01, 0x00, 0x69, 0x78, 0x65, 0x6e);

inline bool connected() { return conn.load() != BLE_HS_CONN_HANDLE_NONE; }

// надпись на экране колонки — чтобы было видно, что с Bluetooth
inline void notify(const char* text, int ms = 4000) {
    auto* d = Board::GetInstance().GetDisplay();
    if (d) d->ShowNotification(text, ms);
}

inline int chr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt, void* arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 1) return 0;
    std::vector<uint8_t> chunk(len);
    ble_hs_mbuf_to_flat(ctxt->om, chunk.data(), len, nullptr);
    uint8_t hdr = chunk[0];
    int type = hdr & 0x03;
    if (rx_type != type) { rx_buf.clear(); rx_type = type; }
    rx_buf.insert(rx_buf.end(), chunk.begin() + 1, chunk.end());
    if (!(hdr & 0x80)) {
        nexi::dispatch(type, rx_buf.data(), rx_buf.size());
        rx_buf.clear();
        rx_type = -1;
    }
    return 0;
}

inline const struct ble_gatt_chr_def chrs[] = {
    {.uuid = &RX.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
    {.uuid = &TX.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &tx_handle},
    {0},
};
inline const struct ble_gatt_svc_def svcs[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &SVC.u, .characteristics = chrs},
    {0},
};

inline void advertise();

inline int gap_event(struct ble_gap_event* e, void* arg) {
    switch (e->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (e->connect.status == 0) {
                conn = e->connect.conn_handle;
                ESP_LOGI(NEXI_TAG, "телефон подключился по Bluetooth");
                notify("📱 Nexi на телефоне подключён");
                // быстрее обмен: интервал 15–30 мс
                struct ble_gap_upd_params p = {.itvl_min = 12, .itvl_max = 24, .latency = 0, .supervision_timeout = 400,
                                               .min_ce_len = 0, .max_ce_len = 0};
                ble_gap_update_params(e->connect.conn_handle, &p);
                if (on_link) on_link(true);
            } else {
                advertise();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(NEXI_TAG, "телефон отключился (причина %d)", e->disconnect.reason);
            notify("Телефон отключился");
            conn = BLE_HS_CONN_HANDLE_NONE;
            mtu = 23;
            rx_buf.clear();
            rx_type = -1;
            if (on_link) on_link(false);
            advertise();
            break;
        case BLE_GAP_EVENT_MTU:
            mtu = e->mtu.value;
            ESP_LOGI(NEXI_TAG, "MTU %d", e->mtu.value);
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            advertise();
            break;
        default:
            break;
    }
    return 0;
}

inline void advertise() {
    struct ble_hs_adv_fields f = {};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = (ble_uuid128_t*)&SVC;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    ble_gap_adv_set_fields(&f);
    struct ble_hs_adv_fields rsp = {};
    const char* name = "Nexi Station";
    rsp.name = (uint8_t*)name;
    rsp.name_len = strlen(name);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);
    struct ble_gap_adv_params p = {};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = 160;  // 100 мс — телефон быстро находит, батарея не страдает
    p.itvl_max = 240;
    ble_gap_adv_start(own_addr_type, nullptr, BLE_HS_FOREVER, &p, gap_event, nullptr);
}

inline void on_sync() {
    ble_hs_id_infer_auto(0, &own_addr_type);
    advertise();
    notify("Bluetooth: жду Nexi на телефоне", 5000);
}

inline void host_task(void*) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

inline void Start() {
    if (started) return;
    started = true;
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(NEXI_TAG, "nimble init failed: %d", err);
        static char msg[48];
        snprintf(msg, sizeof(msg), "Bluetooth не запустился (%d)", (int)err);
        notify(msg, 10000);
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(svcs);
    ble_gatts_add_svcs(svcs);
    ble_svc_gap_device_name_set("Nexi Station");
    ble_att_set_preferred_mtu(512);
    nimble_port_freertos_init(host_task);
    ESP_LOGI(NEXI_TAG, "Bluetooth: жду телефон");
}

// Отправить сообщение телефону кусками под MTU
inline bool Send(int type, const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lk(send_mu);
    uint16_t c = conn.load();
    if (c == BLE_HS_CONN_HANDLE_NONE || tx_handle == 0) return false;
    size_t max = (size_t)std::max<int>(20, mtu.load() - 3) - 1;
    size_t pos = 0;
    do {
        size_t n = std::min(max, len - pos);
        uint8_t hdr = (uint8_t)(type & 0x03) | (pos + n < len ? 0x80 : 0);
        for (int attempt = 0;; attempt++) {
            struct os_mbuf* om = ble_hs_mbuf_from_flat(&hdr, 1);
            if (om != nullptr && n > 0) os_mbuf_append(om, data + pos, n);
            int rc = om ? ble_gatts_notify_custom(c, tx_handle, om) : BLE_HS_ENOMEM;
            if (rc == 0) break;
            if (attempt > 40 || conn.load() == BLE_HS_CONN_HANDLE_NONE) return false;
            vTaskDelay(pdMS_TO_TICKS(5));  // буферы заняты — чуть подождать
        }
        pos += n;
    } while (pos < len);
    return true;
}

}  // namespace ble

// ------------------------------------------------------------------ Wi-Fi: телефон в той же сети
// Колонка раз в 2 с кричит в сеть UDP-«маяком» «NEXI1 47802» на порт 47801; телефон с Nexi слышит и подключается
// по TCP к 47802. Кадр по TCP: 1 байт типа (0 — JSON, 1 — Opus) + 4 байта длины (big-endian) + данные.
namespace tcp {
inline constexpr int UDP_PORT = 47801, TCP_PORT = 47802;
inline std::atomic<int> sock{-1};
inline std::mutex send_mu;
inline bool started = false;

inline bool connected() { return sock.load() >= 0; }

inline bool Send(int type, const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lk(send_mu);
    int s = sock.load();
    if (s < 0) return false;
    uint8_t hdr[5] = {(uint8_t)type, (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
    auto all = [&](const uint8_t* p, size_t n) {
        while (n > 0) {
            int w = send(s, p, n, 0);
            if (w <= 0) return false;
            p += w; n -= w;
        }
        return true;
    };
    if (all(hdr, 5) && all(data, len)) return true;
    shutdown(s, SHUT_RDWR);   // читатель увидит обрыв и всё уберёт
    return false;
}

inline bool read_all(int s, uint8_t* p, size_t n) {
    while (n > 0) {
        int r = recv(s, p, n, 0);
        if (r <= 0) return false;
        p += r; n -= r;
    }
    return true;
}

/** IP колонки и широковещательный адрес её сети; false — Wi-Fi нет. */
inline bool net_addr(uint32_t& ip, uint32_t& bcast) {
    esp_netif_t* n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (!n || esp_netif_get_ip_info(n, &info) != ESP_OK || info.ip.addr == 0) return false;
    ip = info.ip.addr;
    bcast = info.ip.addr | ~info.netmask.addr;
    return true;
}

inline void serve(int c) {
    int one = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    sock = c;
    ESP_LOGI(NEXI_TAG, "телефон подключился по Wi-Fi");
    ble::notify("📶 Nexi на телефоне — по Wi-Fi");
    std::vector<uint8_t> buf;
    for (;;) {
        uint8_t h[5];
        if (!read_all(c, h, 5)) break;
        uint32_t n = ((uint32_t)h[1] << 24) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 8) | h[4];
        if (n > 256 * 1024) break;
        buf.resize(n);
        if (n && !read_all(c, buf.data(), n)) break;
        if (h[0] == 2) continue;   // «пинг» от телефона
        nexi::dispatch(h[0] & 0x03, buf.data(), n);
    }
    {
        std::lock_guard<std::mutex> lk(send_mu);
        sock = -1;
        close(c);
    }
    ESP_LOGI(NEXI_TAG, "телефон отключился от Wi-Fi");
    if (ble::on_link) ble::on_link(false);
}

inline void task(void*) {
    int srv = -1, udp = -1;
    int64_t last_beacon = 0;
    for (;;) {
        uint32_t ip = 0, bcast = 0;
        if (!net_addr(ip, bcast)) {
            if (srv >= 0) { close(srv); srv = -1; }
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (srv < 0) {
            srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            int one = 1;
            setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            sockaddr_in a = {};
            a.sin_family = AF_INET;
            a.sin_port = htons(TCP_PORT);
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            if (bind(srv, (sockaddr*)&a, sizeof(a)) != 0 || listen(srv, 1) != 0) { close(srv); srv = -1; vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
        }
        if (udp < 0) {
            udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            int one = 1;
            setsockopt(udp, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
        }
        int64_t now = esp_timer_get_time() / 1000;
        if (now - last_beacon > 2000) {
            last_beacon = now;
            sockaddr_in b = {};
            b.sin_family = AF_INET;
            b.sin_port = htons(UDP_PORT);
            b.sin_addr.s_addr = bcast;
            const char* msg = "NEXI1 47802 Nexi Station";
            sendto(udp, msg, strlen(msg), 0, (sockaddr*)&b, sizeof(b));
        }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(srv, &rd);
        timeval tv = {.tv_sec = 1, .tv_usec = 0};
        if (select(srv + 1, &rd, nullptr, nullptr, &tv) > 0 && FD_ISSET(srv, &rd)) {
            int c = accept(srv, nullptr, nullptr);
            if (c >= 0) serve(c);   // один телефон за раз; маяк молчит, пока он на связи
        }
    }
}

inline void Start() {
    if (started) return;
    started = true;
    xTaskCreate(task, "nexi_wifi", 6144, nullptr, 4, nullptr);
}
}  // namespace tcp

inline bool link_up() { return tcp::connected() || ble::connected(); }
inline bool link_send(int type, const uint8_t* d, size_t n) {
    if (tcp::connected()) return tcp::Send(type, d, n);
    return ble::Send(type, d, n);
}

// ------------------------------------------------------------------ будильник на самой колонке
// Время колонке сообщает телефон при каждом подключении. Будильник хранится в памяти колонки,
// поэтому зазвенит, даже если телефон к утру отключился. Выключить — только нажатием на звезду Nexi на экране.
namespace alarm {
inline std::atomic<int64_t> at{0};          // когда звенеть, секунды Unix; 0 — нет
inline std::atomic<int> tz_min{420};        // часовой пояс телефона, минуты
inline std::atomic<bool> ringing{false};
inline std::atomic<int64_t> ring_started{0};
inline bool started = false;
inline int saved_volume = -1;
// экран (рисует nexi_ui.h): 0 — убрать всё, 1 — показать «будильник на …», 2 — звенит
inline void (*on_ui)(int state, int hh, int mm) = nullptr;

inline bool time_ok() { return time(nullptr) > 1700000000; }

inline void show() {
    int64_t a = at.load();
    if (!on_ui) return;
    if (ringing) { on_ui(2, 0, 0); return; }
    if (a <= 0) { on_ui(0, 0, 0); return; }
    int64_t loc = a + (int64_t)tz_min.load() * 60;
    on_ui(1, (int)((loc / 3600) % 24), (int)((loc / 60) % 60));
}

inline void Set(int64_t when) {
    at = when;
    Settings s("nexi", true);
    s.SetInt("alarm", (int32_t)when);
    show();
}

/** Выключить звонок. true — звенел (значит, нажатие ушло на выключение). */
inline bool Stop() {
    if (!ringing.exchange(false)) return false;
    Set(0);
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec && saved_volume >= 0) codec->SetOutputVolume(saved_volume);
    saved_volume = -1;
    show();
    // телефону — что проснулись (чтобы он тоже забыл этот будильник)
    const char* m = "{\"type\":\"nexi\",\"alarm_done\":1}";
    link_send(0, (const uint8_t*)m, strlen(m));
    return true;
}

// часы в верхней строке (xiaozhi) берут localtime() — без TZ показывают UTC. POSIX пишет смещение наоборот: UTC+7 → «-07:00».
inline void apply_tz(int minutes) {
    char tz[24];
    int a = minutes < 0 ? -minutes : minutes;
    snprintf(tz, sizeof(tz), "NX%c%02d:%02d", minutes >= 0 ? '-' : '+', a / 60, a % 60);
    setenv("TZ", tz, 1);
    tzset();
}

inline void task(void*) {
    {
        Settings s("nexi");
        at = s.GetInt("alarm", 0);
        tz_min = s.GetInt("tz", 420);
    }
    apply_tz(tz_min);
    vTaskDelay(pdMS_TO_TICKS(3000));
    show();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(ringing ? 1500 : 1000));
        int64_t a = at.load();
        if (!time_ok() || a <= 0) continue;
        int64_t now = time(nullptr);
        if (!ringing && now >= a) {
            if (now - a > 30 * 60) { Set(0); continue; }   // проспали больше получаса (колонка была выключена) — не орём
            ringing = true;
            ring_started = now;
            auto* codec = Board::GetInstance().GetAudioCodec();
            if (codec) { saved_volume = codec->output_volume(); if (saved_volume < 70) codec->SetOutputVolume(70); }
            show();
        }
        if (ringing) {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) app.GetAudioService().PlaySound(Lang::Sounds::OGG_VIBRATION);
        }
    }
}

inline void Start() {
    if (started) return;
    started = true;
    xTaskCreate(task, "nexi_alarm", 4096, nullptr, 3, nullptr);
}
}  // namespace alarm

// ------------------------------------------------------------------ служебное от телефона: время, будильник, Wi-Fi
// экран колонки (nexi_ui.h) подставляет сюда открытие игры: телефон услышал «давай сыграем»
inline void (*on_game)(const char* name) = nullptr;

inline void handle_nexi(cJSON* root) {
    cJSON* g = cJSON_GetObjectItem(root, "game");
    if (cJSON_IsString(g) && on_game) on_game(g->valuestring);
    cJSON* t = cJSON_GetObjectItem(root, "time");
    if (cJSON_IsNumber(t) && t->valuedouble > 1700000000) {
        timeval tv = {.tv_sec = (time_t)t->valuedouble, .tv_usec = 0};
        settimeofday(&tv, nullptr);
    }
    cJSON* tz = cJSON_GetObjectItem(root, "tz");
    if (cJSON_IsNumber(tz) && tz->valueint != alarm::tz_min.load()) {
        alarm::tz_min = tz->valueint;
        Settings s("nexi", true);
        s.SetInt("tz", tz->valueint);
    }
    if (cJSON_IsNumber(tz)) alarm::apply_tz(tz->valueint);
    cJSON* a = cJSON_GetObjectItem(root, "alarm");
    if (cJSON_IsNumber(a)) {
        if (a->valuedouble <= 0) { if (!alarm::Stop()) alarm::Set(0); }
        else if (!alarm::ringing) alarm::Set((int64_t)a->valuedouble);
    }
    cJSON* stop = cJSON_GetObjectItem(root, "alarm_stop");
    if (cJSON_IsNumber(stop)) alarm::Stop();
    // сети Wi-Fi: добавляем новые и перезагружаемся, чтобы подключиться
    cJSON* w = cJSON_GetObjectItem(root, "wifi");
    if (cJSON_IsArray(w)) {
        auto& sm = SsidManager::GetInstance();
        bool added = false;
        cJSON* it;
        cJSON_ArrayForEach(it, w) {
            cJSON* ss = cJSON_GetObjectItem(it, "ssid");
            cJSON* pw = cJSON_GetObjectItem(it, "pass");
            if (!cJSON_IsString(ss) || !cJSON_IsString(pw) || !ss->valuestring[0]) continue;
            bool same = false;
            for (auto& x : sm.GetSsidList()) if (x.ssid == ss->valuestring && x.password == pw->valuestring) same = true;
            if (!same) { sm.AddSsid(ss->valuestring, pw->valuestring); added = true; }
        }
        uint32_t ip, bc;
        if (added && !tcp::net_addr(ip, bc)) {
            ble::notify("Сохранил сети Wi-Fi, перезагружаюсь…", 3000);
            esp_timer_handle_t h;
            esp_timer_create_args_t args = {};
            args.callback = [](void*) { esp_restart(); };
            args.name = "nexi_reboot";
            if (esp_timer_create(&args, &h) == ESP_OK) esp_timer_start_once(h, 3000000);
        } else if (added) {
            ble::notify("Сети Wi-Fi сохранены", 3000);
        }
    }
}

inline void dispatch(int type, const uint8_t* data, size_t len) {
    if (type == 0 && len > 0 && len < 8192) {
        // «nexi»-сообщения — не разговор: обрабатываем всегда, даже без открытого разговора
        if (len > 14 && data[0] == '{') {
            cJSON* root = cJSON_ParseWithLength((const char*)data, len);
            if (root) {
                cJSON* ty = cJSON_GetObjectItem(root, "type");
                if (cJSON_IsString(ty) && strcmp(ty->valuestring, "nexi") == 0) {
                    handle_nexi(root);
                    cJSON_Delete(root);
                    return;
                }
                cJSON_Delete(root);
            }
        }
    }
    if (ble::on_message) ble::on_message(type, data, len);
}

// ------------------------------------------------------------------ протокол xiaozhi поверх BLE
#define NEXI_HELLO_EVENT (1 << 0)

class NexiBleProtocol : public Protocol {
public:
    NexiBleProtocol() {
        events_ = xEventGroupCreate();
        self_ = this;
        ble::on_message = [](int type, const uint8_t* d, size_t n) { if (self_) self_->OnMessage(type, d, n); };
        ble::on_link = [](bool up) { if (self_ && !up && !link_up()) self_->OnLinkDown(); };
    }
    ~NexiBleProtocol() override {
        if (self_ == this) { self_ = nullptr; ble::on_message = nullptr; ble::on_link = nullptr; }
        vEventGroupDelete(events_);
    }

    bool Start() override { return true; }

    bool OpenAudioChannel() override {
        error_occurred_ = false;
        if (!link_up()) { SetError("Телефон не подключён"); return false; }
        xEventGroupClearBits(events_, NEXI_HELLO_EVENT);
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "hello");
        cJSON_AddNumberToObject(root, "version", 1);
        AddClientCapabilities(root, false);
        cJSON_AddStringToObject(root, "transport", "websocket");
        cJSON* ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "format", "opus");
        cJSON_AddNumberToObject(ap, "sample_rate", 16000);
        cJSON_AddNumberToObject(ap, "channels", 1);
        cJSON_AddNumberToObject(ap, "frame_duration", OPUS_FRAME_DURATION_MS);
        cJSON_AddItemToObject(root, "audio_params", ap);
        char* s = cJSON_PrintUnformatted(root);
        std::string hello(s);
        cJSON_free(s);
        cJSON_Delete(root);
        if (!SendText(hello)) { SetError("Не получилось связаться с телефоном"); return false; }
        EventBits_t bits = xEventGroupWaitBits(events_, NEXI_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(8000));
        if (!(bits & NEXI_HELLO_EVENT)) { SetError("Телефон не ответил"); return false; }
        opened_ = true;
        last_incoming_time_ = std::chrono::steady_clock::now();
        if (on_audio_channel_opened_) on_audio_channel_opened_();
        return true;
    }

    void CloseAudioChannel(bool send_goodbye = true) override {
        if (send_goodbye && opened_) SendText("{\"type\":\"goodbye\"}");
        bool was = opened_;
        opened_ = false;
        if (was && on_audio_channel_closed_) on_audio_channel_closed_();
    }

    bool IsAudioChannelOpened() const override { return opened_ && link_up() && !error_occurred_ && !IsTimeout(); }

    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override {
        if (!opened_) return false;
        return link_send(1, packet->payload.data(), packet->payload.size());
    }

protected:
    bool SendText(const std::string& text) override { return link_send(0, (const uint8_t*)text.data(), text.size()); }

private:
    static inline NexiBleProtocol* self_ = nullptr;
    EventGroupHandle_t events_;
    std::atomic<bool> opened_{false};

    void OnLinkDown() {
        if (opened_) { opened_ = false; if (on_audio_channel_closed_) on_audio_channel_closed_(); }
    }

    void OnMessage(int type, const uint8_t* data, size_t len) {
        last_incoming_time_ = std::chrono::steady_clock::now();
        if (type == 1) {
            if (opened_ && on_incoming_audio_) {
                on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                    .sample_rate = server_sample_rate_,
                    .frame_duration = server_frame_duration_,
                    .timestamp = 0,
                    .payload = std::vector<uint8_t>(data, data + len)}));
            }
            return;
        }
        cJSON* root = cJSON_ParseWithLength((const char*)data, len);
        if (!root) return;
        cJSON* t = cJSON_GetObjectItem(root, "type");
        if (cJSON_IsString(t)) {
            if (strcmp(t->valuestring, "hello") == 0) {
                cJSON* sid = cJSON_GetObjectItem(root, "session_id");
                if (cJSON_IsString(sid)) session_id_ = sid->valuestring;
                cJSON* ap = cJSON_GetObjectItem(root, "audio_params");
                if (cJSON_IsObject(ap)) {
                    cJSON* sr = cJSON_GetObjectItem(ap, "sample_rate");
                    cJSON* fd = cJSON_GetObjectItem(ap, "frame_duration");
                    if (cJSON_IsNumber(sr)) server_sample_rate_ = sr->valueint;
                    if (cJSON_IsNumber(fd)) server_frame_duration_ = fd->valueint;
                }
                xEventGroupSetBits(events_, NEXI_HELLO_EVENT);
            } else if (strcmp(t->valuestring, "goodbye") == 0) {
                // телефон закончил разговор (помолчали) — плата возвращается к ожиданию
                if (opened_) { opened_ = false; if (on_audio_channel_closed_) on_audio_channel_closed_(); }
            } else if (on_incoming_json_) {
                on_incoming_json_(root);
            }
        }
        cJSON_Delete(root);
    }
};

}  // namespace nexi
