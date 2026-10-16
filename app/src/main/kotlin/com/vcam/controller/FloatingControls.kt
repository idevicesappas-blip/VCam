package com.vcam.controller

import android.annotation.SuppressLint
import android.graphics.PixelFormat
import android.view.*
import android.widget.Toast
import android.content.Intent
import android.view.LayoutInflater
import com.google.android.material.button.MaterialButton
import org.json.JSONObject

class FloatingControls : android.app.Service() {

    private var wm: WindowManager? = null
    private var rootView: View? = null

    @SuppressLint("ClickableViewAccessibility")
    override fun onCreate() {
        super.onCreate()
        wm = getSystemService(WindowManager::class.java)

        val params = WindowManager.LayoutParams(
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                or WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
            PixelFormat.TRANSLUCENT
        ).apply {
            gravity = Gravity.TOP or Gravity.START
            x = 40; y = 400
        }

        rootView = LayoutInflater.from(this).inflate(R.layout.floating_controls, null)

        // ---- arrastrar la burbuja completa ----
        var downX = 0f; var downY = 0f; var startX = 0; var startY = 0
        rootView!!.setOnTouchListener { v, ev ->
            when (ev.action) {
                MotionEvent.ACTION_DOWN -> {
                    downX = ev.rawX; downY = ev.rawY
                    startX = params.x; startY = params.y; true
                }
                MotionEvent.ACTION_MOVE -> {
                    params.x = startX + (ev.rawX - downX).toInt()
                    params.y = startY + (ev.rawY - downY).toInt()
                    wm!!.updateViewLayout(rootView, params); true
                }
                else -> false
            }
        }

        fun upd(mut: (JSONObject) -> Unit) {
            val c = VcamController.readConfigRaw(); mut(c); VcamController.writeConfig(c)
        }
        
        val fUp = rootView!!.findViewById<MaterialButton>(R.id.fUp)
        val fDown = rootView!!.findViewById<MaterialButton>(R.id.fDown)
        val fLeft = rootView!!.findViewById<MaterialButton>(R.id.fLeft)
        val fRight = rootView!!.findViewById<MaterialButton>(R.id.fRight)
        val fRot = rootView!!.findViewById<MaterialButton>(R.id.fRot)
        val fZoomIn = rootView!!.findViewById<MaterialButton>(R.id.fZoomIn)
        val fZoomOut = rootView!!.findViewById<MaterialButton>(R.id.fZoomOut)
        val fPower = rootView!!.findViewById<MaterialButton>(R.id.fPower)

        fUp.setOnClickListener   { upd { it.put("panY", (it.optDouble("panY",0.0)+0.1).coerceIn(-1.0,1.0)) } }
        fDown.setOnClickListener { upd { it.put("panY", (it.optDouble("panY",0.0)-0.1).coerceIn(-1.0,1.0)) } }
        fLeft.setOnClickListener  { upd { it.put("panX", (it.optDouble("panX",0.0)-0.1).coerceIn(-1.0,1.0)) } }
        fRight.setOnClickListener{ upd { it.put("panX", (it.optDouble("panX",0.0)+0.1).coerceIn(-1.0,1.0)) } }
        fRot.setOnClickListener   { upd { it.put("rotation", (it.optInt("rotation",0)+90)%360) }; Toast.makeText(this, "Rot ${((VcamController.readConfigRaw().optInt("rotation",0)))}°", Toast.LENGTH_SHORT).show() }
        fZoomIn.setOnClickListener{ upd { it.put("zoom", (it.optDouble("zoom",1.0)+0.1).coerceAtMost(3.0)) } }
        fZoomOut.setOnClickListener{upd { it.put("zoom", (it.optDouble("zoom",1.0)-0.1).coerceAtLeast(1.0)) } }

        // Botón principal: activar/desactivar inyección instantáneo
        fPower.setOnClickListener {
            val on = !VcamController.isInjectionActive()
            upd { it.put("active", on) }
            (it as MaterialButton).text = if (on) "⏹" else "▶"
        }

        wm!!.addView(rootView, params)
    }

    override fun onDestroy() { super.onDestroy(); rootView?.let { wm?.removeView(it) } }
    override fun onBind(intent: Intent?) = null
}
