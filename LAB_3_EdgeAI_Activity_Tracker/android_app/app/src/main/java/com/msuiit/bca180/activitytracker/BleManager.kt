package com.msuiit.bca180.activitytracker

import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.util.Log
import java.util.*

/**
 * ==============================================================================
 * Discovered BLE Device Metadata
 * ==============================================================================
 */
data class DiscoveredDevice(
    val device: BluetoothDevice,
    val name: String,
    val address: String,
    val rssi: Int,
    val isCandidate: Boolean,
    val details: String
)

/**
 * ==============================================================================
 * BLE GATT Client Manager for RW007 Spark Board & WeChat Telemetry
 * ==============================================================================
 */
@SuppressLint("MissingPermission")
class BleManager(
    private val context: Context,
    private val listener: BleListener
) {
    interface BleListener {
        fun onScanningStateChanged(isScanning: Boolean)
        fun onDeviceDiscovered(device: DiscoveredDevice, allDevices: List<DiscoveredDevice>)
        fun onConnectionStateChanged(status: String, isConnected: Boolean)
        fun onPacketReceived(packet: ActivityPacket)
        fun onRawDataReceived(message: String)
        fun onError(message: String)
    }

    companion object {
        private const val TAG = "BleManager"
        private const val SCAN_TIMEOUT_MS = 20000L

        // Standard WeChat / AirSync Service & Characteristic UUIDs
        val SERVICE_UUID: UUID = UUID.fromString("0000fee7-0000-1000-8000-00805f9b34fb")
        val NOTIFY_CHAR_UUID: UUID = UUID.fromString("0000fea2-0000-1000-8000-00805f9b34fb")
        val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    }

    private val bluetoothManager: BluetoothManager =
        context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val bluetoothAdapter: BluetoothAdapter? = bluetoothManager.adapter
    private var bluetoothGatt: BluetoothGatt? = null
    private var isScanning = false
    private val handler = Handler(Looper.getMainLooper())

    private val discoveredDevices = mutableListOf<DiscoveredDevice>()

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val device = result.device
            val scanRecord = result.scanRecord
            val rawName = device.name ?: scanRecord?.deviceName ?: ""
            val address = device.address

            // Evaluate if this device matches WeChat, RW007, or Spark signatures
            val hasWeChatUuid = scanRecord?.serviceUuids?.any {
                it.uuid.toString().contains("fee7", ignoreCase = true)
            } == true || scanRecord?.serviceData?.keys?.any {
                it.uuid.toString().contains("fee7", ignoreCase = true)
            } == true

            val nameMatch = rawName.contains("RW007", ignoreCase = true) ||
                    rawName.contains("Spark", ignoreCase = true) ||
                    rawName.contains("WeChat", ignoreCase = true) ||
                    rawName.contains("Air", ignoreCase = true)

            val isCandidate = hasWeChatUuid || nameMatch

            val displayName = when {
                rawName.isNotBlank() -> rawName
                isCandidate -> "Unnamed Device [WeChat / Spark Target]"
                else -> "Unnamed Device"
            }

            // Build informative details string
            val detailsList = mutableListOf<String>()
            detailsList.add("${result.rssi} dBm")
            if (hasWeChatUuid) detailsList.add("WeChat 0xFEE7")
            scanRecord?.serviceUuids?.forEach {
                val shortUuid = it.uuid.toString().substring(0, 8)
                if (!shortUuid.equals("0000fee7", ignoreCase = true)) {
                    detailsList.add("UUID: $shortUuid")
                }
            }
            val details = detailsList.joinToString(" | ")

            Log.i(TAG, "FOUND: $displayName [$address] (${result.rssi} dBm) candidate=$isCandidate")

            val discovered = DiscoveredDevice(
                device = device,
                name = displayName,
                address = address,
                rssi = result.rssi,
                isCandidate = isCandidate,
                details = details
            )

            synchronized(discoveredDevices) {
                val index = discoveredDevices.indexOfFirst { it.address == address }
                if (index >= 0) {
                    discoveredDevices[index] = discovered
                } else {
                    discoveredDevices.add(discovered)
                }
                // Sort candidates to top, then descending RSSI
                discoveredDevices.sortWith(
                    compareByDescending<DiscoveredDevice> { it.isCandidate }
                        .thenByDescending { it.rssi }
                )
            }

            handler.post {
                listener.onDeviceDiscovered(discovered, discoveredDevices.toList())
            }
        }

        override fun onScanFailed(errorCode: Int) {
            Log.e(TAG, "BLE Scan failed with error code: $errorCode")
            isScanning = false
            handler.post {
                listener.onScanningStateChanged(false)
                listener.onError("BLE Scan failed (Code $errorCode)")
            }
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                Log.i(TAG, "Connected to GATT server (${gatt.device.address}). Initiating service discovery...")
                handler.post {
                    listener.onConnectionStateChanged("Connected. Discovering services...", true)
                }
                gatt.discoverServices()
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                Log.w(TAG, "Disconnected from GATT server.")
                bluetoothGatt = null
                handler.post {
                    listener.onConnectionStateChanged("Disconnected", false)
                }
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                Log.e(TAG, "Service discovery failed with status $status")
                handler.post {
                    listener.onError("GATT Service discovery failed ($status)")
                }
                return
            }

            Log.i(TAG, "Services discovered for ${gatt.device.address}")
            val services = gatt.services
            val serviceUuids = services.map { it.uuid.toString().substring(0, 8) }
            handler.post {
                listener.onRawDataReceived("Discovered Services: ${serviceUuids.joinToString(", ")}")
            }

            // 1. Check for standard WeChat / AirSync Service (0xFEE7)
            val fee7Service = services.firstOrNull { it.uuid.toString().contains("fee7", ignoreCase = true) }
            if (fee7Service != null) {
                val fea2Char = fee7Service.characteristics.firstOrNull {
                    it.uuid.toString().contains("fea2", ignoreCase = true)
                }
                if (fea2Char != null) {
                    enableCharacteristicNotification(gatt, fea2Char)
                    handler.post {
                        listener.onConnectionStateChanged("Active: Streaming Telemetry (0xFEA2)", true)
                    }
                    return
                }
            }

            // 2. Fallback: Find ANY characteristic that supports NOTIFY or INDICATE
            for (service in services) {
                for (char in service.characteristics) {
                    val props = char.properties
                    val hasNotify = (props and BluetoothGattCharacteristic.PROPERTY_NOTIFY) != 0
                    val hasIndicate = (props and BluetoothGattCharacteristic.PROPERTY_INDICATE) != 0

                    if (hasNotify || hasIndicate) {
                        Log.i(TAG, "Subscribing to fallback notification on ${char.uuid}")
                        enableCharacteristicNotification(gatt, char)
                        handler.post {
                            listener.onConnectionStateChanged(
                                "Connected: Subscribed to ${char.uuid.toString().substring(0, 8)}",
                                true
                            )
                        }
                        return
                    }
                }
            }

            handler.post {
                listener.onConnectionStateChanged("Connected (No Notify Characteristic found)", true)
            }
        }

        @Deprecated("Deprecated in Java")
        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic
        ) {
            val rawData = characteristic.value ?: return
            val packet = ActivityPacket.parse(rawData)
            if (packet != null) {
                handler.post {
                    listener.onPacketReceived(packet)
                }
            } else {
                val hexStr = rawData.joinToString(" ") { "%02X".format(it) }
                val shortUuid = characteristic.uuid.toString().substring(0, 8)
                Log.d(TAG, "Raw payload ($shortUuid, ${rawData.size} B): $hexStr")
                handler.post {
                    listener.onRawDataReceived("[$shortUuid] $hexStr")
                }
            }
        }
    }

    fun startScan() {
        if (bluetoothAdapter == null || !bluetoothAdapter.isEnabled) {
            listener.onError("Bluetooth is disabled. Please enable Bluetooth.")
            return
        }
        if (isScanning) return

        val scanner = bluetoothAdapter.bluetoothLeScanner
        if (scanner == null) {
            listener.onError("BLE Scanner unavailable.")
            return
        }

        synchronized(discoveredDevices) {
            discoveredDevices.clear()
        }

        isScanning = true
        listener.onScanningStateChanged(true)
        listener.onConnectionStateChanged("Scanning for nearby BLE devices...", false)

        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        Log.i(TAG, "=== BLE SCAN STARTED ===")
        // Scan without hardware filters to ensure all devices (including WeChat AirSync) are visible
        scanner.startScan(null, settings, scanCallback)

        handler.postDelayed({
            if (isScanning) {
                stopScan()
                listener.onConnectionStateChanged("Scan complete (${discoveredDevices.size} devices found)", false)
            }
        }, SCAN_TIMEOUT_MS)
    }

    fun stopScan() {
        if (!isScanning) return
        Log.i(TAG, "=== BLE SCAN STOPPED (${discoveredDevices.size} devices discovered) ===")
        isScanning = false
        bluetoothAdapter?.bluetoothLeScanner?.stopScan(scanCallback)
        listener.onScanningStateChanged(false)
    }

    fun connectToDevice(device: BluetoothDevice) {
        stopScan()
        Log.i(TAG, "CONNECTING to ${device.address} (${device.name ?: "Unknown"})")
        listener.onConnectionStateChanged("Connecting to ${device.name ?: device.address}...", false)
        bluetoothGatt = device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
    }

    fun disconnect() {
        stopScan()
        bluetoothGatt?.disconnect()
        bluetoothGatt?.close()
        bluetoothGatt = null
        listener.onConnectionStateChanged("Disconnected", false)
    }

    private fun enableCharacteristicNotification(
        gatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic
    ) {
        gatt.setCharacteristicNotification(characteristic, true)
        val descriptor = characteristic.getDescriptor(CCCD_UUID)
        if (descriptor != null) {
            descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            gatt.writeDescriptor(descriptor)
            Log.i(TAG, "Subscribed to CCCD notifications on ${characteristic.uuid}")
        }
    }
}
