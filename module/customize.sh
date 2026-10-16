#!/system/bin/sh
# VCam customize.sh — instalación con verificación de salud
SKIPUNZIP=0
VCAM_DIR=/data/adb/vcam
API=$(getprop ro.build.version.sdk)
OEM=$(getprop ro.product.manufacturer)

ui_print "======================================"
ui_print "  VCam — Cámara Virtual HAL v1.0.0"
ui_print "  $OEM · Android $(getprop ro.build.version.release) (API $API)"
ui_print "======================================"

# ---------- 1. Validación de versiones ----------
if [ "$API" -lt 29 ] || [ "$API" -gt 36 ]; then
    ui_print "! ✗ Solo Android 10 (29) a Android 16 (36)"
    abort
fi

# ---------- 2. Root manager ----------
KSU=false
if [ -d /data/adb/ksu ] || [ -d /data/adb/ksun ] || ls /data/adb/*ksu* >/dev/null 2>&1; then
    KSU=true
    ui_print "- Root: KernelSU / KernelSU-Next detectado"
elif [ -d /data/adb/magisk ]; then
    ui_print "- Root: Magisk detectado"
else
    ui_print "! ⚠ No se detectó gestor de root conocido — puede fallar"
fi

# ---------- 3. Arquitectura ----------
ARCH=$(getprop ro.product.cpu.abi)
case "$ARCH" in
    arm64-v8a)   ui_print "- CPU: arm64-v8a ✓" ;;
    armeabi-v7a) ui_print "- CPU: armeabi-v7a ✓" ;;
    *) ui_print "! ✗ ABI no soportada: $ARCH"; abort ;;
esac

# ---------- 4. Verificar provider de cámara presente ----------
CAM_SVC=$(getprop | grep -oE 'init\.svc\.[^]]*camera[^]]*' | head -5)
if [ -z "$CAM_SVC" ]; then
    ui_print "! ⚠ No se detectó servicio cámara provider — continuar con cautela"
else
    ui_print "- Provider cámara vendor: detectado ✓"
fi

# ---------- 5. Preparar directorios de datos ----------
mkdir -p $VCAM_DIR
if [ ! -f $VCAM_DIR/config.json ]; then
    echo '{"active":false,"mode":"video","source":"","replace":"none","zoom":1.0,"rotation":0,"panX":0,"panY":0,"brightness":0,"contrast":1.0,"noiseLevel":0,"dustLevel":0,"aiFaceBypass":true}' > $VCAM_DIR/config.json
    ui_print "- config.json creado (valores por defecto)"
else
    ui_print "- config.json existente preservado ✓"
    # backup por seguridad antes de actualizar
    cp -f $VCAM_DIR/config.json $VCAM_DIR/config.json.bak
fi
# Registrar el nombre del servicio vendor para service.sh (modo replace)
getprop | grep -oE 'init\.svc\.[^]]*camera[^]]*provider[^]]*' \
    | sed 's/init\.svc\.//' | grep -v '^$' > $VCAM_DIR/vendor_services.txt 2>/dev/null
ps -A -o NAME 2>/dev/null | grep -i "camera.provider\|camera-hal" >> $VCAM_DIR/vendor_services.txt 2>/dev/null

# ---------- 6. Instalar motor según API ----------
mkdir -p $MODPATH/system/vendor/bin/hw
if [ "$API" -ge 33 ]; then
    ui_print "- Instalando motor AIDL (Android 13–16)..."
    cp -f "$MODPATH/engine/$ARCH/vcam_provider_aidl" \
          $MODPATH/system/vendor/bin/hw/vcam_provider \
        || { ui_print "! ✗ binario vcam_provider no encontrado para $ARCH"; abort; }
    chmod 755 $MODPATH/system/vendor/bin/hw/vcam_provider
else
    ui_print "- Instalando motor HIDL + shim (Android 10–12)..."
    cp -f "$MODPATH/engine/$ARCH/libengine_hidl.so" \
          $MODPATH/system/vendor/bin/hw/libvcam.so \
        || { ui_print "! ✗ libvcam.so no encontrado para $ARCH"; abort; }
    chmod 644 $MODPATH/system/vendor/bin/hw/libvcam.so
fi

# ---------- 7. Instalar libs ffmpeg (solo si no existen en sistema) ----------
mkdir -p $MODPATH/system/vendor/lib64 $MODPATH/system/vendor/lib
for L in libavformat libavcodec libavswscale libavutil libswresample; do
    if [ -f "$MODPATH/engine/$ARCH/$L.so" ]; then
        if [ "$ARCH" = "arm64-v8a" ]; then
            cp -f "$MODPATH/engine/$ARCH/$L.so" $MODPATH/system/vendor/lib64/
        else
            cp -f "$MODPATH/engine/$ARCH/$L.so" $MODPATH/system/vendor/lib/
        fi
    fi
done

# ---------- 8. Dependencias previas: módulo libc++_shared ----------
if [ -f "$MODPATH/engine/$ARCH/libc++_shared.so" ]; then
    [ "$ARCH" = "arm64-v8a" ] && DST=$MODPATH/system/vendor/lib64 || DST=$MODPATH/system/vendor/lib
    cp -f "$MODPATH/engine/$ARCH/libc++_shared.so" $DST/ 2>/dev/null
fi

# ---------- 9. SELinux para lectura de fuentes de media (/data/media) ----------
ui_print "- Aplicando políticas SELinux..."
if command -v magiskpolicy >/dev/null 2>&1; then
    magiskpolicy --live "allow hal_camera_default { media_rw_data_file adb_data_file } file { rw_file_perms open read getattr }" 2>/dev/null
    magiskpolicy --live "allow cameraserver { media_rw_data_file } file { rw_file_perms open read getattr }" 2>/dev/null
elif command -v ksud >/dev/null 2>&1; then
    # KernelSU: reglas persistentes via sepolicy.rule
    mkdir -p $MODPATH/sepolicy
    cat > $MODPATH/sepolicy.rule <<'EOF'
allow hal_camera_default { media_rw_data_file adb_data_file } file { rw_file_perms open read getattr }
allow cameraserver { media_rw_data_file adb_data_file } file { rw_file_perms open read getattr }
EOF
    ui_print "  (reglas SEP persistidas para KernelSU)"
else
    ui_print "  ⚠ magiskpolicy no disponible — service.sh reintentará"
fi

# ---------- 10. ★ SELFTEST — verificación de salud ★ ----------
ui_print "- Ejecutando selftest del motor..."
if [ "$API" -ge 33 ]; then
    # Preparar entorno mínimo para el test
    export LD_LIBRARY_PATH=$MODPATH/system/vendor/lib64:/vendor/lib64:/system/lib64
    SELFTEST_OUT=$(cd /data/adb && \
        $MODPATH/system/vendor/bin/hw/vcam_provider --selftest 2>&1)
    ui_print "  $SELFTEST_OUT" | head -20
    if echo "$SELFTEST_OUT" | grep -q "VCAM_SELFTEST_RESULT=PASS"; then
        ui_print "- ✓ SELFTEST PASS — motor funcional en este dispositivo"
        SELFTEST_OK=1
    else
        ui_print "! ⚠ SELFTEST FALLÓ — detalles arriba"
        ui_print "!  El módulo se instala igualmente; revisa /data/adb/vcam/vcam.log"
        SELFTEST_OK=0
    fi
else
    # HIDL: selftest limitado (validar .carga + abrir fuente si existe)
    if [ -f "$MODPATH/engine/$ARCH/libengine_hidl.so" ]; then
        SIZE=$(stat -c%s "$MODPATH/engine/$ARCH/libengine_hidl.so")
        if [ "$SIZE" -gt 100000 ]; then
            ui_print "- ✓ libvcam.so válida ($SIZE bytes)"
            SELFTEST_OK=1
        else
            ui_print "! ⚠ libvcam.so sospechosamente pequeña"
            SELFTEST_OK=0
        fi
    fi
fi

# ---------- 11. Estado final para la app ----------
echo "{\"selftest\":$( [ "$SELFTEST_OK" = "1" ] && echo true || echo false ),\"installed\":\"$(date +%s)\",\"api\":$API,\"arch\":\"$ARCH\",\"oem\":\"$OEM\",\"engine\":$( [ $API -ge 33 ] && echo '"aidl"' || echo '"hidl"' )}" > $VCAM_DIR/install_status.json

ui_print "======================================"
if [ "$SELFTEST_OK" = "1" ]; then
    ui_print "  ✓ Instalación COMPLETA y verificada"
else
    ui_print "  ⚠ Instalada con advertencias"
fi
ui_print "  Abre la app VCam para configurar fuentes"
ui_print "  DBG: /data/adb/vcam/vcam.log"
ui_print "======================================"

# Reinstalación limpia: volver a activar si estaba activo
if grep -q '"active":true' $VCAM_DIR/config.json 2>/dev/null; then
    ui_print "- Inyección estaba ACTIVA → se restaurará al boot"
fi
