/*
 * vittil_qemu_eth.c: the emulator transport. QEMU has no Wi-Fi; it emulates an
 * OpenCores Ethernet MAC that ESP-IDF drives with CONFIG_ETH_USE_OPENETH.
 * Compiled to nothing unless CONFIG_VITTIL_QEMU_ETH=y. Production builds leave
 * it off: the application owns its network and vittil only retries.
 *
 * With `-nic user,model=open_eth`, QEMU's user-mode network gives the guest
 * 10.0.2.15 by DHCP and reaches the host at 10.0.2.2.
 */
#include "sdkconfig.h"
#if CONFIG_VITTIL_QEMU_ETH

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_eth_netif_glue.h"
#include "esp_rom_sys.h"
#include "soc/soc_caps.h"
#include "soc/interrupts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "vittil_qemu";
/* the file's state, one struct */
static struct {
    EventGroupHandle_t ev;
} vittil_qemu;
#define GOT_IP_BIT (1 << 0)

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "got IP " IPSTR " gw " IPSTR, IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw));
    xEventGroupSetBits(vittil_qemu.ev, GOT_IP_BIT);
}

esp_err_t vittil_qemu_eth_start(void)
{
    vittil_qemu.ev = xEventGroupCreate();
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.autonego_timeout_ms = 100;
    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_config);
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    err = esp_eth_driver_install(&config, &eth);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "eth driver install failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_netif_attach(netif, esp_eth_new_netif_glue(eth));
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_got_ip, NULL);
    esp_eth_start(eth);

    if (!(xEventGroupWaitBits(vittil_qemu.ev, GOT_IP_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000)) & GOT_IP_BIT)) {
        ESP_LOGW(TAG, "no IP after 30 s, uploads will retry");
    }
    return ESP_OK;
}

/* Stop the emulated MAC. QEMU does not reset the OpenCores Ethernet on the
 * software reset that follows a panic: its receive/transmit and DMA stay on
 * and its interrupt line stays asserted. The next boot then faults the first
 * time interrupts are enabled (observed: InstrFetchProhibited at PC 0 during
 * esp_timer init, boot 2 only, never without Ethernet). Clearing RXEN/TXEN
 * in MODER stops the DMA; masking and clearing INT_SOURCE drops the pending
 * IRQ. Register map: esp_eth's src/openeth/openeth.h (MODER +0x00,
 * INT_SOURCE +0x04, INT_MASK +0x08). Emulator-only, so touching the
 * peripheral without minding its clock gate is fine. */
#include "soc/soc.h"
#include "esp_private/startup_internal.h"

static void vittil_openeth_stop(void)
{
#ifdef DR_REG_EMAC_BASE
    REG_WRITE(DR_REG_EMAC_BASE + 0x00, 0xa000);     /* MODER: default, RXEN and TXEN off */
    REG_WRITE(DR_REG_EMAC_BASE + 0x08, 0);          /* INT_MASK: nothing enabled */
    REG_WRITE(DR_REG_EMAC_BASE + 0x04, 0x7f);       /* INT_SOURCE: write-1-to-clear all */
    /* and the MAC's line taken off the interrupt matrix (the number
     * ESP-IDF's intr_alloc parks a freed source on), so that whatever the
     * emulated MAC still raises after the reset reaches no CPU interrupt the
     * next boot enables before it has a handler for it. Through the
     * ROM's matrix call, every core, so esp32c3 (one core, its own matrix)
     * takes the same path as esp32's two DPORT map registers. */
    for (unsigned core = 0; core < SOC_CPU_CORES_NUM; core++) {
        esp_rom_route_intr_matrix((int)core, ETS_ETH_MAC_INTR_SOURCE, ETS_INVALID_INUM);
    }
#endif
}

/* Before the stock coredump writer, so boot 1's MAC is already quiet when we
 * reset. */
void vittil_qemu_eth_panic_quiesce(void)
{
    vittil_openeth_stop();
}

/* And again as early on boot as the init system allows, ahead of esp_timer's
 * interrupt allocation (priority 100), in case the reset re-armed the MAC. */
static esp_err_t vittil_openeth_early_reset(void)
{
    vittil_openeth_stop();
    return ESP_OK;
}
ESP_SYSTEM_INIT_FN(vittil_openeth_early_reset, SECONDARY, BIT(0), 90)
{
    return vittil_openeth_early_reset();
}

#endif /* CONFIG_VITTIL_QEMU_ETH */
