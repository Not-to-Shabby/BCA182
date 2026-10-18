package com.msuiit.bca180.activitytracker

import android.os.Bundle
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.google.android.material.card.MaterialCardView
import java.text.SimpleDateFormat
import java.util.*

/**
 * ==============================================================================
 * Laboratory Activity 3: Edge AI Mobile Activity Dashboard (Wi-Fi UDP)
 * ==============================================================================
 */
class MainActivity : AppCompatActivity(), UdpReceiver.UdpListener {

    private lateinit var udpReceiver: UdpReceiver

    private lateinit var cardConnectionStatus: MaterialCardView
    private lateinit var tvConnectionStatus: TextView
    private lateinit var tvActivityName: TextView
    private lateinit var tvConfidenceLabel: TextView
    private lateinit var pbConfidence: ProgressBar
    private lateinit var tvStepCount: TextView
    private lateinit var tvCadence: TextView
    private lateinit var tvAccelMag: TextView
    private lateinit var tvActivityLog: TextView

    private val timeFormat = SimpleDateFormat("HH:mm:ss", Locale.getDefault())
    private var lastRecordedActivity: ActivityPacket.ActivityType? = null
    private var packetCount = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        initViews()
        udpReceiver = UdpReceiver(this, this)

        // Tap status badge to restart listener if stopped
        cardConnectionStatus.setOnClickListener {
            if (!udpReceiver.isListening()) {
                udpReceiver.startListening(UdpReceiver.DEFAULT_PORT)
                Toast.makeText(this, "Re-binding Wi-Fi UDP listener on :8888...", Toast.LENGTH_SHORT).show()
            }
        }

        // Auto-start listening on UDP port 8888 on launch
        udpReceiver.startListening(UdpReceiver.DEFAULT_PORT)
    }

    private fun initViews() {
        cardConnectionStatus = findViewById(R.id.cardConnectionStatus)
        tvConnectionStatus = findViewById(R.id.tvConnectionStatus)
        tvActivityName = findViewById(R.id.tvActivityName)
        tvConfidenceLabel = findViewById(R.id.tvConfidenceLabel)
        pbConfidence = findViewById(R.id.pbConfidence)
        tvStepCount = findViewById(R.id.tvStepCount)
        tvCadence = findViewById(R.id.tvCadence)
        tvAccelMag = findViewById(R.id.tvAccelMag)
        tvActivityLog = findViewById(R.id.tvActivityLog)
    }

    /* -------------------------------------------------------------------------- */
    /* UdpReceiver Listener Callbacks                                             */
    /* -------------------------------------------------------------------------- */
    override fun onListeningStateChanged(isListening: Boolean, port: Int) {
        runOnUiThread {
            if (isListening) {
                tvConnectionStatus.text = "● Listening on :$port"
                tvConnectionStatus.setTextColor(ContextCompat.getColor(this, R.color.text_secondary))
            } else {
                tvConnectionStatus.text = "○ Offline (Tap to Listen)"
                tvConnectionStatus.setTextColor(ContextCompat.getColor(this, R.color.running_orange))
            }
        }
    }

    override fun onPacketReceived(packet: ActivityPacket, senderIp: String) {
        runOnUiThread {
            packetCount++
            tvConnectionStatus.text = "● Live ($senderIp)"
            tvConnectionStatus.setTextColor(ContextCompat.getColor(this, R.color.walking_green))

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
            tvCadence.text = "${packet.cadenceSpm}"
            tvAccelMag.text = String.format("%.2fg", packet.accelMagG)

            val timeStr = timeFormat.format(Date())
            val logLine = "[$timeStr] $actStr (${packet.confidence}%) | ${packet.cadenceSpm} SPM | ${packet.stepCount} steps | ${String.format("%.2f", packet.accelMagG)}g\n"

            if (lastRecordedActivity != packet.activityType) {
                val transitionHeader = "\n----------------------------------------\n>>> ACTIVITY CHANGE: $actStr\n----------------------------------------\n"
                tvActivityLog.text = transitionHeader + logLine + tvActivityLog.text
                lastRecordedActivity = packet.activityType
            } else if (packetCount % 5 == 0) {
                // Log periodically every 5 packets
                tvActivityLog.text = logLine + tvActivityLog.text
            }
        }
    }

    override fun onRawDataReceived(message: String) {
        runOnUiThread {
            val timeStr = timeFormat.format(Date())
            tvActivityLog.text = "[$timeStr] [RAW] $message\n" + tvActivityLog.text
        }
    }

    override fun onError(message: String) {
        runOnUiThread {
            tvConnectionStatus.text = "⚠ Error on :8888"
            tvConnectionStatus.setTextColor(ContextCompat.getColor(this, R.color.running_orange))
            Toast.makeText(this, message, Toast.LENGTH_SHORT).show()
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        udpReceiver.stopListening()
    }
}
