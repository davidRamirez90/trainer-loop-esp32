# TrainerLoop visual and information design review

Reviewed 30 September 2026 against the current working tree. Recommendation: retain the black OLED canvas, restrained green accent, and two-column power display. Invest first in truthful state messages, then in a more legible interval countdown and simpler setup. A new visual identity would contribute less than these changes.

## Review artifacts

- [Proposed designs: 31 screens with category filters and original/proposed comparison](concept-gallery.html)
- [All 26 current production fixture screenshots](current-gallery.html)
- Render boards: [prepare](prepare-concepts.png), [devices](devices-concepts.png), [ride and safety](ride-concepts.png), [save and history](finish-concepts.png), [settings and phone setup](settings-concepts.png).
- Original contact sheets: [setup and preparation](evidence/contact-1.png), [ride states](evidence/contact-2.png), [history, settings and errors](evidence/contact-3.png).

The gallery is a local HTML review artifact. Category and comparison controls work; simulated app buttons are illustrative. Device proposals are 368×448 CSS pixels, phone proposals 368×720. Browser typography approximates the intended hierarchy; it is not the firmware font rasterizer. Some proposals introduce navigation or state reporting that does not exist today. Captions identify those additions. Original fixtures beside new states are the closest relevant source screen, not proof that the proposed state already exists.

## Evidence and scope

Built the native simulator against the current source (`cmake --build build-simulator --parallel`), captured all 26 production LVGL fixtures, and inspected every capture. Fixture capture verified 368×448 PNG dimensions and visible-control bounds. Baselines were not changed.

Replayed 11 repository scenario streams through the real simulator pointer controls: workout lifecycle, provisioning rejection, provisioning time-sync failure, local-save retry, trainer loss, stale power, interrupted recovery, empty plan, sync authorization failure, upload retry, and settings persistence. These were observation replays, not a fresh full test-suite run. Runtime protocol logs remain under `simulator-artifacts/design-review-2026-09-30/runtime/`. Inspected the production presenter and snapshot mapping to explain ambiguous messages. Reviewed the actual setup HTML in Chromium at a 390×844 mobile viewport; no real credentials or accounts were used.

The simulator provides synchronous setup and service injections; the phone page was opened locally rather than connected to the device provisioning server. Therefore intermediate phone validation, captive-network handoff, expiry, and server-error screens are proposed improvements informed by code, not fully exercised browser flows. Boot was observed through simulator startup; the initialized presenter's earliest startup text is code-inspected. Physical touch, glare, bezel clearance and reading while cycling still need device validation.

## What already works

The design is cohesive: true black background, dark surfaces, strong power values, predictable full-width actions, and explicit local upload status on completion. The trainer-loss screen correctly makes STOP PEDALING the dominant instruction and avoids automatic resume. The preview step provides a useful boundary before starting resistance control. These are worth preserving.

## Highest-priority findings

| Priority | Before | After | Why / evidence |
|---|---|---|---|
| P0 | Stale power enters the ordinary paused screen with “Trainer pause confirmed” and disabled Resume; the actual reason is absent. | “Power signal lost,” “Workout paused,” and “Waiting for fresh power data.” Resume remains guarded. | Runtime state contains `Power stale — workout paused` and `resume_available:false`. [Observed screen](evidence/power_stale-09.png). Users need to understand why they cannot resume. |
| P1 | Manually ending a two-second workout displays “Complete.” | Derive “Complete,” “Partial ride,” or “Recovered ride” from summary data. | [Real partial finish](evidence/workout_lifecycle-14.png). The completion renderer appends Complete unconditionally. |
| P1 | Saving fixture is titled “Save incomplete”; upload-auth fixture also uses “Retry save.” | Separate normal saving, local-save failure, upload pending, and account-auth failure. | [Saving fixture](evidence/error-loading.png), [mixed error fixture](evidence/error.png). Real local-save failure is correctly retryable: [runtime](evidence/storage_save_retry-12.png). The mixed upload error is fixture/presenter evidence, not proof that runtime navigates to that exact combination. |
| P1 | Busy Home becomes a gray disabled card with no visible syncing message despite the snapshot containing one. | Retain a dark readable card; show “Checking today’s plan…” with a small activity indicator. | [Busy fixture](evidence/home-busy.png). Gray washes out hierarchy, while the reason for blocking interaction is invisible. |
| P1 | Cached-workout authorization failure is absent from Home content; empty Home always gives Wi-Fi advice. | Distinguish synced rest day, no cache, connection failure, and rejected credentials. Keep cached training available where allowed. | Runtime sync-auth fault says cached workouts were kept, but Home does not render it. Empty UI uses generic connection advice even when other reasons exist. |
| P1 | Phone setup asks for 11 fields, including Crr, CdA and a raw POSIX rule; status is below the full form. | Three steps: network, account, rider profile. Advanced physics collapsed; timezone selected normally. Inline errors and nearby validation status. | [Actual mobile form](evidence/web-setup-mobile.png). Expert details and network handoff compete with the first-use task. |

