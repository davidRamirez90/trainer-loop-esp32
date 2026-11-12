# TrainerLoop UI/design gap inventory

Status: first presenter alignment pass, 2026-09-18

## Source of truth

The three concept boards in this directory define the intended visual language
for the fixed 368×448 SH8601 AMOLED display. They are concepts rather than
pixel exports, so physical-device photos remain authoritative for legibility,
wrapping, contrast, and touch affordance.

The implementation keeps the existing snapshot and command model. This pass
changes the LVGL presentation layer only.

## Device constraints

- Display: 368×448 portrait pixels; the enclosure masks the outermost bezel.
- Rendering: LVGL 9.2.2, partial RGB565 flush, 40-line DMA buffers.
- Fonts currently enabled: Montserrat 14, 24, and 48.
- Input: FT3168 touch with the existing snapshot-driven event callbacks; BOOT
  remains a separate physical gesture path.
- Memory/performance: ESP32-S3 with 8 MB PSRAM; avoid per-frame allocation and
  large image assets. The presenter retains a bounded set of LVGL objects.
- Safety: disconnected or stale-power rides must remain paused and visually
  dominant; UI changes must not enable resume or end a ride outside existing
  command guards.

## Inferred visual principles

- True black canvas with charcoal surfaces and a conservative 16 px content
  inset.
- White is reserved for primary information; muted blue-gray supports labels
  and explanatory copy.
- Teal means ready, connected, saved, or primary action. Amber means caution
  or pending work. Coral means danger or a stop action.
- Use a small vocabulary of rounded surfaces, full-width primary actions, and
  full-width secondary actions. Avoid mixing unrelated button geometries.
- Use tabular-looking large numerals for power, duration, and ride metrics;
  use 14 px for labels/status and 24 px for screen titles/actions.
- Treat progress, upload state, and safety state as explicit visual objects,
  not trailing text appended to the active dashboard.

## Prioritized gaps

| Priority | Area | Gap | First-pass response |
|---|---|---|---|
| P0 | Shared frame | Repeated hex colors, mixed insets, reused dashboard geometry | Centralized presenter tokens, 16 px grid, shared surface/button helpers |
| P0 | Ride safety | Paused, stop-confirmation, and trainer-loss states looked like text variants | Dedicated Paused, Stop Confirmation, and STOP PEDALING compositions |
| P0 | Active ride | Power, sensors, timing, terrain, and progress competed as one string stack | Metric surfaces, explicit interval/workout progress tracks, semantic hierarchy |
| P1 | Home/Preview | Profile, readiness, and actions were packed into a generic dashboard | Shared Today/Preview composition with chart surface and semantic actions |
| P1 | Completion | Saved result did not distinguish local success from upload pending | Dedicated saved layout, duration hero, summary surface, upload status surface |
| P2 | History/Pairing | Rows were functional but did not share the concept-board row treatment | Consistent charcoal rows, readable two-line copy, full-width navigation actions |
| P2 | Settings | One selected field was shown as a mini editor; no system-level field list | Five aligned rows with selected-row emphasis; existing +/-/next/save commands kept |
| P3 | Setup/Error/empty | Setup and missing-workout/error states lacked cards and hierarchy | Shared surfaces and semantic empty/error layouts; QR and physical wrapping remain open |

## Decisions and assumptions

1. Keep the coordinator state machine and ride behavior unchanged. Add only a
   small view-state flag and `kOpenWorkoutPreview` command so a cached workout
   can remain selected while Home stays sparse and Preview is explicit.
2. Keep Montserrat 14/24/48 rather than enabling more fonts until physical
   photos show a real need; this avoids a speculative font/memory change.
3. Keep device readiness visible on Home as a passive status rail: small colored
   marks plus `TRN`, `HR`, and `WI-FI` text. These marks have no button surface
   or navigation callback; actual buttons remain visually distinct below.
4. Render the disconnected pause variant from the existing safety message.
   This preserves the existing safety gate while giving the user the intended
   STOP PEDALING emphasis.
5. `Upload Status` remains an enum-level state rather than a separate runtime
   navigation screen; completion/history own the visible upload feedback today.
6. Home is the default Today surface: one tappable workout card, passive link
   status, and utility navigation. Preview owns duration, FTP, interval profile,
   readiness, Start, and Back/Next controls.
7. Empty/loading Home uses the same frame as synced Home: a dark informational
   card, the passive status rail, Retry as the primary action, and the same
   Devices/History/Settings utility row. Long operational copy must be shortened
   or explicitly wrapped before reaching the physical display.
8. Back is a secondary navigation action throughout the utility screens. The
   primary treatment is reserved for the screen's task: scan, save, retry
   upload, resume, or end the ride.
9. At normal viewing distance, readability improves more from reducing dense
   metadata than from globally enlarging the UI. Pairing omits full MAC
   addresses, history uses compact workout names, and ride summaries use short
   metric labels while preserving the underlying data.
10. A paused ride now exposes a full-width coral-outlined `DISCARD RIDE`
    secondary action. It opens a dedicated confirmation before deleting only
    the in-progress journal and returning to Today/Home; `END & SAVE` remains
    the separate persistence action, and `KEEP PAUSED` preserves the existing
    recovery path.

## Review sequence

1. Build and host-contract verification.
2. Physical photos of Home, Preview, Active, Paused, Stop Confirmation,
   Completion, and trainer-disconnected pause.
3. Adjust wrapping, contrast, perceived density, and touch target placement
   from those photos.
4. Physical photos of Pairing, History, History Detail, Settings, Setup, and
   empty/error states.
5. Final regression check for every existing command path and state guard.

## Physical review request

After flashing the built application through the existing authorized device
workflow, photograph each requested screen straight-on with the full display
and surrounding bezel visible. Do not crop away the top/bottom edges.

For milestone one, capture:

- Home: boot to the cached-workout screen; include all four navigation tiles.
- Preview: tap the selected workout; include the profile bars and Start/Next
  controls.
- Active: start a ride far enough to show target/actual power and both progress
  bars; keep the whole display visible.
- Paused: tap Pause while connected; include Resume and End & Save.
- Stop Confirmation: tap End & Save from Paused; include both choices.
- Trainer disconnected: while riding, remove trainer connectivity and wait for
  the safety pause; include STOP PEDALING and the End & Save control.
- Completion: finish/save a short ride; include duration, summary, and upload
  status.
- Discard: pause a short ride, tap `DISCARD RIDE`, photograph the confirmation,
  then confirm `DISCARD RIDE` and verify the device returns to Today/Home without
  adding a history row.

Use straight-on photos for all seven. One photo per state is sufficient unless
text wraps or an edge is clipped; in that case include a second photo of the
same state after holding the device still. No simulator image is considered
physical validation.
