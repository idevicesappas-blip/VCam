package com.vcam.controller

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.os.Handler
import android.os.IBinder
import android.os.Looper

class InjectionWatchdogService : Service() {

    private val handler = Handler(Looper.getMainLooper())
    private var running = true

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForeground(1, buildNotification())

        // Verificación de salud cada 15s: si el stack de cámara murió → auto-restore
        handler.postDelayed(object : Runnable {
            override fun run() {
                if (!running) return
                if (VcamController.isInjectionActive() &&
                    !VcamController.cameraStackHealthy()) {
                    VcamController.emergencyRestore()
                    stopSelf()
                }
                handler.postDelayed(this, 15000)
            }
        }, 15000)
        return START_STICKY   // el sistema lo revive si lo mata
    }

    private fun buildNotification(): Notification {
        val ch = NotificationChannel("vcam", "Inyección VCam",
            NotificationManager.IMPORTANCE_LOW)
        getSystemService(NotificationManager::class.java).createNotificationChannel(ch)
        return Notification.Builder(this, "vcam")
            .setContentTitle("VCam activo")
            .setContentText("Inyección de cámara en ejecución")
            .setSmallIcon(android.R.drawable.ic_menu_camera)
            .build()
    }

    override fun onDestroy() { running = false; super.onDestroy() }
    override fun onBind(intent: Intent?): IBinder? = null
}
