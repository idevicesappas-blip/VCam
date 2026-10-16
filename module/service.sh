#!/system/bin/sh
# VCam service.sh — inicia/repara inyección según OEM y modo
MODDIR=${0%/*}
VCAM_DIR=/data/adb/vcam
CFG=$VCAM_DIR/config.json
ENGINE_AIDL=$MODDIR/system/vendor/bin/hw/vcam_provider
ENGINE_HIDL=$MODDIR/system/vendor/bin/hw/libvcam.so

# ---------------- utilidades ----------------
log() { echo "[$(date '+%H:%M:%S')] vcam: $*" >> $VCAM_DIR/vcam.log; }

wait_until_login() {
    # el HAL puede reiniciarse durante el boot; esperamos boot completo
    while [ "$(getprop sys.boot_completed)" != "1" ]; do sleep 2; done
    while [ "$(getprop init.svc.bootanim)" = "running" ]; do sleep 1; done
    sleep 5
}

provider_pids() {
    ps -A -o PID,NAME 2>/dev/null | grep -i "camera.provider" | awk '{print $1}'
}

provider_running() { [ -n "$(provider_pids)" ]; }

# ---------------- 1. Detectar servicio provider del VENDOR (por OEM) ----------------
detect_vendor_service() {
    # Nombres reales por OEM (grep sobre las props init):
    #   Pixel/AOSP:    vendor.camera.provider-aidl  (Android 13+)
    #                  vendor.camera-provider-2.7-service (Android 10-12 HIDL)
    #   Samsung:       vendor.camera-provider-2-7-service  / camera.provider-aidl
    #   Xiaomi/Redmi:  vendor.camera-hal-server /  camera.provider-aidl
    #   Oppo/Realme:   vendor.camera.provider
    #   Huawei:        vendor.camera.provider-2-x
    SVC=""
    for s in $(getprop | grep -oE '\[init\.svc\.[^]]*camera[^]]*\]' \
              | tr -d '[]' | sed 's/init\.svc\.//'); do
        case "$s" in
            *camera*) SVC="$SVC $s" ;;
        esac
    done
    # También buscar por procesos vivos (más fiable)
    DEV_SERVICE=$(ps -A -o NAME 2>/dev/null | grep -i "camera.provider\|camera-hal\|cameraserver" | head -3)
    log "Servicios detectados: [$SVC] / procesos: [$DEV_SERVICE]"
    # Persistir para que customize.sh / app lo lean
    echo "$SVC" | tr ' ' '\n' | grep -v '^$' > $VCAM_DIR/vendor_services.txt
    echo "$DEV_SERVICE" | tr ' ' '\n' | grep -v '^$' >> $VCAM_DIR/vendor_services.txt
}

# ---------------- 2. SELinux (re-aplicarlo tras cada boot) ----------------
apply_selinux() {
    # El provider AIDL de vcam corre en su propio dominio si se inicia como root
    # desde Magisk (= magisk domain), pero si se adopta como servicio vendor
    # necesita permisos para leer nuestro config y las fuentes de media.
    if command -v magiskpolicy >/dev/null 2>&1; then
        magiskpolicy --live "allow * { media_rw_data_file } file { rw_file_perms open getattr }" 2>/dev/null
        magiskpolicy --live "allow * { adb_data_file } file { rw_file_perms open getattr }" 2>/dev/null
        magiskpolicy --live "allow cameraserver { adb_data_file } file { rw_file_perms open getattr }" 2>/dev/null
        magiskpolicy --live "allow hal_camera_default { adb_data_file } file { rw_file_perms open getattr }" 2>/dev/null
    fi
    # vcam_provider como root (magisk) → dominio suelto, no necesita más
}

# ---------------- 3. Lanzar el provider AIDL (Android 13+) como root ----------------
start_vcam_daemon() {
    # Evitar duplicados
    if pidof vcam_provider >/dev/null 2>&1; then
        log "provider ya corriendo (pid $(pidof vcam_provider))"
        return 0
    fi
    API=$(getprop ro.build.version.sdk)
    if [ "$API" -ge 33 ]; then
        export LD_LIBRARY_PATH=$MODDIR/system/vendor/lib64:/vendor/lib64:/system/lib64
        nohup $ENGINE_AIDL >> $VCAM_DIR/vcam.log 2>&1 &
        log "provider AIDL lanzado pid=$!"
    else
        # En HIDL (Android 10-12) el engine se activa por LD_PRELOAD del
        # servicio vendor — se reinicia el provider para que cargue:
        restart_vendor_provider
    fi
}

# ---------------- 4. Modo SUPRIMIR cámara frontal REAL (opcional) ----------------
# Se activa escribiendo {"replace":"front"} en config.json
check_replace_mode() {
    REPLACE=$(grep -o '"replace":"[^"]*"' $CFG 2>/dev/null | cut -d'"' -f4)
    [ "$REPLACE" != "front" ] && return
    # 1. Para el provider vendor del OEM
    for SVC in $(cat $VCAM_DIR/vendor_services.txt 2>/dev/null); do
        [ -z "$SVC" ] && continue
        # Solo detenemos el que contenga "provider" o "hal"
        case "$SVC" in
            *provider*|*hal*)
                setprop init.svc.$SVC stopped 2>/dev/null
                log "detenida init.svc.$SVC (modo replace-front)" ;;
        esac
    done
    # Si el OEM lo relanza desde init, lo obligamos a dormir con un pgrep-loop
    (
        while [ "$(grep -c '"replace":"front"' $CFG)" -gt 0 ] 2>/dev/null; do
            [ -f $VCAM_DIR/APPLY_REPLACE ] && break
            for P in $(provider_pids); do kill -STOP $P 2>/dev/null; done
            sleep 5
        done
        for P in $(provider_pids); do kill -CONT $P 2>/dev/null; done
    ) &
    log "modo REPLACE-FRONT activo; cámaras físicas congeladas"
}

# ---------------- 5. HIDL: reinicio con LD_PRELOAD (Android 10-12) ----------------
restart_vendor_provider() {
    API=$(getprop ro.build.version.sdk)
    [ "$API" -ge 33 ] && return    # AIDL no usa preload
    SVC_LIST=$(cat $VCAM_DIR/vendor_services.txt 2>/dev/null)
    if [ -z "$SVC_LIST" ]; then
        detect_vendor_service
        SVC_LIST=$(cat $VCAM_DIR/vendor_services.txt 2>/dev/null)
    fi
    for SVC in $SVC_LIST; do
        case "$SVC" in
            *provider*)
        # verilog: relanzar el proceso del vendor con LD_PRELOAD de libvcam.so.
        # En init rc los servicios se declaran con setenv; usamos el truco de
        # magisk: sustituir binario por wrapper script del mismo nombre en el
        # overlay del módulo (hecho en customize.sh) — aquí solo reiniciamos.
                setprop init.svc.$SVC stopped
                sleep 1
                setprop init.svc.$SVC running
                log "provider HIDL reiniciado con engine preload ($SVC)"
                ;;
        esac
    done
}

# ---------------- 6. Watchdog: mantener vcam_provider vivo ----------------
watchdog() {
    (
        while true; do
            sleep 10
            [ ! -f $CFG ] && continue

            # Aplicar cambio de replace IMMEDIATO si la app lo pidió
            if [ -f $VCAM_DIR/APPLY_REPLACE ]; then
                rm -f $VCAM_DIR/APPLY_REPLACE
                NEWREPLACE=$(grep -o '"replace":"[^"]*"' $CFG | cut -d'"' -f4)
                if [ "$NEWREPLACE" = "front" ]; then
                    check_replace_mode      # congela providers vendor
                else
                    # descongelar TODO
                    for P in $(provider_pids); do kill -CONT $P 2>/dev/null; done
                    log "replace=$NEWREPLACE → providers vendor descongelados"
                fi
            fi

            # Check health
            ACTIVE=$(grep -o '"active":true' $CFG 2>/dev/null | head -1)
            if [ -n "$ACTIVE" ] && ! pidof vcam_provider >/dev/null 2>&1; then
                API=$(getprop ro.build.version.sdk)
                [ "$API" -ge 33 ] && start_vcam_daemon
                log "watchdog: provider relanzado"
            fi
            # lint de SELinux por si se perdió
            [ $(( $(date +%s) % 600 )) -lt 10 ] && apply_selinux
        done
    ) &
}

# ============================================================
#                          MAIN
# ============================================================
wait_until_login
mkdir -p $VCAM_DIR
[ -f $CFG ] || echo '{"active":false,"mode":"video","source":"","zoom":1.0,"rotation":0,"panX":0,"panY":0,"brightness":0,"contrast":1.0,"noiseLevel":0,"dustLevel":0,"aiFaceBypass":true}' > $CFG
touch $VCAM_DIR/vcam.log

log "=== VCam service iniciado (Android $(getprop ro.build.version.release) / $(getprop ro.product.manufacturer)) ==="
apply_selinux
detect_vendor_service
check_replace_mode

# Si la inyección ya estaba activa (persistencia al reiniciar boot)
ACTIVE=$(grep -o '"active":true' $CFG 2>/dev/null | head -1)
if [ -n "$ACTIVE" ]; then
    log "inyección estaba activa → re-lanzando motor"
    start_vcam_daemon
    API=$(getprop ro.build.version.sdk)
    [ "$API" -lt 33 ] && restart_vendor_provider
fi

watchdog
log "=== service.sh completado ==="
