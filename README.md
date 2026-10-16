# 🎥 VCam — Cámara Virtual HAL para Android 10–16

Inyección **global** de cámara virtual a nivel **HAL** (Hardware Abstraction Layer).
Sustituye lo que ven la app de cámara nativa, apps de verificación selfie
(banca, KYC) y cualquier app que use la cámara — con un video, imagen o
stream RTMP/RTSP/HTTP enviado por OBS.

```
[OBS / archivo mp4 / jpg] ──► motor C++ (ffmpeg + HAL wrapper)
                          ──► FrameProcessor (zoom, pan, rot, brillo,
                              contraste, ruido, polvo, micro-movimiento IA)
                          ──► gralloc buffer ──► app cliente (bancaria etc.)
```

## ✨ Características

| Función | Detalle |
|---|---|
| 🎬 Fuentes | Video MP4, imagen JPG/PNG, stream RTMP / RTSP / HTTP (OBS) |
| 🔴 Inyección | Nivel HAL — invisible a apps con detección anti-Xposed |
| 🤖 AI Face Bypass | Micro-movimiento subpixel + breathing temporal + metadata HAL3 realista |
| 🛠 Controles | Zoom, pan (arriba/abajo/izq/der), rotación, brillo, contraste |
| 🌫 Realismo | Ruido de sensor, micro-polvo en lente, wobble de autofocus |
| 🪟 Flotantes | Panel overlay arrastrable con power/zoom/rotación/pan |
| 🔄 Replace:Front | Suplanta la cámara frontal REAL (apps bancarias) |
| 🚨 Seguridad | Auto-restore si el stack de cámara cae; restauración manual 1-click |
| 📱 Compatibilidad | Android 10–16, arm64-v8a + armeabi-v7a, Magisk / KernelSU / KSU-Next |

## 📋 Requisitos

- Dispositivo Android 10–16 **rooteado** (Magisk, KernelSU o KernelSU-Next)
- Para desarrollo: Linux/Mac con Android SDK, NDK 26, CMake, Ninja

## 🚀 Instalación

1. Descargar `vcam-vX.X.X-module.zip` y `VCam-X.X.X-release.apk`
2. Instalar el **zip del módulo** desde la app de Magisk/KernelSU
   (Módulos → Instalar desde almacenamiento)
   - Durante la instalación se ejecuta un **selftest** del motor
3. Instalar el **APK** y conceder:
   - Permiso root (diálogo de Magisk/KSU)
   - Permisos de multimedia y notificaciones
   - "Mostrar sobre otras apps" (para flotantes)
4. Abrir VCam → seleccionar fuente → **START INJECTION** ▶

## 📡 Usar con OBS

1. En OBS: Configuración → Emisión → Servicio "Personalizado":
   `rtmp://IP_DE_TU_SERVIDOR/live` con clave `vcam`
   (o usa un servidor RTMP simple con nginx-rtmp en tu PC:
   `docker run -p 1935:1935 tiangolo/nginx-rtmp`)
2. En VCam app: modo **📡 Stream** → pega `rtmp://IP/live/vcam` → START
3. Todo lo que emitas en OBS aparece como cámara frontal del teléfono

## 🧠Cómo engaña a la verificación selfie (liveness)

- **HAL3 metadata realista**: timestamps SENSOR_TIMESTAM REALTIME monotónicos,
  ISO/exposición coherentes, rolling shutter, focal EXIF real
- **Micro-movimiento**: ninguna persona real está 100% quieta — el motor
  aplica breathing subpixel sinusoidal de baja frecuencia
- **Ruido + polvo**: frames "demasiado limpios" delatan inyección
- **Nivel HAL**: no hay ni Xposed ni hook de Java que detectar

## 🗂 Estructura del código

```
VCam/
├── native/                  Motor C++ (NDK)
│   ├── hal_wrapper_aidl.cpp   Provider AIDL (Android 13–16)
│   ├── hal_wrapper_hidl.cpp   Wrapper HIDL (Android 10–12)
│   ├── video_source.cpp       ffmpeg: mp4/jpg/RTMP/RTSP → NV12
│   ├── frame_processor.cpp   zoom/pan/rot/brillo/ruido/polvo/IA
│   ├── gralloc_access.cpp    lock de buffers YUV del framework
│   └── vcam_config.cpp        config.json compartida con la app
├── app/                    APK de control (Kotlin)
│   ├── MainActivity.kt        UI completa + preview
│   ├── FloatingControls.kt    Panel overlay arrastrable
│   ├── InjectionWatchdog.kt   Foreground watchdog
│   └── VcamController.kt      Root (libsu) + config.json
├── module/                 Módulo Magisk/KSU
│   ├── customize.sh           Instalación + selftest + OEM detect
│   ├── service.sh             Daemon + watchdog + replace:front + SELinux
│   └── sepolicy.rule         Reglas para KernelSU
└── build_all.sh            UN comando: ffmpeg → engine → APK → zip
```

## 🔨 Compilación

```bash
export SDK_DIR=$HOME/Android/Sdk
export NDK_DIR=$HOME/Android/Sdk/ndk/26.2.11394342
./build_all.sh                 # todo (~15 min la 1ª vez, ffmpeg cached después)
./build_all.sh --only-apk     # iterar rápido en la app
./build_all.sh --skip-ffmpeg  # recompilar solo motor
```
El CI de GitHub Actions (`.github/workflows/build.yml`) publica
APK + módulo automáticamente al crear un tag `v*`.

## 🛠 Solución de problemas

| Problema | Causa probable | Solución |
|---|---|---|
| Selftest FAIL en instalación | SELinux estricto del OEM | Revisa `/data/adb/vcam/vcam.log`; el módulo se instala igual |
| Cámara no aparece en apps | App abre cámara por id fijo | Activa `Replace:Front` |
| Stream se congela | Pantalla apagada mató el feed | El foreground service lo evita; verifica notificación "VCam activo" |
| Cámara negra tras desactivar | cameraserver atascado | Botón 🔧 Restaurar cámaras físicas |
| Xiaomi: nada funciona | MiuiCam bloquea providers | Modo `replace:front` congela el provider Xiaomi: usar siempre |
| Samsung: selftest binder FAIL | Knox/secure folder | Verifica que root no esté en estado "incompleto" en Magisk |

Logs: `/data/adb/vcam/vcam.log` · Estado: `/data/adb/vcam/install_status.json`

## ⚠️ Aviso

Uso bajo tu responsabilidad. Puede incumplir los Términos de Servicio
de las apps donde lo uses.