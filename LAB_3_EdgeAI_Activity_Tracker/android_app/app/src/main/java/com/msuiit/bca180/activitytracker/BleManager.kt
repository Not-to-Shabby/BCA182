package com.msuiit.bca180.activitytracker

import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
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
 * BLE GATT Client Manager for RW007 Spark Board Telemetry
 * ==============================================================================
 */
@SuppressLint("MissingPermission")
class BleManager(
    private val context: Context,
    private val listener: BleListener
) {
    interface BleListener {
        fun onScanningStateChanged(isScanning: Boolean)
        fun onConnectionStateChanged(status: String, isConnected: Boolean)
        fun onPacketReceived(packet: ActivityPacket)
        fun onError(message: String)
    }

    companion object {
        private const val TAG = "BleManager"
        private const val SCAN_TIMEOUT_MS = 15000L

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

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val device = result.device
            val deviceName = device.name ?: ""
            Log.d(TAG, "Discovered BLE Device: $deviceName (${device.address})")

            // Match RW007 board by name or advertised service
            if (deviceName.contains("RW007", ignoreCase = true) ||
                deviceName.contains("Spark", ignoreCase = true) ||
                result.scanRecord?.serviceUuids?.contains(ParcelUuid(SERVICE_UUID)) == true
            ) {
                Log.i(TAG, "Target Spark Board identified! Connecting to ${device.address}...")
                stopScan()
                connectToDevice(device)
            }
        }

        override fun onScanFailed(errorCode: Int) {
            Log.e(TAG, "BLE Scan failed with error code: $errorCode")
            isScanning = false
            listener.onScanningStateChanged(false)
            listener.onError("BLE Scan failed (Code $errorCode)")
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                Log.i(TAG, "Connected to GATT server. Initiating service discovery...")
                listener.onConnectionStateChanged("Connected. Discovering services...", true)
                gatt.discoverServices()
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                Log.w(TAG, "Disconnected from GATT server.")
                bluetoothGatt = null
                listener.onConnectionStateChanged("Disconnected", false)
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                val service = gatt.getService(SERVICE_UUID)
                if (service != null) {
                    val notifyChar = service.getCharacteristic(NOTIFY_CHAR_UUID)
                    if (notifyChar != null) {
                        enableCharacteristicNotification(gatt, notifyChar)
                        listener.onConnectionStateChanged("Active: Streaming Telemetry", true)
                        return
                    }
                }
                Log.w(TAG, "Service 0xFEE7 or Notify Characteristic 0xFEA2 not found.")
                listener.onError("Provisioning GATT characteristics not found on device.")
            } else {
                Log.e(TAG, "Service discovery failed with status $status")
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
                Log.w(TAG, "Received invalid or unparseable BLE packet (${rawData.size} bytes)")
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

        isScanning = true
        listener.onScanningStateChanged(true)
        listener.onConnectionStateChanged("Scanning for Spark Board...", false)

        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        val filters = listOf(
            ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE_UUID)).build()
        )

        // Timeout handler
        handler.postDelayed({
            if (isScanning) {
                stopScan()
                listener.onConnectionStateChanged("Scan timed out (Device not found)", false)
            }
        }, SCAN_TIMEOUT_MS)

        // Scan both with and without filter for broad compatibility
        scanner.startScan(null, settings, scanCallback)
    }

    fun stopScan() {
        if (!isScanning) return
        isScanning = false
        bluetoothAdapter?.bluetoothLeScanner?.stopScan(scanCallback)
        listener.onScanningStateChanged(false)
    }

    fun disconnect() {
        stopScan()
        bluetoothGatt?.disconnect()
        bluetoothGatt?.close()
        bluetoothGatt = null
        listener.onConnectionStateChanged("Disconnected", false)
    }

    private fun connectToDevice(device: BluetoothDevice) {
        listener.onConnectionStateChanged("Connecting to ${device.address}...", false)
        bluetoothGatt = device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
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