## Screen-by-screen recommendations

| Screen / state | Before | After | Why |
|---|---|---|---|
| Startup | Small startup sentence mixes starting and sensor loading. | Brief “Starting TrainerLoop,” followed by actual status or a named recovery action when startup fails. | Users should know whether to wait or intervene. Avoid fabricated progress percentages. |
| Device setup | SSID/PASS/OPEN strings, fixed 10-minute sentence, long password; no numbered sequence. | Two clear tasks: join device Wi-Fi, then open the URL. Label password/case sensitivity. Show remaining time only when measured. | Reduces transcription mistakes and makes the phone handoff clear. A real Wi-Fi QR could help later, provided it fits and scans physically. |
| Setup rejected / time unavailable | In observed setup replays, device remains on the credential/instruction screen; protocol reports the rejection. | Show a concise reason and next step on device and phone: account rejected or time unavailable. Retain safe candidate values for correction. | Failure should not look identical to waiting. Phone failure UI was code-inspected, not exercised with a live backend. |
| Setup expiry / network handoff | Browser catch message sends the user back to device; status sits far below fields. | Keep handoff guidance by the action, distinguish expected network transition from definite failure, and show recovery address only when valid. | Avoids an apparently broken setup when the device switches networks. |
| Today ready | Tappable workout card with a small “Tap for workout details”; much unused space. | Explicit Preview workout action plus tappable card. Clear trainer state and optional HR. | Makes the main task obvious without adding unrelated metrics. |
| Today syncing / busy | Gray card and disabled utility buttons, no named operation. | Readable dark content, visible checking indicator, and unavailable action with reason. | A recognizable screen should remain while loading. Keep only genuinely conflicting operations blocked. |
| Today offline with cache | Red Wi-Fi/trainer markers; saved-plan explanation in snapshot is not displayed. | “Offline · using saved plan,” plus separate trainer readiness. | Offline networking and workout usability are different facts. Use caution rather than danger for routine offline operation. |
| Today no cache | Multiple repeated Wi-Fi instructions and red status rail. | One reason, Retry sync, and a route to connection setup. Saved-rides reassurance. | A next step beats repeated diagnosis. New configuration route requires guarded navigation. |
| Today synced empty / rest day | Uses generic no-cache structure, even when server reports no workout today. | “No workout today · plan synced,” no failure coloring. | A successful empty result is a legitimate state. Preserve freshness provenance across cache updates. |
| Today auth rejected | Cached card remains available but reason is hidden. | Small “Account needs attention; saved plan available” notice with account-recovery route. | Allows training while making the sync problem discoverable. |
| Workout preview | Duration appears twice; readiness/status repeated; Next lacks context. | One duration/interval/FTP summary, profile, one readiness block, Start, and “Next workout”; show selection index. | Removes redundancy and makes multiple-workout navigation intelligible. |
| Preview blocked | Disabled Start unavailable, generic “Trainer not ready.” | Name the actual blocking reason and offer Pair trainer when disconnection is the cause. | A recovery action is more useful than a dead button. Other blockers need their own messages; do not map all failures to pairing. |
| Devices empty | Large blank area, generic Ready, “Scan again” even before results. | Wake/pedal guidance, “No devices found,” Scan for devices, HR optional. | Explains the peripheral discovery task. |
| Devices scanning | Text “Scanning,” but generic Ready remains below; Stop scan emits Back. | Activity indicator; remove Ready. Stop should end scanning and remain on Devices, with Back separate. | Present command mapping conflates canceling a scan with leaving the screen; behavioral change requires coordinator work. |
| Devices results | Role/name combined at 14 px; raw negative dBm. | Name first, role grouping, qualitative signal strength, selected/connected indicator. | Device names support recognition; dBm is low-value metadata. Grouping must map taps to stable identities. |
| Devices connecting / reconnecting | No dedicated fixture for selected, connecting, control-ready progression. | Explicit connecting/checking-control state and recovery after failure. | Discovery is not readiness to control resistance. This needs observable connection-stage data. |
| Active | Good 48 px power pair; no watt labels; repeated tiny interval time; two visually identical unlabeled tracks. | Keep the power pair; add W labels; promote remaining interval time to ~40 px; put each label beside its track. | Rider glance priority is effort, remaining time, then supporting telemetry. |
| Active HR missing | HR becomes `--`; detailed sensor-loss message is not presented. | “Heart rate unavailable · ride continues,” missing value dash, neutral/amber optional-sensor status. | Avoids implying a resistance fault. Render only when true; avoid over-alerting for intentionally unpaired HR. |
| Active power missing | Green `--`; stale warning not displayed in this fixture. | Missing power loses ready coloring; expose stale-data state or the actual safety-pause screen. | Fixture shows a presenter state, while real stale-power runtime pauses. Do not preserve a misleading green power indication. |
| Paused connected | Large green confirmation, three equally broad actions including prominent coral Discard above Resume. | Resume first, End & save second, subdued but accessible Discard… leading to confirmation. | Reduces accidental destructive intent during a sweaty touch interaction. Use at least a 44–48 px hit area even if the label is understated. |
| Paused stale power | Ordinary pause screen with unexplained disabled Resume. | Named signal-loss state, safe-pause confirmation only if verified, and reason Resume is blocked. | Confirmed runtime information is available but not used in composition. |
| Paused trainer lost | Good stop instruction; End & save filled coral, Discard prominent. | Preserve STOP PEDALING and resistance unknown. Make data-preserving action secondary/neutral; keep deletion subordinate. | Hazard information should dominate over destructive controls. Never invent verified resistance or resume automatically. |
| End confirmation | Small question label, oversized generic duration/state card, coral saving action. | Clear question, partial-save consequence, compact active duration and workout context, End & save primary. | Save and delete should not share destructive semantics. |
| Discard confirmation | “Cancel workout?” rather than delete wording. | “Discard this ride?” with lost-duration consequence, Keep paused, coral Discard ride. | Clearly names irreversible data loss; retains existing separate confirmation. |
| Completion | “OK,” unconditional Complete, wall duration labeled simply Duration, absent HR shown as zero in runtime. | “Ride saved,” accurate completion status, explicit active/elapsed duration, meaningful summary units, missing HR as unavailable. | Removes ambiguous success and avoids false physiological data. Keep local-save success separate from upload outcome. |
| Completion upload pending / retry | Short status sentence in green-tinted panel. | Amber pending notice with reason and automatic-retry expectation, only when that policy applies. | Clarifies that local ride data is already safe. |
| Upload auth blocked | Generic upload-status screen or mixed error fixture; only Back. | “Ride saved locally,” credential reason, account correction then retry. | Repeating an upload will not repair credentials. Respect existing provisioning unlock/ride guards. |
| Saving | Save incomplete header despite busy Saving ride text. | “Saving your ride,” single activity indicator, actions blocked only for the persistence operation. | Normal waiting should not appear to have failed. |
| Local save failure | Correct retry model, sparse error paragraph. | State that local save has not finished and explain retained-session retry. | Use a truthful promise supported by storage state, not a generic saved-success message. |
| History empty | Bare “No saved rides yet.” | Short friendly empty message; Back remains available. | Low-priority polish; no illustration is needed on this display. |
| History list | Small dense rows, no date, whole uploaded row green, Retry upload prominent. | Date/name first, concise duration and complete/partial status, upload status separately. Contextual retry only for retryable entries. | Improves recognition and reduces color noise. Long lists need an explicit bounded paging policy. |
| History detail | Large wall duration without labeling, terse AVG/HR string; upload status. | Same metric grammar as completion, active and elapsed time distinguished, date and partial/recovered context. | Users should reconcile the ride screen with the saved record. |
| Recovered interrupted ride | Boot returns Home; later History has a partial pending ride. | A dismissible “Interrupted ride recovered and saved” notice with View ride. | Makes successful recovery visible. [Observed recovered history](evidence/interrupted_recovery-11.png). |
| Rider & physics settings | Five small rows; NEXT FIELD cycling and +/- controls; no source indication. | Tap field to edit; prominent value and units; Save/Cancel; advanced physics separate. Show local vs synced FTP where available. | Avoids hidden selection mechanics and unexplained coefficients. Requires view-state additions rather than a purely visual reskin. |
| Settings edited / saved | Changes apply next ride note; no clear dirty/save-success composition. | Unsaved changes, named saved feedback, and exit handling consistent with actual persistence behavior. | Confirms the edit was accepted and when it takes effect. |
| Phone setup | Dense eleven-field vertical form with disclosures before tasks. | Three-step network/account/profile sequence; grouped labels, help for credentials, advanced defaults, normal timezone selection. | Reduce cognitive load while retaining truthful security information in accessible details. |
| Phone validating / errors | Bottom status sentence; submit is not disabled by script. | Disable duplicate submit, nearby validation state, focus/scroll to failed field, preserved non-secret values, explicit retry. | Feedback must be visible at the point of action. Report checkpoints only if server actually exposes them. |

