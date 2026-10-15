# Held-arrow navigation: diagnosis and regression roadmap

## Picasa reference

Google's 2009 account of Picasa Web Albums explicitly describes holding an
arrow key and moving through an album “flip-book style,” while still filling
the browser view with large images:
https://googleblog.blogspot.com/2009/06/picasa-web-albums-stays-big-gets-faster.html

The Picasa Photo Viewer quick-start guide describes arrow-key selection in the
desktop viewer:
https://sites.google.com/site/picasaresources/picasa/quick-start-guide

These sources establish the interaction target, not Picasa's private decoding
or caching implementation. The Google post concerns the web viewer, so its
exact performance does not establish a desktop benchmark.

## Observed behavior and scope

In v0.3.0, holding Left or Right can advance the selected filename faster than a
preview can be decoded and painted. The outgoing photo then remains on screen
through several selections. The current tests cover a single transition and
request cancellation, but do not check a sustained sequence of key repeats or
which filenames reach the screen.

## Code path and likely cause

1. `Main.qml` calls `browseImage()` for each arrow shortcut. It immediately
   chooses the adjacent file and calls `loadImageUrl()`.
2. Every replacement calls `releasePictureResourcesForNavigation()`, which
   cancels all active image requests. This correctly prioritizes the newest
   selection, but prevents an in-flight intermediate photo from appearing.
3. `ImageViewport.qml` restarts a zero-delay request timer whenever its source
   changes. Multiple source changes before that timer runs collapse to the
   latest source. A completed stale response is rejected as well.
4. The last ready texture stays visible until another photo is ready. That
   avoids a blank frame but makes fast repeats look frozen.
5. The nearby-preview worker is cancelled at each navigation step and is
   rescheduled after an image becomes ready. When repeat is faster than decode,
   the cache cannot reliably get ahead. Metadata inspection for each selected
   non-RAW file also runs on the UI thread and may add delay.

The first four points follow directly from the v0.3.0 code. The relative cost
of metadata, cache misses, decode, and texture upload still needs measurement
on the reported image set.

## Intended behavior

During a held-arrow run, each *displayed navigation step* must have a visible
preview for at least one completed screen frame before the next step replaces
it. Decode speed and display refresh set the upper bound on that rate. Excess
key-repeat events must not build an unbounded backlog that keeps moving after
release. A distinct deliberate key press should still move one image. Once
navigation is idle, the selected photo upgrades to its normal quality.

## Roadmap

1. **Measure and reproduce.** Add a held-key diagnostic that records key
   events, selected filenames, request starts/cancellations, preview-ready
   times, and frame-swapped filenames. Run it on cached and cold mixed-format
   folders at repeat intervals of 10, 20, 40, and 80 ms. Capture UI-thread
   time spent in metadata inspection and directory lookup.
2. **Separate input from presentation.** Give burst navigation a pending
   direction/step state. Advance at most one photo per ready preview and
   presented frame. Do not cancel that step merely because more auto-repeat
   events arrived. On release, discard excess repeat demand; keep the last
   displayed photo or finish the single step already in progress. Preserve
   separate one-step behavior for deliberate presses.
3. **Use a fast visual tier.** In burst mode request a small, bounded preview
   (and any prepared neighbor) before full resolution. Keep speculative full
   reads, expensive metadata, and optional media probes idle until the burst
   stops. Resume normal preview/full progression for the settled photo.
4. **Make prefetch useful while moving.** Prepare a directional window ahead
   of the displayed photo. Retain overlapping entries when the center moves;
   do not cancel a useful neighbor decode at every step. Respect the existing
   memory budget and cancel work that is genuinely outside the new window.
5. **Regression gate.** Assert that every *admitted display step* appears in
   frame-swapped order, at least once, with correct filename and image pixels;
   the old photo stays visible while the next preview loads; final selection
   receives full detail after idle; reverse direction and rapid release work;
   memory remains bounded. Include slow RAW/TIFF and cache-miss cases. Measure
   key-to-first-preview and sustained visible photos per second against v0.3.0.

## Practical limit

The app cannot visibly show more unique photos per second than the screen can
paint or the decoder can supply. Meeting “every image shown” requires pacing
navigation to presentation, rather than applying every hardware auto-repeat
event immediately. Burst previews may initially look softer; the settled
image should sharpen automatically.

## Prototype in progress

The first prototype in `qml/Main.qml` and `qml/ImageViewport.qml` caps held-key
preview requests at 2 megapixels and advances after the preview's frame swap.
It holds at most one subsequent step while a preview is loading and suppresses
the outgoing-image fade during browsing. This uses decode and presentation
events; there is no fixed delay between displayed photos. The existing idle
timer still controls only the return to full-resolution rendering.

The `licasa_navigation_burst` integration test sends 30 rapid navigation calls
against 12 distinct, large PNG fixtures. It checks consecutive filenames and
samples the center pixel of each painted frame against the expected image
color. It also verifies that the settled image reaches full resolution. The
prototype passed this test on the local software renderer. Manual testing on
the user's actual display and image folders remains the next validation step.

## Follow-up and local handoff (2026-10-14)

The prototype now drops its one queued repeat when the physical arrow key is
released. Auto-repeat key-release events are ignored; a real release also
cancels a step already scheduled for the next event-loop turn. A quick reversal
keeps the newly requested direction. The production window manager forwards
these key releases to the viewer without consuming the key event.

`licasa_navigation_burst` now checks 1, 10, 20, 40, and 80 ms repeat intervals,
leftward travel, reversal, and release while a repeat is queued. It verifies
consecutive painted filenames and center pixels from twelve 3000 × 2000 PNGs,
the nearby-cache byte bound, and full-detail promotion after idle. The complete
modern profile passed all 46 registered tests, including AVIF, HEIF, JPEG XL,
RAW, APNG, motion playback, QML, navigation, and viewer lifecycle checks. Both
the application and diagnostic QML lint targets passed.

An additional cold, offscreen sequence used symlinks to the unchanged local
PNG, DNG, CR2, 13,583 × 5,417 JPEG, and 10,072 × 8,087 TIFF samples. At 10 ms,
four consecutive photos were painted from twelve calls; at 80 ms, all twelve
were painted in order. The settled photo reached full detail with a 1000 ms
idle observation. Excess repeats are deliberately coalesced.

For a reference comparison, the last committed build (`414f5ea`) and the
current modern build each received thirty presses against the same synthetic
PNG directory with cold app settings and the software renderer. Only the
reference diagnostic's input-validation minimum was lowered to permit 10 ms;
the reference viewer code was unchanged. Each figure is one run, counting
painted navigation steps rather than filenames selected ahead of the screen.

| Repeat interval | Committed build | Current build |
| ---: | ---: | ---: |
| 10 ms | 1 | 9 |
| 20 ms | 1 | 16 |
| 40 ms | 29 | 30 |
| 80 ms | 30 | 30 |

The current build is installed as `Licasa (Local)` in `~/.local/bin/licasa`
with the modern image plugins and motion backend. The installed binary starts
and quits cleanly offscreen, and its registered formats and desktop entry were
checked. The desktop X server did not respond to a basic display query during
part of this session, then recovered; a physical compositor review was not
completed. On the next
desktop check, hold each arrow through a mixed folder, release during a slow
preview, reverse direction, and inspect whether each admitted photo is visible
without a long pause or a late extra step. An in-progress very large
full-resolution texture upload may still delay input until that render work
finishes.
