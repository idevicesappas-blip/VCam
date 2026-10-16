package com.vcam.controller

import android.util.Log
import com.topjohnwu.superuser.Shell
import org.json.JSONObject

object VcamController {
    init { Shell.enableVerboseLogging = false; Shell.getShell() }

    private val DIR = "/data/adb/vcam"
    val CONFIG = "$DIR/config.json"
    val ACTIVE = "$DIR/ACTIVE"

    fun ensureDirs(): Boolean = Shell.cmd(
        "mkdir -p $DIR",
        "touch $CONFIG",
        "[ -f $CONFIG ] && echo ok"
    ).exec().out.lastOrNull() == "ok" ||
        // sin root verificado → reintenta
        Shell.cmd("mkdir -p $DIR && touch $CONFIG").exec().isSuccess

    fun writeConfig(cfg: JSONObject): Boolean {
        return Shell.cmd("cat > $CONFIG << 'VCAMEOF'\n${cfg.toString()}\nVCAMEOF").exec().isSuccess
    }

    fun startInjection(): Boolean {
        writeConfigField("active", true)
        // Reinicio del HAL provider para que aplique — Magisk/KSU con HUP
        return Shell.cmd(
            "kill -HUP \$(pidof android.hardware.camera.provider* 2>/dev/null | tr ' ' '\\n' | head -1) 2>/dev/null",
            "echo started"
        ).exec().isSuccess
    }

    fun stopInjection() {
        writeConfigField("active", false)
    }

    fun writeConfigField(key: String, value: Any) {
        val cur = readConfigRaw()
        cur.put(key, value)
        writeConfig(cur)
    }

    fun readConfigRaw(): JSONObject {
        val r = Shell.cmd("cat $CONFIG 2>/dev/null").exec()
        val s = r.out.joinToString("").trim()
        return if (s.isEmpty() || !s.startsWith("{")) JSONObject() else JSONObject(s)
    }

    fun isInjectionActive(): Boolean =
        readConfigRaw().optBoolean("active", false)

    fun restartHal() = Shell.cmd(
        "kill -HUP \$(pidof android.hardware.camera.provider* 2>/dev/null | tr ' ' '\\n' | head -1)"
    ).exec().isSuccess

    fun setReplaceFront(on: Boolean): Boolean {
        writeConfigField("replace", if (on) "front" else "none")
        // El service.sh congela/descongela los providers vendor; para aplicar
        // YA (sin esperar el watchdog de 30s) mandamos señal al loop del módulo
        return Shell.cmd(
            "touch /data/adb/vcam/APPLY_REPLACE",
            "kill -HUP \$(pidof vcam_provider 2>/dev/null) 2>/dev/null",
            "echo ok"
        ).exec().isSuccess
    }

    /** RESTAURACIÓN DE EMERGENCIA: revierte TODO a cámara física y detiene el daemon.
     *  Se llama desde: botón "Restaurar" + auto-trigger si algo va mal. */
    fun emergencyRestore(): Boolean {
        val r = Shell.cmd(
            // 1. quitar replace (descongela providers vendor)
            "sed -i 's/\"replace\":\"front\"/\"replace\":\"none\"/' $CONFIG",
            // 2. desactivar inyección
            "sed -i 's/\"active\":true/\"active\":false/' $CONFIG",
            "rm -f $CONFIG.bak",
            // 3. descongelar TODO proceso de cámara (kill -CONT) por si quedó STOP
            "for P in \$(ps -A -o PID,NAME | grep -i camera.provider | awk '{print \$1}'); do kill -CONT \$P; done",
            // 4. matar el daemon vcam
            "kill -9 \$(pidof vcam_provider) 2>/dev/null",
            // 5. reiniciar cameraserver (re-enumera cámaras limpio)
            "kill -9 \$(pidof cameraserver) 2>/dev/null",
            // 6. verificación final
            "[ -n \"\$(ps -A -o NAME | grep cameraserver)\" ] && echo RESTORED"
        ).exec()
        Log.d("VcamController", "restore: ${r.out}")
        return r.out.any { it.toString().contains("RESTORED") } || r.isSuccess
    }

    /** Diagnóstico en vivo para el toggle de seguridad: ¿funcionan las cámaras? */
    fun cameraStackHealthy(): Boolean {
        val r = Shell.cmd(
            "ps -A -o NAME | grep -c cameraserver",
            "dumpsys media.camera 2>/dev/null | grep -c 'Device [0-9]* is open' || true",
            "pidof vcam_provider || echo none"
        ).exec()
        val cameraserverAlive = r.out.getOrNull(0)?.trim()?.toIntOrNull()?.let { it > 0 } ?: false
        val providerRunning = r.out.lastOrNull()?.trim() != "none"
        // saludable = cameraserver vivo Y (daemon corre o inyección desactivada)
        val cfg = readConfigRaw()
        val healthy = cameraserverAlive && (!cfg.optBoolean("active", false) || providerRunning)
        return healthy
    }
}
