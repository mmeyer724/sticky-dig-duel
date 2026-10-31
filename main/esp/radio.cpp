#include "radio.h"

#include <cstring>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"

namespace {

constexpr char kTag[] = "radio";
FrameHandler s_on_frame = nullptr;

void on_receive(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (s_on_frame == nullptr || info == nullptr || len <= 0) {
        return;
    }
    Mac from;
    std::memcpy(from.b, info->src_addr, 6);
    const int rssi = info->rx_ctrl != nullptr ? info->rx_ctrl->rssi : -100;
    s_on_frame(from, data, static_cast<size_t>(len), rssi);
}

esp_err_t add_peer(const Mac &mac)
{
    if (esp_now_is_peer_exist(mac.b)) {
        return ESP_OK;
    }
    esp_now_peer_info_t peer = {};
    std::memcpy(peer.peer_addr, mac.b, 6);
    peer.channel = 0;  // the current channel
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_err_t err = esp_now_add_peer(&peer);
    if (err == ESP_ERR_ESPNOW_FULL) {
        // Only the opponent matters; forget everyone else.
        esp_now_peer_info_t old = {};
        bool first = true;
        while (esp_now_fetch_peer(first, &old) == ESP_OK) {
            first = false;
            if (Mac{{old.peer_addr[0], old.peer_addr[1], old.peer_addr[2], old.peer_addr[3],
                     old.peer_addr[4], old.peer_addr[5]}} != Mac::broadcast()) {
                esp_now_del_peer(old.peer_addr);
                break;
            }
        }
        err = esp_now_add_peer(&peer);
    }
    return err;
}

}  // namespace

esp_err_t EspNowRadio::init(FrameHandler on_frame)
{
    s_on_frame = on_frame;
    ESP_RETURN_ON_ERROR(esp_netif_init(), kTag, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), kTag, "event loop");
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&config), kTag, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), kTag, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), kTag, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "start");
    // Keep the receiver on so turns arrive immediately.
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), kTag, "power save");
    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(kChannel, WIFI_SECOND_CHAN_NONE), kTag, "channel");
    ESP_RETURN_ON_ERROR(esp_now_init(), kTag, "esp-now");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(on_receive), kTag, "recv cb");
    ESP_RETURN_ON_ERROR(add_peer(Mac::broadcast()), kTag, "broadcast peer");
    ESP_RETURN_ON_ERROR(esp_read_mac(self_.b, ESP_MAC_WIFI_STA), kTag, "mac");
    ESP_LOGI(kTag, "ESP-NOW up on channel %u as %02X:%02X:%02X:%02X:%02X:%02X", kChannel,
             self_.b[0], self_.b[1], self_.b[2], self_.b[3], self_.b[4], self_.b[5]);
    return ESP_OK;
}

void EspNowRadio::send(const Mac &to, const uint8_t *data, size_t len)
{
    if (add_peer(to) != ESP_OK) {
        return;
    }
    const esp_err_t err = esp_now_send(to.b, data, len);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "send failed: %s", esp_err_to_name(err));
    }
}
