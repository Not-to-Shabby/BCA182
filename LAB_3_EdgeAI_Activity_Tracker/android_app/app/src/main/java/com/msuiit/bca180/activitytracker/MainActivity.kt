package com.msuiit.bca180.activitytracker

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.widget.Button
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import java.text.SimpleDateFormat
import java.util.*

/**
 * ==============================================================================
 * Laboratory Activity 3: Mobile Dashboard Activity
 * ==============================================================================
 */
class MainActivity : AppCompatActivity(), BleManager.BleListener {

    private lateinit var bleManager: BleManager
    private var isConnected = false

    private lateinit var tvConnectionStatus: TextView
    private lateinit var btnScan: Button
    private lateinit var tvActivityName: TextView
    private lateinit var tvConfidenceLabel: TextView
    private lateinit var pbConfidence: ProgressBar
    private lateinit var tvStepCount: TextView
    private lateinit var tvCadence: TextView
    private lateinit var tvAccelMag: TextView
    private lateinit var tvActivityLog: TextView

    private val timeFormat = SimpleDateFormat("HH:mm:ss", Locale.getDefault())
    private var lastRecordedActivity: ActivityPacket.ActivityType? = null

    private val requestPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { permissions ->
        val allGranted = permissions.all { it.value }
        if (allGranted) {
            bleManager.startScan()
        } else {
            Toast.makeText(this, "Bluetooth permissions are required to connect.", Toast.LENGTH_LONG).show()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        initViews()
        bleManager = BleManager(this, this)

        btnScan.setOnClickListener {
            if (isConnected) {
                bleManager.disconnect()
            } else {
                checkPermissionsAndScan()
            }
        }
    }

    private fun initViews() {
        tvConnectionStatus = findViewById(R.id.tvConnectionStatus)
        btnScan = findViewById(R.id.btnScan)
        tvActivityName = findViewById(R.id.tvActivityName)
        tvConfidenceLabel = findViewById(R.id.tvConfidenceLabel)
        pbConfidence = findViewById(R.id.pbConfidence)
        tvStepCount = findViewById(R.id.tvStepCount)
        tvCadence = findViewById(R.id.tvCadence)
        tvAccelMag = findViewById(R.id.tvAccelMag)
        tvActivityLog = findViewById(R.id.tvActivityLog)
    }

    private fun checkPermissionsAndScan() {
        val permissions = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            permissions.add(Manifest.permission.BLUETOOTH_SCAN)
            permissions.add(Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            permissions.add(Manifest.permission.ACCESS_FINE_LOCATION)
        }

        val missing = permissions.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }

        if (missing.isEmpty()) {
            bleManager.startScan()
        } else {
            requestPermissionLauncher.launch(missing.toTypedArray())
        }
    }

    override fun onScanningStateChanged(isScanning: Boolean) {
        runOnUiThread {
            if (isScanning) {
                btnScan.text = "Stop"
            } else if (!isConnected) {
                btnScan.text = getString(R.string.btn_scan)
            }
        }
    }

    override fun onConnectionStateChanged(status: String, isConnected: Boolean) {
        this.isConnected = isConnected
        runOnUiThread {
            tvConnectionStatus.text = status
            if (isConnected) {
                btnScan.text = getString(R.string.btn_disconnect)
                tvConnectionStatus.setTextColor(ContextCompat.getColor(this, R.color.walking_green))
            } else {
                btnScan.text = getString(R.string.btn_scan)
                tvConnectionStatus.setTextColor(ContextCompat.getColor(this, R.color.text_secondary))
            }
        }
    }

    override fun onPacketReceived(packet: ActivityPacket) {
        runOnUiThread {
            val actStr = when (packet.activityType) {
                ActivityPacket.ActivityType.WALKING -> getString(R.string.act_walking)
                ActivityPacket.ActivityType.RUNNING -> getString(R.string.act_running)
                ActivityPacket.ActivityType.STATIONARY -> getString(R.string.act_stationary)
                ActivityPacket.ActivityType.UNKNOWN -> getString(R.string.act_idle)
            }

            tvActivityName.text = actStr

            val colorRes = when (packet.activityType) {
                ActivityPacket.ActivityType.WALKING -> R.color.walking_green
                ActivityPacket.ActivityType.RUNNING -> R.color.running_orange
                ActivityPacket.ActivityType.STATIONARY -> R.color.accent_blue
                else -> R.color.text_secondary
            }
            val color = ContextCompat.getColor(this, colorRes)
            tvActivityName.setTextColor(color)
            pbConfidence.progressTintList = android.content.res.ColorStateList.valueOf(color)

            tvConfidenceLabel.text = "Confidence: ${packet.confidence}%"
            pbConfidence.progress = packet.confidence

            tvStepCount.text = String.format("%,d", packet.stepCount)
            tvCadence.text = "${packet.cadenceSpm} SPM"
            tvAccelMag.text = String.format("%.2f g", packet.accelMagG)

            // Append state transition or periodic log
            val timeStr = timeFormat.format(Date())
            val logLine = "[$timeStr] $actStr (${packet.confidence}%) | ${packet.cadenceSpm} SPM | ${packet.stepCount} steps | ${String.format("%.2f", packet.accelMagG)}g\n"
            
            if (lastRecordedActivity != packet.activityType) {
                val transitionHeader = "----------------------------------------\n>>> ACTIVITY CHANGE: $actStr\n----------------------------------------\n"
                tvActivityLog.text = transitionHeader + logLine + tvActivityLog.text
                lastRecordedActivity = packet.activityType
            } else {
                tvActivityLog.text = logLine + tvActivityLog.text
            }
        }
    }

    override fun onError(message: String) {
        runOnUiThread {
            Toast.makeText(this, message, Toast.LENGTH_SHORT).show()
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        bleManager.disconnect()
    }
}
