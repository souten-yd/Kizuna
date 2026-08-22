# When the cable comes out

The symptom: on battery, holding the button to speak leaves the device frozen
showing LISTEN. On USB it never happens.

This document is about why that is a power problem, why it is hard to see, and
what the firmware now does about it.

## Why you cannot just measure it

Two pieces of bad news, both worth knowing before you spend an evening on this.

**There is no battery voltage to read.** The classic Core's PMIC is an IP5306,
and its entire battery interface over I2C is a four-bit field that
M5Unified turns into one of five numbers:

```cpp
// M5Unified/src/utility/power/IP5306_Class.cpp
case 0x00: return 100;   case 0x08: return 75;
case 0x0C: return 50;    case 0x0E: return 25;
default:   return 0;
```

So `battery: 71%` anywhere in this project is a smoothed average of those five
steps. It is not a measurement of anything happening on a millisecond scale, and
**a cell sagging under load looks exactly like a cell that is fine**. A worn cell
reads 100% right up until it does not.

**The brownout detector is set to its lowest threshold.** arduino-esp32 ships
`CONFIG_ESP32_BROWNOUT_DET_LVL=0`, the least sensitive of the eight. The CPU
will keep running through a sag that has already upset the SD card and the
radio, both of which want a clean 3.3 V. That is why the failure presents as a
freeze rather than a reboot: **the peripherals give up before anything resets
and writes down why.**

A frozen LCD is also consistent with this. The panel holds its last frame with
no refresh - so a display task that has stopped, for any reason, looks exactly
like a device showing LISTEN forever.

## Why LISTEN is the moment it happens

Holding the button turns on, in the same instant:

| load | rough draw |
|---|---|
| ESP32 core, radio on with modem sleep disabled | ~100 mA |
| Wi-Fi transmit, 50 frames a second at full power | bursts to several hundred mA |
| backlight at 160/255 | tens of mA |
| ten SK6812 pixels running the LISTEN meter | tens of mA |
| SD reads + SPI writes for the face changing to `listening` | a burst |
| microphone ADC | small |

[The ESP32 draws up to 500 mA in
peaks](https://randomnerdtutorials.com/esp32-brownout-detector-was-triggered/)
when the radio is working. All of that goes through a boost converter fed by a
small cell, and an old cell has internal resistance. Resistance times current is
a voltage drop, and the drop lands on everything at once.

On USB the supply absorbs it. On battery it does not. That is the whole
difference, and it is why the failure is perfectly reproducible on one power
source and invisible on the other.

## What the firmware does about it now

### On battery, three things are given up

Applied whenever the charging state changes, so plugging and unplugging while
you watch is a live experiment:

| given up | why that one |
|---|---|
| Wi-Fi transmit power, capped at 13 dBm | the biggest single spike, and the standard fix; range is the cheapest thing to trade |
| the LED bar, off entirely | ten pixels at their brightest during the exact moment the radio is busiest |
| backlight, capped at 110/255 | visible, and cheap to give back |

The LED bar is switched off rather than dimmed, because at brightness zero this
firmware stops clocking the strip at all rather than clocking ten pixels of
zeroes with interrupts disabled.

```json
{ "battery_saver": false, "tx_power_dbm": 17 }
```

in `device.json` turns it off or retunes it. The status page reports
`battery_saver` and the transmit power the radio actually accepted.

### The bursts are moved apart

The face changing to `listening` is a burst of SD reads and SPI writes. The
radio starting is a burst of transmit spikes. They used to begin on the same
button press.

Now the audio uplink waits 150 ms before the first frame goes up. **Nothing is
lost** - the capture queue holds 320 ms, so the delayed chunks are sent, just
later. The reply arrives a fraction of a second further out and nobody can tell.

This is `appcfg::kUplinkHoldoffMs`, and it is the one change that is specifically
about *when* rather than *how much*.

### It says where it was when it died

`listen: begin` and `listen: end` are logged, with the battery reading, whether
the cable is in, and the transmit power in force. Those lines land in the RTC
tail, which survives a panic, a watchdog and a software reset.

So after a lock-up: power-cycle, then look.

```bash
tools/m5net.py log        # the tail from before the restart is printed first
tools/m5net.py boots      # or: did it reboot at all, and for what reason
```

If the last line before the restart is `listen: begin`, that is your answer.

## Finding out which load it is

Since the sag cannot be measured, apply the loads separately instead. The
firmware turns them on one stage at a time and **announces each stage before it
starts**, so a device that dies partway through leaves the name of the thing
that killed it as the last line in the log.

```bash
tools/m5net.py power           # 6 seconds a stage, over Wi-Fi
tools/m5net.py power 15        # longer, for a marginal cell
```

or `power [seconds]` on the serial console.

| stage | loads |
|---|---|
| 1 | quiet - screen dim, no LEDs, no mic, no radio |
| 2 | + backlight at full |
| 3 | + LED bar at full white |
| 4 | + microphone capturing |
| 5 | + radio sending 640-byte frames 50 times a second |
| 6 | everything at once - this is what LISTEN does |

Stage 5 uses a UDP broadcast to a port nothing listens on, so it needs no server
- the point is the radio transmitting, in the same shape the uplink does it.

Read the result like this:

- **dies at 5 or 6, survives 1-4** - the radio. Lower `tx_power_dbm` further;
  11 dBm is still a usable link on a home network.
- **dies at 3** - the LED bar. Leave `battery_saver` on.
- **dies at 2** - the cell is far gone, or the boost converter is struggling at
  a load any M5GO makes. Replace the battery.
- **survives all six on battery** - it is not the supply, and the freeze is
  something else. Start with `boots`: a reset reason of `task_wdt` or `panic`
  points at the firmware, and the RTC tail has the lines before it.

Run it on the cable first as a control. If a stage fails on USB too, the problem
was never the battery.

## If it is the battery

Nothing in software fixes a cell with high internal resistance; the settings
above only make the device ask less of it. The M5Stack community's answers are
hardware, and they are the same ones that come up for the light-load shutdown:

- replace the cell - the M5GO base is the larger of the two and the usual
  suspect is an old one;
- [a large capacitor across the supply](https://forum.m5stack.com/topic/1822/no-reset-when-usb-power-removed-solved)
  to absorb the transmit spikes;
- [an ideal-diode part such as the MAX40200](https://forum.m5stack.com/topic/1822/no-reset-when-usb-power-removed-solved)
  so the battery feeds the 5 V rail directly.

## The other power fault, which looks nothing like this one

If the device does not freeze but simply *vanishes* - screen dark, no reboot
message, and the next start reports `poweron` - that is the IP5306 switching its
own boost converter off because it decided the load was too small to be worth
supplying. Different fault, different fix, covered in
[REMOTE_DEBUG.md](REMOTE_DEBUG.md#the-ip5306-which-is-probably-your-answer).
The status page's `boost_held` says whether the firmware's mitigation took.
