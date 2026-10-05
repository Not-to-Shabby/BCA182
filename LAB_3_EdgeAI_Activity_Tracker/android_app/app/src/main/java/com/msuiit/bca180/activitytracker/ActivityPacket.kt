package com.msuiit.bca180.activitytracker

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * ==============================================================================
 * Activity Telemetry Packet Parser (Method A: RW007 Provisioning BLE Exploit)
 * ==============================================================================
 * Matches binary C struct ble_activity_packet_t:
 *   [0]    Magic (0xA5)
 *   [1]    Activity (0 = Walking, 1 = Running)
 *   [2]    Confidence (0..100 %)
 *   [3]    Cadence (Steps per minute)
 *   [4..7] Step Count (32-bit uint little endian)
 *   [8..9] Accel Magnitude x100 (16-bit uint little endian)
 *   [10]   XOR Checksum (bytes 0..9)
 */
data class ActivityPacket(
    val activityType: ActivityType,
    val confidence: Int,
    val cadenceSpm: Int,
    val stepCount: Long,
    val accelMagG: Float
) {
    enum class ActivityType {
        WALKING,
        RUNNING,
        STATIONARY,
        UNKNOWN
    }

    companion object {
        const val PACKET_SIZE = 11
        const val MAGIC_BYTE: Byte = 0xA5.toByte()

        fun parse(data: ByteArray): ActivityPacket? {
            if (data.size < PACKET_SIZE) return null
            if (data[0] != MAGIC_BYTE) return null

            // Validate Checksum (XOR bytes 0..9)
            var cks: Byte = 0
            for (i in 0 until 10) {
                cks = (cks.toInt() xor data[i].toInt()).toByte()
            }
            if (cks != data[10]) {
                return null // Checksum mismatch
            }

            val act = when (data[1].toInt()) {
                0 -> ActivityType.WALKING
                1 -> ActivityType.RUNNING
                2 -> ActivityType.STATIONARY
                else -> ActivityType.UNKNOWN
            }

            val confidence = data[2].toInt() and 0xFF
            val cadence = data[3].toInt() and 0xFF

            // Parse little-endian step count (bytes 4..7)
            val buffer = ByteBuffer.wrap(data, 4, 6).order(ByteOrder.LITTLE_ENDIAN)
            val stepCount = buffer.int.toLong() and 0xFFFFFFFFL
            val magRaw = buffer.short.toInt() and 0xFFFF
            val accelMagG = magRaw.toFloat() / 100.0f

            return ActivityPacket(
                activityType = act,
                confidence = confidence,
                cadenceSpm = cadence,
                stepCount = stepCount,
                accelMagG = accelMagG
            )
        }
    }
}
