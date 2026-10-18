package com.msuiit.bca180.activitytracker

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetSocketAddress

/**
 * ==============================================================================
 * Wi-Fi UDP Telemetry Receiver for RT-Thread Spark Board
 * ==============================================================================
 * Listens on UDP port 8888 for 11-byte binary activity packets broadcast by the
 * RW007 Wi-Fi module over the local WLAN network.
 */
class UdpReceiver(
    private val context: Context,
    private val listener: UdpListener
) {

    interface UdpListener {
        fun onListeningStateChanged(isListening: Boolean, port: Int)
        fun onPacketReceived(packet: ActivityPacket, senderIp: String)
        fun onRawDataReceived(message: String)
        fun onError(message: String)
    }

    companion object {
        private const val TAG = "UdpReceiver"
        const val DEFAULT_PORT = 8888
    }

    private var socket: DatagramSocket? = null
    @Volatile
    private var isListening = false
    private val handler = Handler(Looper.getMainLooper())
    private var receiverThread: Thread? = null
    private var multicastLock: WifiManager.MulticastLock? = null

    fun startListening(port: Int = DEFAULT_PORT) {
        if (isListening) return

        isListening = true
        listener.onListeningStateChanged(true, port)

        receiverThread = Thread {
            // Acquire MulticastLock to allow UDP broadcast reception on Android Wi-Fi
            try {
                val wifi = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
                multicastLock = wifi?.createMulticastLock("SparkActivityTrackerUdpLock")?.apply {
                    setReferenceCounted(true)
                    acquire()
                }
                Log.i(TAG, "Wi-Fi MulticastLock acquired: ${multicastLock?.isHeld}")
            } catch (e: Exception) {
                Log.w(TAG, "Could not acquire MulticastLock", e)
            }

            try {
                val s = DatagramSocket(null)
                s.reuseAddress = true
                s.broadcast = true
                s.bind(InetSocketAddress(port))
                socket = s
                Log.i(TAG, "UDP Socket successfully bound to 0.0.0.0:$port (Broadcast enabled)")

                val buffer = ByteArray(256)

                while (isListening) {
                    val datagram = DatagramPacket(buffer, buffer.size)
                    s.receive(datagram)

                    val senderIp = datagram.address.hostAddress ?: "Unknown"
                    val receivedBytes = datagram.data.copyOfRange(0, datagram.length)

                    val packet = ActivityPacket.parse(receivedBytes)
                    if (packet != null) {
                        Log.d(TAG, "Parsed ActivityPacket from $senderIp: ${packet.activityType} (${packet.confidence}%)")
                        handler.post {
                            listener.onPacketReceived(packet, senderIp)
                        }
                    } else {
                        val hex = receivedBytes.joinToString(" ") { "%02X".format(it) }
                        Log.d(TAG, "Received raw UDP from $senderIp (${receivedBytes.size} B): $hex")
                        handler.post {
                            listener.onRawDataReceived("UDP from $senderIp: $hex")
                        }
                    }
                }
            } catch (e: Exception) {
                if (isListening) {
                    Log.e(TAG, "UDP Socket error", e)
                    handler.post {
                        listener.onError("UDP Error: ${e.message}")
                    }
                }
            } finally {
                socket?.close()
                socket = null
                isListening = false
                try {
                    if (multicastLock?.isHeld == true) {
                        multicastLock?.release()
                    }
                } catch (ignored: Exception) {}
                multicastLock = null

                handler.post {
                    listener.onListeningStateChanged(false, port)
                }
            }
        }.apply {
            name = "UdpReceiverThread"
            isDaemon = true
            start()
        }
    }

    fun stopListening() {
        if (!isListening) return
        isListening = false
        try {
            socket?.close()
        } catch (ignored: Exception) {}
        socket = null
        try {
            if (multicastLock?.isHeld == true) {
                multicastLock?.release()
            }
        } catch (ignored: Exception) {}
        multicastLock = null

        receiverThread?.interrupt()
        receiverThread = null
    }

    fun isListening(): Boolean = isListening
}