## Proposed visual system

Use a 16 px content inset and 8–12 px spacing, consistent dark surfaces, 10 px corner radii, and a predictable bottom action area. Prefer mixed-case action labels for easier word recognition. Do not fill disabled controls with light gray: use a dark surface, muted readable label, and a nearby explanation when important.

Keep white primary values, green actions/readiness/local-save success, amber recoverable delay, and coral hazard or deletion. Routine Wi-Fi offline and optional HR absence should not look like emergency resistance faults. Status must be readable in words as well as color. Workout profile colors need their own intensity mapping; the proposal uses muted high blocks to avoid borrowing warning semantics. Never imply that amber workout intensity is an operational warning.

Typography target: power 48–56 px, interval countdown 36–40 px, titles 24–26 px, supporting values 20–24 px, important explanatory copy around 16 px, secondary labels 14 px. Keep numbers stable in width. The shipped font set is currently Montserrat 14/24/48, so intermediate sizes require a considered font/memory change or a first pass using the available fonts. The renders use Arial to communicate hierarchy, not to dictate a firmware typeface change.

Retain workout profiles before the ride. During the ride, emphasize power, countdown, HR/cadence, then clearly named interval/workout progress. Keep virtual distance and speed subordinate. Grade/ascent details can move to a secondary view only if an intentional, discoverable navigation model is added; do not silently remove required information.

