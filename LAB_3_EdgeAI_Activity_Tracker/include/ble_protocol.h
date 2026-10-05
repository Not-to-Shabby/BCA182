/**
 * ==============================================================================
 * BLE Activity Telemetry Protocol (Method A: RW007 Provisioning GATT Exploit)
 * ==============================================================================
 * Packet structure streamed from RT-Thread Spark Board to Android Smartphone
 */

#ifndef BLE_PROTOCOL_H
#define BLE_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_PACKET_MAGIC            0xA5
#define BLE_PACKET_LENGTH           11

/* Standard WeChat / AirSync 16-bit Service & Characteristic UUIDs in RW007 */
#define BLE_UUID_AIRSYNC_SERVICE    0xFEE7
#define BLE_UUID_WRITE_CHAR         0xFEA1
#define BLE_UUID_NOTIFY_CHAR        0xFEA2
#define BLE_UUID_CCCD               0x2902

#pragma pack(push, 1)
typedef struct {
    uint8_t  magic;          /* 0xA5 Sync byte */
    uint8_t  activity;       /* 0 = Walking, 1 = Running, 2 = Stationary */
    uint8_t  confidence;     /* 0..100 (%) */
    uint8_t  cadence_spm;    /* Steps per minute (e.g. 110) */
    uint32_t step_count;     /* Accumulated step count */
    uint16_t accel_mag_x100; /* Accel magnitude in g * 100 (e.g. 1.24g -> 124) */
    uint8_t  checksum;       /* XOR checksum of bytes 0..9 */
} ble_activity_packet_t;
#pragma pack(pop)

static inline uint8_t ble_protocol_compute_checksum(const uint8_t *data, uint8_t len)
{
    uint8_t cks = 0;
    for (uint8_t i = 0; i < len; i++) {
        cks ^= data[i];
    }
    return cks;
}

static inline void ble_protocol_pack(
    ble_activity_packet_t *pkt,
    uint8_t activity,
    uint8_t confidence,
    uint8_t cadence_spm,
    uint32_t step_count,
    float accel_mag_g)
{
    pkt->magic = BLE_PACKET_MAGIC;
    pkt->activity = activity;
    pkt->confidence = confidence;
    pkt->cadence_spm = cadence_spm;
    pkt->step_count = step_count;
    
    if (accel_mag_g < 0.0f) accel_mag_g = 0.0f;
    if (accel_mag_g > 30.0f) accel_mag_g = 30.0f;
    pkt->accel_mag_x100 = (uint16_t)(accel_mag_g * 100.0f);
    
    pkt->checksum = ble_protocol_compute_checksum((const uint8_t *)pkt, BLE_PACKET_LENGTH - 1);
}

#ifdef __cplusplus
}
#endif

#endif /* BLE_PROTOCOL_H */
