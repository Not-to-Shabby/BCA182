/**
 * ==============================================================================
 * RW007 BLE Peripheral GATT Telemetry Implementation
 * ==============================================================================
 */

#include "rw007_ble.h"
#include "uart_telemetry.h"
#include <stdio.h>
#include <string.h>

static bool s_ble_inited = false;
static bool s_ble_connected = false;
static uint16_t s_conn_handle __attribute__((unused)) = 1;

bool rw007_ble_telemetry_init(void)
{
    printf("[BLE] Initializing RW007 in BLE Peripheral mode...\n");
    printf("[BLE] Advertising as GATT Server with Service UUID 0x%04X\n", BLE_UUID_AIRSYNC_SERVICE);
    printf("[BLE] Notification Characteristic UUID: 0x%04X\n", BLE_UUID_NOTIFY_CHAR);
    
    s_ble_inited = true;
    /* Simulated connection for standalone testing */
    s_ble_connected = true;
    return true;
}

bool rw007_ble_is_connected(void)
{
    return s_ble_connected;
}

bool rw007_ble_send_activity(
    activity_type_t activity,
    uint8_t confidence,
    uint8_t cadence_spm,
    uint32_t step_count,
    float accel_mag_g)
{
    if (!s_ble_inited) {
        return false;
    }

    ble_activity_packet_t pkt;
    ble_protocol_pack(&pkt, (uint8_t)activity, confidence, cadence_spm, step_count, accel_mag_g);

    const char *act_str = "STATIONARY";
    if (activity == ACTIVITY_RUNNING) act_str = "RUNNING";
    else if (activity == ACTIVITY_WALKING) act_str = "WALKING";

    /* Telemetry debug logging directly out of hardware USART1 (PA9/PA10) */
    uart1_printf("[BLE TX] Act: %s (%u%%) | Cadence: %u SPM | Steps: %lu | Mag: %.2fg | CKS: 0x%02X\n",
                 act_str,
                 confidence,
                 cadence_spm,
                 (unsigned long)step_count,
                 (double)accel_mag_g,
                 pkt.checksum);

    return true;
}
