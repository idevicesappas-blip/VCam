package com.vcam.controller

import android.annotation.SuppressLint
import android.app.Activity
import android.content.Intent
import android.graphics.BitmapFactory
import android.graphics.ColorMatrix
import android.graphics.ColorMatrixColorFilter
import android.graphics.Matrix
import android.graphics.RenderEffect
import android.graphics.SurfaceTexture
import android.media.MediaPlayer
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.view.Surface
import android.view.TextureView
import android.view.View
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.button.MaterialButton
import org.json.JSONObject
import java.io.File

class MainActivity : AppCompatActivity() {

    // ---- estado en vivo (se aplica en preview Y en el JSON) ----
    private var cfg = JSONObject()
    private var player: MediaPlayer? = null
    private lateinit var preview: TextureView
    private val handler = Handler(Looper.getMainLooper())
    private var toast: Toast? = null

    // UI
    private lateinit var modeVideo: MaterialButton
    private lateinit var modeImage: MaterialButton
    private lateinit var modeStream: MaterialButton
    private lateinit var selFile: MaterialButton
    private lateinit var urlInput: EditText
    private lateinit var switchMain: Switch
    private lateinit var switchAi: Switch
    private lateinit var switchNoise: Switch
    private lateinit var switchDust: Switch
    private lateinit var switchFloating: Switch
    private lateinit var switchReplaceFront: Switch
    private lateinit var rotSlider: SeekBar
    private lateinit var zoomSlider: SeekBar
    private lateinit var brightSlider: SeekBar
    private lateinit var contrastSlider: SeekBar
    private lateinit var noiseSlider: SeekBar
    private lateinit var dustSlider: SeekBar
    private lateinit var btnUp: MaterialButton
    private lateinit var btnDown: MaterialButton
    private lateinit var btnLeft: MaterialButton
    private lateinit var btnRight: MaterialButton
    private lateinit var btnRestore: MaterialButton
    private lateinit var previewState: TextView
    private lateinit var sourceInfo: TextView
    private lateinit var safetyStatus: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        // Root check + carpetas
        if (!VcamController.ensureDirs())
            toast("⚠️ Root no disponible (Magisk/KernelSU requerido)")

        bindViews()
        loadConfig()
        wireControls()
        requestRuntimePermissions()
        
        handleShortcut(intent)