Use minimal motion: a small activity indicator for unknown-duration operations and immediate press feedback. Keep operational numbers and safety instructions stable. No decorative transitions, animated charts, or full-screen loading replacement during normal metric updates.

## Implementation sequence and verification

1. **Truthful presenter states first:** partial/complete, missing HR, stale-power pause reason, syncing feedback, and saving versus error headers. Separate cached/offline/rest-day/auth messages. Add the missing visual fixtures using real snapshot data.
2. **Ride and action hierarchy:** larger countdown, named progress tracks, units, dark disabled treatment, reduced discard prominence, explicit confirmation copy, consistent summary durations.
3. **Device/setup tasks:** understandable device rows and readiness stages; repair scan-stop semantics; stepwise phone form, server-specific validation errors, and guarded connection/account recovery links.
4. **Settings/history navigation:** selected-field editor, advanced physics, dirty/save feedback, dates, bounded history paging, and recovery notice. These require deliberate coordinator/view-model additions.

For each implementation pass, replay real pointer paths and fault injection, review screenshots together with control bounds, run relevant existing simulator contracts, and explicitly review any new visual baselines. Preserve safe start/resume/save/discard guards. Check recovery action reachability, long workout/device names, missing telemetry, four-digit power, timezone changes, localization lengths, long error copy, pending and auth-blocked uploads, and multiple-workout selection.

Then review on the physical display at riding distance with glare and wet-touch conditions. The most valuable physical check is whether power, remaining interval time, and Pause can each be recognized in a quick glance. Compare colors and margins against the actual enclosure rather than treating desktop pixels as proof of physical readability.

## Changes made for this review

Documentation, evidence captures and concept artifacts only. No application or firmware source changed, no device flashed, and no reviewed baseline overwritten. Existing user working-tree changes were left intact.
