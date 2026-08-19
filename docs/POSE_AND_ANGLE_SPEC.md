# Poses, head angles, and what it costs to move them

An extension to [ASSET_BRIEF.md](ASSET_BRIEF.md), written once the device had
been measured rather than guessed at. The measurements are the whole argument
here: they decide which layer a thing belongs in, and therefore how many
drawings are needed.

## The budget, first

The LCD and the SD card share one SPI bus carrying about **850 KB/s**, and a
full 320x240 RGB565 frame is 150 KB. Everything below follows from that.

| What changes | Bytes | Rate | So it can be |
|---|---|---|---|
| Mouth tile | 5 KB | 170/s | continuous |
| Eye tile | 11 KB | 75/s | continuous |
| Eyes + mouth | 16 KB | 53/s | continuous |
| Head region (~200x160) | 64 KB | 13/s | a short move |
| Whole screen | 150 KB | 5.5/s | a transition, once |

**A thing that animates continuously must be a small tile. A thing that
changes on an event can be the whole screen.** That single rule decides the
layer stack.

## The layer stack

```
1. body/<pose>.png           torso, arms, hands - no head
2. base/<angle>.png          head, neck, hair behind the face - no features
3. eyes/<angle>/<name>.png
4. brows/<angle>/<name>.png
5. mouths/<angle>/<name>.png
6. hair_front/<angle>.png    the bangs, drawn over the eyes
7. fx/<name>.png             blush, sweat, sparkles
```

Two changes from the original brief, both earned:

**Poses are a body layer, not a whole frame.** §6 had them as complete
pictures with the face drawn in. That works, but the moment the character
strikes a pose it stops blinking and stops lip syncing, because there is no
longer a tile to update - the face is baked into the pose. Cupping a hand to
its ear while listening is exactly when it should look most alive. So the pose
draws everything below the neck and nothing above it.

**Head angle indexes the features.** A head turned 15 degrees needs eyes drawn
for a head turned 15 degrees; the front-facing pair pasted onto it looks like a
sticker. This is what §3.7 called optional, and it is not optional if the head
is going to turn.

## The matrix, and keeping it finite

Indexing every feature by every angle multiplies out fast: 7 angles x 22 eye
parts is 154 drawings of eyes alone. It does not need to.

**The character emotes facing forward and turns its head to react.** So the
full expressive set exists only at `front`, and an angled head gets the parts
that animate - blink, gaze, a viseme ramp - and nothing else.

| Angle | Eyes | Mouths | Brows |
|---|---|---|---|
| `front` | all 22 | all 20 | all 8 |
| `turn_left_15`, `turn_right_15` | 5 | 4 | 3 |
| `look_up`, `look_down` | 5 | 4 | 3 |
| `tilt_left`, `tilt_right` *(later)* | 5 | 4 | 3 |

The five eyes an angled head needs: `open`, `half`, `closed`, `look_left`,
`look_right`. The four mouths: `rest`, `small`, `medium`, `wide` - enough for a
viseme ramp, which is what lip sync actually uses.

That is 50 additional parts for four angles, against 616 for the naive matrix.

## What the framing allows, which is less than it sounds

Look at `assets/kizuna/_review/template_body_front.png` before writing a pose
list. The delivered base is framed head-and-shoulders: the chin sits about two
thirds down the canvas and the shoulders run off the bottom edge. There is no
room for an arm, let alone a hand on a hip.

That leaves two ways to have poses at all:

- **Zoom out.** More of the body fits, and the face gets smaller in exactly
  the same proportion. The eye tile is 128x44 of a 320x240 screen today; at
  half the size it is 64x22, and a blink stops being legible. This is a bad
  trade for a device whose whole job is a face.
- **Bring the hands to the head.** A hand cupped at the ear, a knuckle under
  the chin, fingers against a cheek, a wave at head height - all of these are
  inside the frame already, and all of them read clearly at 320x240.

**Take the second.** A pose here is a hand entering frame near the head, not a
full-body attitude. It is a tighter vocabulary than a full figure gives, and it
is the one that survives being 240 pixels tall.

`body/` therefore holds the shoulders, the collar and whatever the arms are
doing where they cross into frame - which is usually a forearm and a hand.

## Poses, and what each is for

A pose is named for the state it belongs to, because that is what selects it.
The device has five states and two moods worth drawing:

| Pose | When | What the body does |
|---|---|---|
| `idle` | resting | shoulders and collar only - the neutral everything else is edited from |
| `listening` | button held | one hand cupped behind the ear, that shoulder raised |
| `thinking` | waiting for the reply | a knuckle under the chin |
| `speaking` | talking | one open hand at chest height, just inside the frame |
| `happy` | a cheerful reply | a small wave at head height, shoulders up |
| `sleepy` | idle for a long time | shoulders dropped, no hands |
| `confused` | it did not understand | one shoulder up, one palm turned out at the edge |

Every one of those keeps the hand at or above shoulder height, because that is
where the frame is.

Seven bodies. A pose change costs 150 KB once, which is a fifth of a second -
fine for something that happens when the state changes, and not something to
animate through.

## What this buys

With the stack above, a listening companion is:

- the `listening` body, drawn once when the button goes down,
- the `front` head, drawn once,
- eyes and mouth updating at 30 Hz for the whole time it is listening.

16 KB a frame against a 850 KB/s bus. It blinks and breathes while it listens,
and the hand stays at its ear because nothing needs to redraw it.

Compare with the pose-as-whole-frame model, where the same scene is 150 KB per
frame and therefore holds completely still.

## Registration

Unchanged, and it is what makes the composite work without any per-part
arithmetic: **every file is the full 1280x960 canvas, transparent except for
its own part, with that part where it belongs in the finished picture.**

For angled parts this matters more, not less. `eyes/turn_right_15/open.png`
has the eyes where they land on a head turned 15 degrees - which is not where
they land on a front-facing one. Because the part carries its own position,
nothing has to compute the offset, and nothing can get it wrong.
