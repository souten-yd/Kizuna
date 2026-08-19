# Preparing the SD card

## The one thing that will bite you

A 64 GB card ships formatted as **exFAT**. The ESP32 Arduino SD library speaks
FAT12/16/32 only. The card will not mount, and the failure is silent - the
card responds perfectly to the SPI layer, and only the filesystem mount fails.

The firmware distinguishes the two cases and says so on screen:

- *no card* - nothing in the slot, or it is not seated. A card that is present
  but unseated logs `GO_IDLE_STATE failed` and `no token received` at every
  clock rate; that is a connection symptom, not a speed one. Push until it
  clicks.
- *unreadable filesystem* - the card answered but FATFS refused it. On a 64 GB
  card that is almost always exFAT.

## Formatting (destroys everything on the card)

```bash
sudo umount /dev/sdX1
sudo parted /dev/sdX --script mklabel msdos mkpart primary fat32 1MiB 100%
sudo mkfs.vfat -F 32 -s 64 -n M5COMPANION /dev/sdX1
```

`-F 32` forces FAT32 rather than exFAT. `-s 64` selects 32 KiB clusters, which
keeps the allocation table small on a large card. Run it in a real terminal;
`sudo` cannot prompt for a password from a non-interactive shell.

Cards up to 32 GB usually arrive as FAT32 already and need nothing.

## Getting a pack onto the card

Three routes, in order of convenience:

```bash
python tools/push_sd.py                 # over USB, incremental, no card removal
```

Files are compared by CRC first, so re-packing one expression sends a few
hundred kilobytes rather than the whole 11 MB. A full first sync runs about
three minutes at 921600 baud.

**Over Wi-Fi** - once the device is on a network, open `http://<device-ip>/`
to upload, delete and switch packs from a browser.

**With a card reader** - fastest for the very first fill:

```bash
python tools/pack_assets.py --out build/sd
./tools/deploy_sd.sh /media/you/M5COMPANION
```

## Layout on the card

```
/companion/config/device.json
/companion/packs/<pack>/manifest.json
/companion/packs/<pack>/{base,eyes,mouth,clips}/*.m5a
```

## device.json

Edit it on the card and reboot; the values are copied into NVS on boot, so the
card is not needed afterwards.

```json
{
  "wifi_ssid": "your-network",
  "wifi_password": "…",
  "server_host": "192.168.1.10",
  "server_port": 8765,
  "server_path": "/m5companion",
  "device_name": "M5GO-Companion",
  "pack": "claudecode",
  "volume": 150,
  "brightness": 160,
  "led_brightness": 40
}
```

Leaving `wifi_ssid` empty is fine - the companion runs offline. Holding B+C
through power-on opens a setup access point instead.
