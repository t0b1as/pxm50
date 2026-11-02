# PXM50 → HA-Panel — NOTES (v0.8)

## Status
- v0.1 bootet: DDR2-Default (BBW) ok, 512 MB, 720 MHz, LCD ok, RGMII/AR8031 1 Gbit ok
- v0.2: NAND read-only (+ HW-WP) ok: mtd0 nand-all 1 GB, erase 128 KiB
- v0.3: blue-and-red-wiring = "crossed" (fbdev läuft RG16; mit straight Tux-Füße cyan),
  TSL2561 @ i2c1 0x49, Testbild liest Format aus DRM-debugfs, deploy.sh schiebt auch zImage

## Hardware (verifiziert)
- TASI-100, AM3354 PG1.0 @720 MHz, 512 MB DDR2 (2x Micron D9MTG x8), NAND K9K8G08U0D 1 GB
- LVDS DS90C387 → AUO G156XW01 V1, 24-bit LCD auf lcd_data0-15 + gpmc_ad8-15
- I2C0: 0x2d/0x12 TPS65910, 0x50/0x51 EEPROM (vermutl. 24C04, Siemens factoryset)
- I2C1: 0x49 = TSL2561 Lichtsensor (ID 0x50) → Auto-Helligkeit möglich
- USB: nichts dran
- Ethernet: RGMII1, AR8031 @0, phy-mode rgmii-id passt

## Touch – GEFUNDEN (v0.4)
- Siemens-Kernel 3.2.0-r8 (TI SDK) + EETI-Vendortreiber egalax_i2c, tslib, Qt 4.8 QWS
- I2C2 auf spi0_sclk (sda) / spi0_d0 (scl), mode 2
- INT/Wakeup: gpio1_24 (verifiziert per GPIO1-DATAIN-Snapshot)
- I2C2-Takt 20 kHz PFLICHT – bei 100 kHz nur 1. Byte gültig (Rest 0xff, "0x09"/"0x07"-Müll)
- Mainline egalax_ts (eeti,egalax_ts @0x04) passt: Report 0x04, X/Y 16 bit LE, max ~32760
- Probe beim Boot evtl. zu früh (NACK) → S11touch weckt (1 s Low) + bindet nach
- Siemens-pointercal (Rohwert >>4): 43990 -98 -471744 -232 24576 -588 65536 1376 768
- Umgerechnet für Mainline (c,f,s ×16): 43990 -98 -7547904 -232 24576 -9408 1048576 1376 768

## HA-Panel (v0.6)
- Paket package/pxm50-panel, Quelle src/pxm50-panel (main.c, lv_conf.h), LVGL 9.2.2 aus Paket pxm50-lvgl (local-Pakete laden keine EXTRA_DOWNLOADS)
- fbdev-Flush mit R/B-Tausch (fb0 = BG16), tslib-Indev, DejaVu via TinyTTF
- HA (v0.8): WebSocket /api/websocket, subscribe_entities nur für ents[], Ping 10 s, Reconnect 1..30 s Backoff
  REST: Service-Calls (Queue), Zeit-Sync (Date-Header), Fallback-Polling 3 s wenn WS > 20 s stumm
  nur ws:// (http) – https braucht TLS (nicht implementiert)
- Kamera /api/camera_proxy ~1 fps, 420x236
- Schalter: Antippen = toggle (v0.8.1; vorher langer Druck) – UWP WW, UWP WP=switch.tasmota, UWP S, IRH 650, IRH 1300
- Thermostate: −/+ 0,5 K, Aus/Auto/Heizen
- Backlight nach Lux (EMA), 60 s voll nach Touch; rote LED = HA nicht erreichbar
- Config: /etc/pxm50/ha.conf (HA_URL, HA_TOKEN, CAMERA) – Token eines eigenen Nicht-Admin-Users
- Start: /etc/init.d/S90panel (Schleife), Log /var/log/pxm50-panel.log
- v0.7: Kamera-Vollbild (Seite 2): Tap auf Kachel → 1376x768, Tap → zurück, KEINE Auto-Rückkehr (v0.7.1);
  camera_proxy mit ?width=&height= (serverseitige Skalierung, falls Integration unterstützt)
- Strom-Karte (v0.8.1): Netz groß neutral (kein Rot/Grün), Batterie Leistung + Ladung (deye_battery_power/soc), WP Leistung, Bezug, Einspeisung, WP Energie
- übrige Deye-Werte weiterhin nicht im Panel
- Zeit: S49ntp (busybox ntpd, NTP_SERVER in ha.conf) + Panel stellt Uhr per HTTP-Date von HA (alle 10 min, >3 s Drift)
- Kopfzeile: "Wochentag, TT.MM.JJJJ   HH:MM:SS" (TZ CET/CEST in S90panel)
- deploy.sh: ls-Liste mit "|| true" (fehlende libz.so* brach sonst still ab)

## Siemens-NAND (Dump liegt vor)
- 0x080000 U-Boot 2011.09 "pxm2" · 0x280000/0x780000 Kernel A/B · 0x1480000 UBI
- UBI-Volumes: rootfs_a, rootfs_b, configuration

## Backlight
- PWM-Polarität normal (höherer Wert = heller)

## Pins/GPIOs
LCD-Power gpio3_15 · Backlight-EN gpio3_16 · eCAP0 PWM · Touch-Power gpio1_25
LED gpio3_21 · DFU-Taste gpio1_27 · NAND-WP# gpio0_31 (Hog LOW)

## Workflow
- Bauen: ./build.sh (inkrementell, DTB via post-image)
- Deploy: ./deploy.sh <IP> → zImage + DTB + Skripte, Reboot. SD neu nur bei MLO/U-Boot/Rootfs-Paketen.
- Kernel-Fragment geändert: make ... linux-reconfigure all

## Offen
- [ ] NAND raw dump (nanddump -n -o) + EEPROM-Backup
- [ ] I2C2-Takt ggf. auf 50 kHz hochtesten
- [ ] FIFO-Underflows beim Boot beobachten
- [ ] Link-Flaps beobachten (ggf. EEE aus)
- [ ] PMIC + cpufreq, memtester
- [ ] v0.6 auf Hardware testen (Layout, Fonts, Kamera-fps)
- [ ] später: History-Kurve, Deye-Seite, Entities in Config auslagern
