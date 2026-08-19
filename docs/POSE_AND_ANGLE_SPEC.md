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

## Variants as edits, not as layers

There is a better way to get a feature to animate than compositing a part onto
a face, and it removes the problem this project has spent the most time on.

**One full picture per angle, plus full pictures identical to it except for the
eyes.** The packer works out what differs and stores only that rectangle; the
device draws the angle once and then pushes the rectangle whenever the eye
state changes.

Nothing is registered, because nothing has to be. The difference *is* where the
feature is, by construction. There is no anchor to measure, no canvas to agree
on, no interocular distance to get wrong - and those three between them cost a
day.

Measured on a real angle from the existing sheets, with the eye area edited:

| Variant | Rectangle found | On screen | Bytes | Rate |
|---|---|---|---|---|
| `closed` | 408 x 128 | 102 x 32 | 6.5 KB | 129/s |
| `half` | 400 x 72 | 100 x 18 | 3.6 KB | 235/s |

Tighter than the fixed 128x44 eye tile the pack uses today, which is 11 KB,
because it is exactly what moved rather than a box drawn around where the eyes
were expected to be.

`tools/diff_variants.py` does this, and it doubles as the acceptance test for
the delivery. A variant that was genuinely *edited* produces a small, densely
filled rectangle. One that was redrawn from scratch produces a rectangle
spanning the canvas, and the tool says so:

```
closed.png    48,48 1080x912   123120   60%  <- spans the canvas; the whole picture was redrawn
```

That is the failure this project keeps hitting, finally visible to a machine
rather than to an argument.

### What to ask for

For each angle: the picture, then the same picture with the eyes changed.

> Here is the picture for this head angle. Give me the same picture again with
> the eyes closed. Change nothing else - not the hair, not the shading, not the
> background. Everything except the eyes must be identical, pixel for pixel.

Five of those per angle - `open`, `half`, `closed`, `look_left`, `look_right` -
and four more for the mouth. The instruction is the simplest one in this whole
document, and it is the one that cannot be got wrong in a way we would not
notice.

### And the angles are already drawn

`head_pose8_set.png` holds sixteen usable heads: front, turned left and right
at two depths each, looking up, chin down. Rendered at 320x240 they are clean,
because they were never composited - a whole cell has no seam to get wrong.

So the angles cost nothing. What has to be commissioned is the *variants*, and
only for the angles that need to animate.

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

## The screen is not all picture

A companion that answers out loud should also show what it heard and what it
said - a reply is easier to check than to ask for again. So the bottom of the
screen is a text band, and the artwork gets what is left.

Mocked at full size and read on a 320x240 panel, with M5GFX's built-in
Japanese gothic:

| Band | Text | Result |
|---|---|---|
| 40 px | 14 px, one line | fits, but one line only |
| 48 px | 16 px, two lines | the second line is clipped |
| **56 px** | **16 px, two lines** | **both lines fit, 20 full-width characters each** |
| 64 px | 20 px, two lines | the text runs off the right edge |

**56 px**, then: a status strip and two lines of 16 px Japanese, which is 40
characters - enough for the short replies this thing gives.

That leaves the artwork **320 x 184** of the 320 x 240 screen.

It does *not* mean asking for a 1280 x 736 canvas. Three deliveries in this
project came back at 1448 x 1086 against a request for 1280 x 960 - the ratio
exact to four decimal places, the pixel count never once right. The generator
works in its own sizes and honours an aspect; it does not honour a resolution.
Asking for 40:23 would be pushing that further for no gain.

So: **the canvas stays 4:3 and covers the whole screen, and the band lives
inside it.** On a 1280 x 960 canvas the band is the bottom 224 px. Nothing
that matters goes below **y = 736**; the character is composed for the 1280 x
736 above it.

Ask for the aspect and for every file in a set to be the same size as the one
it was edited from. Both of those the generator does reliably. The scale is
normalised on the way in, which is a resize and cannot go wrong.

The band is always there rather than appearing with text. A character that
grows and shrinks by a quarter whenever it speaks is worse than one composed
for the space it actually has.

## What the framing allows, which is less than it sounds

Look at `assets/kizuna/_review/template_body_front.png` before writing a pose
list. The delivered base is framed head-and-shoulders: the chin sits about two
thirds down the canvas and the shoulders run off the bottom edge. There is no
room for an arm, let alone a hand on a hip.

That leaves two ways to have poses at all:

- **Zoom out.** More of the body fits and the face shrinks with it. Rendered
  at the real size and compared: at 0.8 the eyes are 77 px across and still
  crisp; at 0.65 they are 63 px and still clearly legible; at 0.5 they are 48
  px and it starts to go. The byte cost moves the right way too - a smaller
  eye tile is 5 KB rather than 11.
- **Bring the hands to the head.** A hand cupped at the ear, a knuckle under
  the chin, a wave at head height - all inside the tight frame already.

**Both, at about 0.65 - which is the framing of the character's own gesture
sheet.** That sheet already draws explaining, pointing, presenting, cheering,
listening with a hand at the ear, thinking, apologising, a thumbs up, rubbing
an eye and a raised fist, all with the hands in shot. Matching its framing
means the reference art is the specification.

The hands still stay at or above shoulder height. That is not a stylistic
choice; below the shoulders is off the bottom of a 184 px picture.

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

## The eyes move; the head cuts

Measured on the new framing, with the head's bounding box at 138 x 126 px:

| What moves | Bytes | Rate |
|---|---|---|
| Mouth | 2.7 KB | 301/s |
| Eyes | 4.5 KB | 182/s |
| The whole head | 34 KB | 24/s |

Seven times the headroom for a glance as for a turn. That ratio is what
decides how this character moves.

**Gaze, blink and lip sync run continuously.** There is bandwidth for six
times the 30 Hz the display loop offers, so nothing about them needs
rationing.

**A head turn is a cut, not an animation.** At 24 fps a 0.3 second turn is
seven frames, and seven drawn intermediate heads per angle is the wrong thing
to commission: it is seven more chances for the registration to drift, in a
project that has already spent a day on exactly that. The head changes angle
in one repaint and the eyes lead the way there - which is what animation does
anyway, for the same reason, and it reads as natural rather than as thrift.

So an angle needs **one head**, not a sequence to it:

| Per angle | Files |
|---|---|
| `base/<angle>.png` | 1 |
| `eyes/<angle>/` | 5 |
| `mouths/<angle>/` | 4 |
| `brows/<angle>/` | 3 |
| `hair_front/<angle>.png` | 1 |
| | **14** |

Four angles is 56 files. Drawing the turn instead would be 84 more heads for
motion the bus cannot deliver smoothly regardless.

**Rigid motion is the exception, and it is free.** A nod or a tilt is the same
drawing at a different offset, so the intermediate positions are computed, not
drawn - one head, any number of steps. It is only a change of *angle* that
needs a new drawing, because a turning head occludes itself and no amount of
interpolating between two pictures invents the ear that was hidden.

## Registration

Unchanged, and it is what makes the composite work without any per-part
arithmetic: **every file is the full 1280x960 canvas, transparent except for
its own part, with that part where it belongs in the finished picture.**

For angled parts this matters more, not less. `eyes/turn_right_15/open.png`
has the eyes where they land on a head turned 15 degrees - which is not where
they land on a front-facing one. Because the part carries its own position,
nothing has to compute the offset, and nothing can get it wrong.