        if (intent?.action == Intent.ACTION_SEND) {
            val uri = if (Build.VERSION.SDK_INT >= 33)
                intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java)
            else
                intent.getParcelableExtra<Uri>(Intent.EXTRA_STREAM)
            uri?.let {
                copyToVcamCache(it)?.let { path ->
                    sourceInfo.text = "Fuente: ${path.substringAfterLast('/')}"
                    toast("✓ Fuente compartida guardada — pulsa START")
                }
            }
        }
    }

    private fun bindViews() {
        preview = findViewById(R.id.preview)
        modeVideo  = findViewById(R.id.modeVideo)
        modeImage   = findViewById(R.id.modeImage)
        modeStream  = findViewById(R.id.modeStream)
        selFile     = findViewById(R.id.selFile)
        urlInput    = findViewById(R.id.urlInput)
        switchMain  = findViewById(R.id.switchMain)
        switchAi    = findViewById(R.id.switchAi)
        switchNoise = findViewById(R.id.switchNoise)
        switchDust  = findViewById(R.id.switchDust)
        switchFloating = findViewById(R.id.switchFloating)
        switchReplaceFront = findViewById(R.id.switchReplaceFront)
        rotSlider = findViewById(R.id.rotSlider)
        zoomSlider = findViewById(R.id.zoomSlider)
        brightSlider = findViewById(R.id.brightSlider)
        contrastSlider = findViewById(R.id.contrastSlider)
        noiseSlider = findViewById(R.id.noiseSlider)
        dustSlider = findViewById(R.id.dustSlider)
        btnUp = findViewById(R.id.btnUp); btnDown = findViewById(R.id.btnDown)
        btnLeft = findViewById(R.id.btnLeft); btnRight = findViewById(R.id.btnRight)
        btnRestore = findViewById(R.id.btnRestore)
        previewState = findViewById(R.id.previewState)
        sourceInfo = findViewById(R.id.sourceInfo)
        safetyStatus = findViewById(R.id.safetyStatus)
    }

    private fun loadConfig() {
        cfg = VcamController.readConfigRaw()
        urlInput.setText(cfg.optString("source", "").takeIf { it.startsWith("rt") } ?: "")
        switchAi.isChecked = cfg.optBoolean("aiFaceBypass", true)
        val mode = cfg.optString("mode", "video")
        applyMode(if (mode == "stream") "stream" else if (mode == "image") "image" else "video")
        switchReplaceFront.isChecked = cfg.optString("replace", "none") == "front"
        val fn = cfg.optString("source", "").substringAfterLast('/')
        if (fn.isNotEmpty()) sourceInfo.text = "Fuente: $fn"
        updateSafetyStatus()
    }

    private fun applyMode(mode: String) {
        val isVideo = mode == "video"; val isImage = mode == "image"; val isStream = mode == "stream"
        modeVideo.isEnabled = !isVideo; modeImage.isEnabled = !isImage; modeStream.isEnabled = !isStream
        selFile.visibility  = if (isStream) TextView.GONE else TextView.VISIBLE
        urlInput.visibility = if (isStream) EditText.VISIBLE else EditText.GONE
        cfg.put("mode", mode)
        stopPreview()
    }

    private fun wireControls() {
        modeVideo.setOnClickListener { applyMode("video"); persist() }
        modeImage.setOnClickListener { applyMode("image"); persist(); if (cfg.optString("source").isNotEmpty()) startPreview() }
        modeStream.setOnClickListener { applyMode("stream"); persist() }

        selFile.setOnClickListener {
            val intent = Intent(Intent.ACTION_GET_CONTENT).apply {
                val isImage = cfg.optString("mode") == "image"
                type = if (isImage) "image/*" else "video/*"
                addCategory(Intent.CATEGORY_OPENABLE)
            }
            startActivityForResult(Intent.createChooser(intent, "Seleccionar"), 100)
        }

        switchMain.setOnCheckedChangeListener { _, on ->
            cfg.put("active", on); persist()
            if (on) {
                startForegroundService(Intent(this, InjectionWatchdogService::class.java))
                startPreview()
            } else {
                stopService(Intent(this, InjectionWatchdogService::class.java))
                stopPreview()
            }
            previewState.text = if (on) "🟢 INYECCIÓN ACTIVA" else "🔴 Inyección detenida"
            updateSafetyStatus()
        }
        
        switchAi.setOnCheckedChangeListener { _, on -> cfg.put("aiFaceBypass", on); persist() }
        
        switchFloating.setOnCheckedChangeListener { _, on ->
            if (on) startService(Intent(this, FloatingControls::class.java))
            else stopService(Intent(this, FloatingControls::class.java))
        }
        
        switchNoise.setOnCheckedChangeListener { _, on ->
            if (!on) { noiseSlider.progress = 0; cfg.put("noiseLevel", 0.0); }
            noiseSlider.isEnabled = on
            persist()
        }
        
        switchDust.setOnCheckedChangeListener { _, on ->
            if (!on) { dustSlider.progress = 0; cfg.put("dustLevel", 0.0); }
            dustSlider.isEnabled = on
            persist()
        }

        switchReplaceFront.setOnCheckedChangeListener { _, on ->
            if (on && cfg.optString("source").isEmpty() && cfg.optString("mode") != "stream") {
                toast("⚠ Selecciona una fuente antes de suplantar")
                switchReplaceFront.isChecked = false
                return@setOnCheckedChangeListener
            }
            VcamController.setReplaceFront(on)
            cfg.put("replace", if (on) "front" else "none")
            updateSafetyStatus()
            toast(if (on) "Modo suplantación frontal activo" else "Modo suplantación desactivado")
        }

        btnRestore.setOnClickListener {
            btnRestore.isEnabled = false
            btnRestore.text = "Restaurando…"
            Thread {
                val ok = VcamController.emergencyRestore()
                runOnUiThread {
                    btnRestore.isEnabled = true
                    btnRestore.text = "🔧 Restaurar cámaras físicas"
                    switchMain.isChecked = false
                    switchReplaceFront.isChecked = false
                    cfg = VcamController.readConfigRaw()
                    stopPreview()
                    updateSafetyStatus()
                    toast(if (ok) "✓ Cámaras físicas restauradas" else "⚠ Ejecuta restore de nuevo")
                }
            }.start()
        }

        handler.postDelayed(object : Runnable {
            override fun run() {
                if (switchMain.isChecked || switchReplaceFront.isChecked) {
                    if (!VcamController.cameraStackHealthy()) {
                        toast("⚠ Stack de cámara caído — auto-restaurando")
                        VcamController.emergencyRestore()
                        runOnUiThread {
                            switchMain.isChecked = false
                            switchReplaceFront.isChecked = false
                            updateSafetyStatus()
                        }
                    } else updateSafetyStatus()
                }
                handler.postDelayed(this, 15000)
            }
        }, 15000)

        // ---- Sliders (0–100 ↔ real) ----
        rotSlider.max = 3                        // 0/90/180/270
        zoomSlider.max = 200                     // → 1.0–3.0
        brightSlider.max = 100                   // → ±50
        contrastSlider.max = 20                  // → 0.5–2.0 (10 = 1.0)
        noiseSlider.max = 10
        dustSlider.max = 10
        contrastSlider.progress = 10

        rotSlider.setOnSeekBarChangeListener(s { v, _ ->
            cfg.put("rotation", v * 90); persist(); applyPreviewTransform() })
        zoomSlider.setOnSeekBarChangeListener(s { v, _ ->
            cfg.put("zoom", 1.0 + v / 100.0); persist(); applyPreviewTransform() })
        brightSlider.setOnSeekBarChangeListener(s { v, _ ->
            cfg.put("brightness", v / 2.0 - 25.0); persist(); applyPreviewFx() })
        contrastSlider.setOnSeekBarChangeListener(s { v, _ ->
            cfg.put("contrast", (v + (10 - v) * 0.0) / 10.0); persist(); applyPreviewFx() })
        noiseSlider.setOnSeekBarChangeListener(s { v, _ ->
            cfg.put("noiseLevel", v.toDouble()); persist() })
        dustSlider.setOnSeekBarChangeListener(s { v, _ ->
            cfg.put("dustLevel", v.toDouble()); persist() })

        // ---- Pad de movimiento ----
        btnUp.setOnClickListener { move(0f, +0.1f) }
        btnDown.setOnClickListener { move(0f, -0.1f) }
        btnLeft.setOnClickListener { move(-0.1f, 0f) }
        btnRight.setOnClickListener { move(+0.1f, 0f) }
    }

    private fun move(dx: Float, dy: Float) {
        cfg.put("panX", (cfg.optDouble("panX", 0.0) + dx).coerceIn(-1.0, 1.0))
        cfg.put("panY", (cfg.optDouble("panY", 0.0) + dy).coerceIn(-1.0, 1.0))
        persist(); applyPreviewTransform()
    }

    private fun persist() = VcamController.writeConfig(cfg)

    // ---------------- PREVIEW (lo que se inyecta, en directo) ----------------

    private fun startPreview() {
        val mode = cfg.optString("mode", "video")
        val src = if (mode == "stream") urlInput.text.toString().trim() else cfg.optString("source", "")
        if (src.isEmpty()) { toast("Selecciona una fuente primero"); return }

        if (mode == "image") {
            preview.surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) {
                    val bmp = BitmapFactory.decodeFile(src)
                    // We need a custom View to draw bitmap as TextureView cannot just set bitmap
                }
                override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) {}
                override fun onSurfaceTextureDestroyed(st: SurfaceTexture) = true
                override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}
            }
            setPreviewModeVisibility()
            applyPreviewTransform()
            applyPreviewFx()
            return
        }

        stopPreview()
        preview.surfaceTextureListener = object : TextureView.SurfaceTextureListener {
            override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) {
                try {
                    val mp = MediaPlayer()
                    // Copia a caché local (content:// no es accesible al HAL directamente)
                    val path = if (src.startsWith("content://")) {
                        copyToVcamCache(Uri.parse(src)) ?: run { toast("No se pudo copiar"); return }
                    } else src
                    mp.setDataSource(path)
                    mp.setSurface(Surface(st))
                    mp.isLooping = true
                    mp.prepareAsync()
                    mp.setOnPreparedListener { it.start(); sourceInfo.text = "Fuente: ${path.substringAfterLast('/')}" }
                    player = mp
                } catch (e: Exception) { toast("Preview: ${e.message}") }
            }
            override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) {}
            override fun onSurfaceTextureDestroyed(st: SurfaceTexture) = true
            override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}
        }
        setPreviewModeVisibility()
        applyPreviewTransform(); applyPreviewFx()
    }

    private fun copyToVcamCache(uri: Uri): String? {
        return try {
            val isImage = cfg.optString("mode") == "image"
            val ext = if (isImage) ".jpg" else ".mp4"
            val out = java.io.File(cacheDir, "vcam_src$ext")
            contentResolver.openInputStream(uri)?.use { input ->
                out.outputStream().use { input.copyTo(it) }
            }
            val final = "/data/adb/vcam/source$ext"   // ¡el MOTOR lee desde aquí!
            com.topjohnwu.superuser.Shell.cmd("cp ${out.absolutePath} $final && chmod 644 $final && echo ok").exec()
            if (com.topjohnwu.superuser.Shell.cmd("ls $final").exec().isSuccess) {
                cfg.put("source", final); persist(); final
            } else out.absolutePath   // si falla copia al menos seguimos con preview local
        } catch (e: Exception) { null }
    }

    private fun stopPreview() { player?.release(); player = null }

    private fun applyPreviewTransform() {
        if (preview.width == 0) return
        val rot = cfg.optInt("rotation", 0).toFloat()
        val zoom = cfg.optDouble("zoom", 1.0).toFloat()
        val px = cfg.optDouble("panX", 0.0).toFloat()
        val py = cfg.optDouble("panY", 0.0).toFloat()
        val m = Matrix()
        m.postRotate(rot, preview.width / 2f, preview.height / 2f)
        m.postScale(zoom, zoom, preview.width / 2f, preview.height / 2f)
        m.postTranslate(px * preview.width * 0.25f, -py * preview.height * 0.25f)
        preview.setTransform(m)
    }

    private fun applyPreviewFx() {
        if (Build.VERSION.SDK_INT >= 31) {
            val b = cfg.optDouble("brightness", 0.0).toFloat()   // ±50
            val c = cfg.optDouble("contrast", 1.0).toFloat()
            val m = ColorMatrix(floatArrayOf(
                c, 0f, 0f, 0f, b,     // R
                0f, c, 0f, 0f, b,     // G
                0f, 0f, c, 0f, b,     // B
                0f, 0f, 0f, 1f, 0f))
            preview.setRenderEffect(RenderEffect.createColorFilterEffect(ColorMatrixColorFilter(m)))
        }
    }

    private fun setPreviewModeVisibility() {
        preview.visibility = TextureView.VISIBLE
        previewState.visibility = TextView.VISIBLE
    }
    
    private fun updateSafetyStatus() {
        val replace = switchReplaceFront.isChecked
        val active  = switchMain.isChecked
        safetyStatus.text = when {
            replace && active  -> "🔴 SUPLANTANDO cámara frontal (mod. reemplazo)"
            active             -> "🟢 Inyección activa (cámara virtual añadida)"
            else               -> "⚪ Modo normal — cámaras físicas"
        }
        safetyStatus.setTextColor(
            when {
                replace && active -> 0xFFFF5252.toInt()
                active            -> 0xFF4CAF50.toInt()
                else              -> 0xFF9E9E9E.toInt()
            }
        )
        switchReplaceFront.isEnabled = active   // solo se puede suplantar si hay inyección
        if (!active) switchReplaceFront.isChecked = false
    }

    private fun requestRuntimePermissions() {
        val perms = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= 33) {
            if (checkSelfPermission(android.Manifest.permission.READ_MEDIA_VIDEO)
                    != android.content.pm.PackageManager.PERMISSION_GRANTED)
                perms.add(android.Manifest.permission.READ_MEDIA_VIDEO)
            if (checkSelfPermission(android.Manifest.permission.READ_MEDIA_IMAGES)
                    != android.content.pm.PackageManager.PERMISSION_GRANTED)
                perms.add(android.Manifest.permission.READ_MEDIA_IMAGES)
            if (checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS)
                    != android.content.pm.PackageManager.PERMISSION_GRANTED)
                perms.add(android.Manifest.permission.POST_NOTIFICATIONS)
        } else if (Build.VERSION.SDK_INT >= 29) {
            if (checkSelfPermission(android.Manifest.permission.READ_EXTERNAL_STORAGE)
                    != android.content.pm.PackageManager.PERMISSION_GRANTED)
                perms.add(android.Manifest.permission.READ_EXTERNAL_STORAGE)
        }
        if (perms.isNotEmpty())
            requestPermissions(perms.toTypedArray(), 200)

        if (!Settings.canDrawOverlays(this)) {
            toast("Habilita 'Mostrar sobre otras apps' para los controles flotantes")
            startActivity(Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                Uri.parse("package:$packageName")))
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleShortcut(intent)
    }

    private fun handleShortcut(intent: Intent?) {
        when (intent?.action) {
            "vcam.action.QUICK_START"  -> quickStart()
            "vcam.action.QUICK_STOP"   -> quickStop()
            "vcam.action.QUICK_RESTORE" -> quickRestore()
        }
        intent?.action = null
    }

    private fun quickStart() {
        val saved = VcamController.readConfigRaw()
        val src   = saved.optString("source", "")
        val mode  = saved.optString("mode", "video")

        if (mode != "stream" && src.isEmpty()) {
            toast("⚠ No hay fuente previa — selecciona una")
            return
        }
        cfg = saved
        cfg.put("active", true)
        cfg.put("replace", if (switchReplaceFront.isChecked) "front" else saved.optString("replace", "none"))
        VcamController.writeConfig(cfg)
        VcamController.setReplaceFront(cfg.optString("replace") == "front")

        switchMain.isChecked = true
        startForegroundService(Intent(this, InjectionWatchdogService::class.java))
        startPreview()
        updateSafetyStatus()
        toast("✓ Inyectando: ${src.substringAfterLast('/', if (mode=="stream") "stream" else "")}")
    }

    private fun quickStop() {
        cfg.put("active", false)
        cfg.put("replace", "none")
        persist()
        VcamController.setReplaceFront(false)
        switchMain.isChecked = false
        switchReplaceFront.isChecked = false
        stopService(Intent(this, InjectionWatchdogService::class.java))
        stopPreview()
        updateSafetyStatus()
        toast("⏹ Inyección detenida")
    }

    private fun quickRestore() {
        btnRestore.isEnabled = false
        Thread {
            val ok = VcamController.emergencyRestore()
            runOnUiThread {
                btnRestore.isEnabled = true
                switchMain.isChecked = false
                switchReplaceFront.isChecked = false
                cfg = VcamController.readConfigRaw()
                stopPreview()
                updateSafetyStatus()
                toast(if (ok) "✓ Cámaras físicas restauradas" else "⚠ Reintenta desde la app")
            }
        }.start()
    }

    // ---- utilidades ----
    private fun s(block: (Int, Boolean) -> Unit) = object : SeekBar.OnSeekBarChangeListener {
        override fun onProgressChanged(sb: SeekBar?, p: Int, fromUser: Boolean) { if (fromUser) block(p, true) }
        override fun onStartTrackingTouch(sb: SeekBar?) {}
        override fun onStopTrackingTouch(sb: SeekBar?) { VcamController.restartHal() } // aplicar al engine en vivo
    }

    private fun toast(msg: String) {
        toast?.cancel(); toast = Toast.makeText(this, msg, Toast.LENGTH_SHORT).also { it.show() }
    }

    override fun onActivityResult(req: Int, res: Int, data: Intent?) {
        super.onActivityResult(req, res, data)
        if (req == 100 && res == Activity.RESULT_OK) {
            data?.data?.let { uri ->
                val saved = copyToVcamCache(uri)
                sourceInfo.text = "Fuente: ${saved?.substringAfterLast('/')}"
                if (switchMain.isChecked) startPreview()
            }
        }
    }

    override fun onDestroy() { super.onDestroy(); stopPreview() }
}
