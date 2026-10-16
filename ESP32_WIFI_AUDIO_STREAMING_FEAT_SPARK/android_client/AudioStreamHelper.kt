package com.rtspark.wifiaudio

import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * AudioStreamHelper
 * Connects to the ESP32-S3 over Wi-Fi (IP: 192.168.4.1 in AP mode)
 * and streams real-time 44.1 kHz 16-bit Stereo PCM audio + Song Metadata.
 */
class AudioStreamHelper(
    private val espIpAddress: String = "192.168.4.1",
    private val udpPort: Int = 44100
) {
    private var isStreaming = false
    private var socket: DatagramSocket? = null

    /**
     * Send Track Metadata (Title, Artist, Album) to display on the RT-Spark ST7789 LCD.
     */
    suspend fun sendMetadata(title: String, artist: String, album: String = "Android Cast", format: String = "WAV 44.1k") = withContext(Dispatchers.IO) {
        try {
            val sock = socket ?: DatagramSocket().also { socket = it }
            val dest = InetAddress.getByName(espIpAddress)

            val buf = ByteBuffer.allocate(4 + 40 + 28 + 28 + 16 + 4 + 4).order(ByteOrder.LITTLE_ENDIAN)
            buf.put("META".toByteArray(Charsets.US_ASCII))
            
            fun putPadded(str: String, len: Int) {
                val b = str.toByteArray(Charsets.UTF_8)
                val copyLen = minOf(b.size, len - 1)
                buf.put(b, 0, copyLen)
                for (i in copyLen until len) buf.put(0.toByte())
            }

            putPadded(title, 40)
            putPadded(artist, 28)
            putPadded(album, 28)
            putPadded(format, 16)
            buf.putInt(0) // duration
            buf.putInt(0) // position

            val pkt = DatagramPacket(buf.array(), buf.position(), dest, udpPort)
            sock.send(pkt)
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }

    /**
     * Stream real-time PCM audio buffer (e.g. from AudioRecord or decoded MP3/WAV file).
     */
    suspend fun sendPcmChunk(pcmData: ByteArray, offset: Int, length: Int) = withContext(Dispatchers.IO) {
        try {
            val sock = socket ?: DatagramSocket().also { socket = it }
            val dest = InetAddress.getByName(espIpAddress)
            val pkt = DatagramPacket(pcmData, offset, length, dest, udpPort)
            sock.send(pkt)
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }

    fun close() {
        isStreaming = false
        socket?.close()
        socket = null
    }
}
