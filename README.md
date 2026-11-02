# PXM50 → Home-Assistant-Wandpanel

Ausgemusterte **Siemens Desigo PXM50** Touchpanels (15,6", S55623-H120, Board „TASI-100“)
als freies Linux-Wandpanel für Home Assistant – ohne Lötarbeiten, ohne Eingriff in die
Original-Firmware (bootet von SD-Karte, NAND bleibt hardwareseitig schreibgeschützt).

## Hardware

| Komponente | Details |
|---|---|
| SoC | TI AM3354 (PG1.0), 720 MHz |
| RAM | 512 MB DDR2 (2× Micron x8) |
| Flash | 1 GB SLC-NAND (Samsung K9K8G08U0D) – wird nicht beschrieben |
| Display | AUO G156XW01 V1, 1366×768 (LCDC 1376×768), LVDS über DS90C387 |
| Touch | EETI eGalax an I2C2 (0x04), **nur 20 kHz** stabil, INT = gpio1_24 |
| Licht | TSL2561 an I2C1 (0x49) |
| Netz | Gigabit (AR8031, RGMII), Versorgung 24 V AC/DC, vermutl. PoE |

Alle Pins, Timings und Fundstellen stehen in [`NOTES.md`](NOTES.md) und im
Device-Tree [`board/pxm50/am335x-pxm50.dts`](board/pxm50/am335x-pxm50.dts).

## Bauen

Linux-Host (oder WSL2) mit den üblichen Buildroot-Abhängigkeiten
(`build-essential git rsync bc cpio unzip file wget python3 libncurses-dev`).

```bash
./build.sh          # klont Buildroot 2025.02.19-115-gefc09a0fce nach ~/buildroot, Erstbuild ca. 1 h
```

Ergebnis: `output/images/sdcard.img`

## Flashen & Booten

```bash
sudo dd if=output/images/sdcard.img of=/dev/sdX bs=4M conv=fsync status=progress
```

1. SD-Karte ins Panel, **DIP-Schalter SW1 auf ON („SD BOOT“)** – OFF bootet wieder das Siemens-Original.
2. 24 V anlegen, LAN anstecken. IP per DHCP, Login `root` / `pxm50` (**ändern:** `passwd`).

## Home Assistant anbinden

1. In HA einen eigenen Benutzer **ohne Admin-Rechte** anlegen, damit einloggen,
   Profil → Sicherheit → *Langlebiges Zugriffstoken* erstellen.
2. Auf dem Panel:
   ```bash
   cp /etc/pxm50/ha.conf.example /etc/pxm50/ha.conf
   vi /etc/pxm50/ha.conf          # HA_URL, HA_TOKEN, CAMERA, optional NTP_SERVER
   chmod 600 /etc/pxm50/ha.conf
   /etc/init.d/S90panel restart
   ```
3. **Entities anpassen:** Die angezeigten Entities stehen aktuell fest in
   [`src/pxm50-panel/main.c`](src/pxm50-panel/main.c) (Tabelle `ents[]`, Schalter `tiles[]`,
   Thermostate `thermos[]`, Zeilen in `ui_build()`). Für eine andere Installation dort ändern
   und neu bauen.

Bedienung: Schalter per **langem Druck**, Thermostate −/+ und Aus/Auto/Heizen,
Tippen auf das Kamerabild → Vollbild, erneutes Tippen → zurück.

## Entwickeln ohne SD-Wechsel

```bash
make -C ~/buildroot O=$PWD/output BR2_EXTERNAL=$PWD
./deploy.sh <panel-ip>     # Kernel, DTB, Overlay, Panel-App per SSH, dann Reboot
```

## Hinweise

- Kein Siemens-Code und keine Firmware-Teile in diesem Repo. Erkenntnisse zur Hardware
  stammen aus dem Mainline-U-Boot (`board/siemens/pxm2`) und eigener Analyse.
- Die Original-Firmware bleibt im NAND unangetastet; vor Experimenten trotzdem ein
  Backup ziehen (`nanddump -n -o /dev/mtd0`, siehe NOTES.md).

## Lizenz

Siehe [LICENSE](LICENSE): eigener Code MIT, Device-Tree GPL-2.0.
