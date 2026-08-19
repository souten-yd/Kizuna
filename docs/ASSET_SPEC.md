# Asset packs

A pack is a directory on the SD card holding one character. Packs sit side by
side and the device picks one by name, so adding a character never disturbs
an existing one.

```
/companion/
  config/device.json              Wi-Fi, server, brightness, active pack
  packs/
    claudecode/
      manifest.json
      base/<expression>.m5a       full screen, 1 frame
      eyes/<expression>.m5a       12 frames, one per eye slot
      mouth/<expression>.m5a      8 frames, one per viseme
      clips/<gesture>.m5a         full-screen gesture animations
    someone-else/
      ...
```

## The .m5a container

Raw, panel-native RGB565. No compression, no decoding on the device.

```
offset  size  field
0       4     magic 'M','5','A','1'
4       2     version (1)
6       2     flags
8       2     width
10      2     height
12      2     frameCount
14      2     fps hint (0 = the application decides)
16      4     frameBytes = width * height * 2
20      1     pixel format (0 = RGB565 big-endian, the ILI9341 wire order)
21      11    reserved
32      ...   frames, back to back
```

Frame *i* starts at `32 + i * frameBytes`, so seeking is one multiply. Big-endian
is not an accident: it is what the panel expects, so M5GFX pushes the bytes
with no per-pixel conversion.

See `include/AssetFormat.hpp` and `tools/m5a.py` - they must agree.

## Eye slots

Fixed order, shared between the packer and the firmware
(`m5a::EyeSlot` / `tools/m5a.py`):

```
0 open_center   3 open_up     6 half            9  wide
1 open_left     4 open_down   7 almost_closed   10 sleepy_half
2 open_right    5 soft_lower  8 closed          11 sleepy_closed
```

Visemes 0-4 track loudness from quiet to wide; 5 is a rounded "o", 6 and 7 are
the closed and open smiles the director selects when the expression is happy.

## Adding a character

Everything about which artwork means what lives in a recipe file, not in code:
`assets/characters/<name>.json`. Copy `claudecode.json` and edit it.

```bash
python tools/pack_assets.py --character assets/characters/mine.json --out build/sd
python tools/push_sd.py                       # over USB, incremental
```

A recipe declares:

| Key | Meaning |
|---|---|
| `theme` | `light` or `dark` backdrop |
| `geometry.eye_center` / `eye_span` | where the eyes are pinned on screen, and how far apart |
| `geometry.eye_rect` / `mouth_rect` | the rectangles the renderer blits |
| `sheets` | file name per sheet key; each must be a 4x4 grid of cells |
| `eye_slots` | slot name, sheet key, cell index - order must match the firmware |
| `visemes` | same, for the mouth |
| `expressions` | firmware expression name, source cell, backdrop accent colour |
| `gestures` | full-screen clips: sheet, frame list, fps |

Cell indices are the number printed on the sheet minus one.

## How the packer builds a pack

The source sheets are generated independently, so nothing is registered:
head size and position differ from cell to cell. The pipeline fixes that.

1. **Find the eyes.** The irises are the only cool-toned thing on the
   character - skin runs an R−B of about +65, the brown hair about +15, and the
   iris is genuinely blue-grey. That sign flip separates eyes from hair and
   shadow far more reliably than brightness.
2. **Re-anchor the outliers.** A closed eye has no iris, and the detector
   sometimes locks onto a brow instead and reports success. Every cell is
   therefore checked against its own silhouette - which does not move whether
   the eyes are open or shut - and outliers are replaced.
3. **Cut out the white background** by flood filling inwards from the top
   edge, never the bottom: on several sheets the character is cropped by the
   bottom edge, and a seed there floods straight through the white T-shirt.
4. **Composite** onto a generated backdrop with the eyes pinned to
   `eye_center`.
5. **Graft** each variant's eye or mouth rectangle onto every expression's
   base: aligned by correlating the *ring around* the rectangle, tone-matched
   to the surrounding skin, and blended through a mask that is inset before
   blurring so it reaches zero inside the tile border. Without the inset the
   mask sits at half strength along the edge, and since the tile is cropped to
   exactly that rectangle, every edge pixel ships as a 50/50 mix of two faces -
   a visible seam on all four sides.
6. **Write** the `.m5a` files and the manifest.

Gesture clips skip the grafting. They are placed once from their first frame
and every later frame inherits that placement, because for a nod the head
moving *is* the animation - pinning each frame's eyes would cancel it.

## Sizing the rectangles

Tile size is a bandwidth decision. Each tile crosses the shared SPI bus twice
per draw, and the budget is around 850 KB/s. The shipped geometry:

| Layer | Size | Bytes | At 30 Hz |
|---|---|---|---|
| eyes | 128x44 | 11,264 | 338 KB/s |
| mouth | 64x40 | 5,120 | 154 KB/s |
| base | 320x240 | 153,600 | one-off, ~180 ms |

Eyes and mouth rarely change on the same tick - blinks are occasional, lip
sync is continuous - so the steady-state cost during speech is the mouth
figure. Enlarging either rectangle scales its row linearly; check the numbers
before doing it.
