# Changelog

## v1.6.1 (2026-08-18)

### Fixed

- **Smithing improvements and player enchantments now pass through
  reliably** (2game.info report). Three holes, one spot - the capture that
  snapshots an item's instance data:
  - The tempering improvement was never captured: a Legendary iron cuirass
    counted only its base 25 armor on the token. Capture now measures the
    improvement with the engine's own item-card arithmetic (so the token
    shows exactly what the piece showed worn directly - the bonus is NOT a
    flat "x health" scale, and GMST-retuning mods are respected), and every
    armor sum (worn token, published costumes, the "Item data" readouts)
    carries it.
  - A player-made enchantment captured while the costume's stat ability was
    already built (always the case for persist entries, a timing race in the
    MCM flow) silently sat out until the next game load. The capture now
    rebuilds the ability itself, so the order the UI calls things in can no
    longer matter.
  - **Conditional enchantments keep their conditions.** The synthesized
    "Costume Stats" ability used to carry only effect magnitudes, so a mod
    enchant like "+30 stamina while sneaking" became always-on. It now
    copies the live enchantment's full effect data - conditions and
    duration - whenever your original item is in CEF custody (base-enchanted
    gear always; player-enchanted gear while the hidden store holds the
    original). Published costumes keep the frozen snapshot by design.
  - **Items you captured on older versions heal automatically**: the hidden
    store still holds your original item with its tempering and enchantment
    intact, so each save load recovers whatever snapshot is missing - no
    need to re-capture anything.
- **Costumes shown while you are invisible now go invisible too.** Skyrim's
  invisibility (and any effect shader) only treats the geometry that exists
  when the effect starts, so a CEF costume added or re-shown mid-effect kept
  floating in plain sight. CEF now shadows whatever the engine has written
  onto your own equipped body and mirrors it onto the meshes it injected -
  for boxes, persist entries, substitute bodies, first and third person, and
  NPCs alike. It follows the effect ending just as faithfully, and a shape
  whose author gave it refraction of its own keeps it.

## v1.6.0 (2026-08-05)

### Added

- **NPC support is out of beta.** Dress NPCs two ways, both driven from the
  new SMF "NPC" page:
  - **Publish**: freeze a box into a wearable costume token and hand it to
    any NPC through normal gameplay (gift menus, follower managers). CEF
    tracks wearers and re-applies across saves; recall/unpublish any time.
  - **NPC persist**: assign costumes to a specific NPC (crosshair-targeted).
    Per-NPC content checklists let you pick items from the shared Persist
    catalog **or straight from your inventory** (reference-only - the item
    stays in your bags), edit an assignment in place, and re-equip the
    carrier with a Refresh button until physics converge.
  - Console: `cef pub ...` / `cef npcpersist ...` for scripting/testing.
  - Requires the separate **CostumeFW_NPC.esp add-on package** (ships
    alongside this release; ESL-flagged, Skyrim.esm-only). Without it, CEF
    runs exactly as before and the NPC page shows an install notice.
- **"Item data" switches** (Nexus request): every captured item gets
  per-item ON/OFF switches for its **enchantments, weight, and armor
  rating** under a collapsible "Item data" section - drop an outfit's
  enchantment and keep the look. Defaults ON (nothing changes until you
  touch them), fully reversible, and the readout shows what each switch is
  worth. Persist entries show the enchantment switch only (weight/armor
  never applied there to begin with).
- **Box renaming** (Nexus request): rename any box; the name follows into
  the token in your inventory, so it reads as the outfit it holds instead
  of a slot number.
- **CJK-safe menu text**: item/NPC names in legacy ANSI-codepage plugins
  (Japanese cp932, Chinese cp936, ...) no longer render as "????" - CEF now
  guarantees UTF-8 at every string it feeds the menu. NOTE: glyphs
  themselves come from SKSE Menu Framework's own font - if your language
  still shows "?", install a font covering it into SMF's fonts folder and
  enable the language in SKSEMenuFramework.ini (enable ONLY the language
  you play in: all CJK ranges at once can exceed the GPU texture limit and
  crash on startup).
- `cef invisdiag`: diagnostic dump comparing engine-equipped gear and
  CEF-displayed pieces under invisibility (verified: invisibility already
  propagates to CEF costumes - kept as a permanent triage tool).

### Fixed

- The NPC addon esp now carries the same 23-race armor-addon list as the
  v1.5.2 core fix - publish/persist carriers were silently physics-dead on
  Elder and ArmorRace-proxy custom races (custom-race followers!).
- NPC persist refused every standard catalog capture (it only accepted
  raw armor-addon ids) - completely silent failure.
- The NPC page Refresh button was a silent no-op (the unequip+equip pair
  coalesced inside one frame); it now cycles across frames, and npc-persist
  gained its own Refresh (there was NO manual physics-convergence driver).
- The player's hide-when-worn rules no longer apply to NPC persist
  costumes - an NPC's own AI-equipped gear (e.g. a follower whose default
  outfit is its own wig) could silently hide CEF content.
- The 8-NPC display budget no longer counts unloaded/dead actors - the 9th
  NPC went silently undressed with zero dressed NPCs on screen.
- Ability spells: no longer re-granted by cell re-attach while the master
  switch is off, and no longer touched at all if the NPC addon esp was
  removed mid-save.
- "Prepare for uninstall" now also recalls published costumes and strips
  every npc-persist assignment - NPCs no longer stay dressed into a save
  of a mod you are deleting.
- `cef npcpersist` could never see the console-selected NPC on 1.6.1170
  (a CommonLib raw-offset miss); it now uses the engine-passed reference.
- Two render-thread races (the NPC page and the Diagnostics readout
  deep-copying live state while the game mutates it) - both now snapshot
  on the main thread.

### Notes

- **v1.4.0-beta.1 testers**: this release supersedes the beta - update
  core AND the NPC addon (the addon esp changed; FormIDs are unchanged, so
  existing saves and assignments are safe).
- VR: the VR patch ships as before; the NPC page needs SMF's VR helper or
  the console fallbacks.

## v1.5.2 (2026-08-02)

### Changed

- **The documented `CostumeExpansionFW.ini` now ships in the package**
  (`SKSE\Plugins\CostumeExpansionFW.ini`) instead of being a
  download-on-request extra. Note for users who edited theirs (e.g.
  `bDebugMode=1`): reinstalling/updating resets the ini to defaults —
  re-apply your edits after updating.
- **This release updates CostumeFW.esp** (the race-coverage fix below), so
  install the full package — do not overwrite only the DLL. The esp change
  adds race entries to existing armor addons only: no new records, no
  FormID changes, safe to update mid-save.

### Fixed

- **Boxes never got SMP physics on custom races whose ArmorRace points at a
  vanilla race (BD Ungulates and similar) — fixed.** Every CEF token/carrier
  armor addon shipped as Race=DefaultRace with an empty additional-race list,
  so on such races the engine silently refused to render the invisible
  carrier: the token equipped, nothing looked wrong, and every costume in the
  box stayed permanently stiff while persist (a different, unaffected path)
  worked. Confirmed live on a BD Ungulates deer character — carrier file held,
  token re-equipped repeatedly, `cef headdiag` showed zero Armor merge groups.
  All 31 armor addons now carry the community-standard 23-race list
  (playables, their vampire variants, Elder, Manakin) — the same list every
  outfit mod ships — applied by the new `tools/esprace` (idempotent, backs up
  the esp). If a race still isn't covered (an exotic ArmorRace), CEF now says
  so out loud — a log line naming the race and an on-screen notice — instead
  of failing silently.
  **Update note: this changes CostumeFW.esp — v1.5.2 must ship the esp, not
  a DLL-only package.**

- **Frame containment: the crash right after a persist add is now defused the
  moment it forms, not 2.5 seconds later.** A reporter's v1.5.1 crash log
  (2026-07-31) finally measured the race: the costume node CEF attached was
  intact when attached, another mod overwrote its child-array field within the
  same second, and the engine's own per-frame scene pass read the poisoned
  field before CEF's 2.5-second background check ever got a turn. CEF now
  verifies every node it has attached at the START of every frame, just before
  the engine processes the player: if one was overwritten, it is repaired,
  detached, logged (with a full hex dump of the damage for the investigation)
  and re-injected - instead of the game crashing. An item that keeps getting
  damaged is parked for the session after 3 strikes, with an on-screen notice,
  so a hostile neighbor can't turn the repair into a loop. SE/AE only (VR keeps
  the background check); escape hatch: `[Diagnostics] bFrameContainment=0`.
  The stomped values observed so far are all tiny bone-index-like integers
  (0x1, 0x2, uint16 runs) - the new baseline comparison also catches aligned
  variants a value check alone would pass.

- **`persist-add[done]` log line no longer blames the wrong window.** Both
  post-add crashes on record happened BEFORE the carrier-sync / head-rebuild
  tail started (no `auto-sync` / `persist head` lines followed); the line now
  names the actual window (the engine's first scene pass after the 3D change).

## v1.5.1

### Fixed

- **The crash when adding to Persist.** Six crash logs (v1.3.1 and v1.5.0) all
  land on the same operation: the sweep at the end of `Reconcile()` that walks
  the player's skeleton looking for CEF's own nodes. Three fault inside the
  engine's `NiNode::GetObjectByName` while indexing the child array of a
  **CEF-injected holder node** — an array reporting a size with no usable
  buffer behind it. The other three fault just after, because that same walk ran
  off the end and returned a value that is not a node at all, which CEF then
  used as one.

  **CEF no longer searches your skeleton for its own nodes at all.** Everything
  it attaches, it now remembers: a reference to the node it created and to the
  node it hung it on, and detaching goes through that pair. Nothing walks the
  scene graph, nothing looks a node up by name, and nothing reads a node's
  parent pointer — so none of it can be handed something that is not what it
  expects.

  This took two attempts, and the first one made things worse for the reporter,
  so the history is worth recording. Replacing the by-name lookup with a
  hand-written walk of the child lists moved the crash instead of removing it,
  and spread it to **New Game**: the walk died on the children of
  `NPC Root [Root]`. That node is a `BSFlattenedBoneTree`, and Skyrim gives it
  its **own** `GetObjectByName`, different from the one every other node uses —
  the engine does not find bones there by walking children, so what those
  entries hold is nobody's contract. Walking it was never valid; it was merely
  survivable on some skeletons, which is why it never reproduced in testing
  here. Not searching at all removes the entire class.

  This is also why it looked random and why it happened through **both** the MCM
  and the SMF UI, on **every** version people tried back to v1.2.1: that step
  ran for every persist operation — adding an item, removing one, or just
  toggling "Active on this save".

- **Long lists in the SMF UI are reachable again.** Every unbounded list —
  the **Persist** catalog, the **Boxes** list, **Presets**, **Blocked** and
  **Diagnostics** — now lives in its own scrollable region instead of running
  off the bottom of the page. Reported on Nexus (2026-07-27, recorded against
  v1.3.0): a persist catalog that outgrew the page height had no way to reach
  its newest entries, so the only way to get at the latest capture was to
  **delete older ones**. The page's own controls (pickers, filters, the
  Blocked **Add** row, **Remove all persist**) now sit outside the scrolling
  region, so a long list can no longer push them off-screen either.
- **Persist catalog filter.** A name filter over the catalog, with an
  `n of N shown` count — scrolling makes a long catalog reachable, filtering
  makes it navigable.

- **A captured item could come back as the wrong piece of armour.** When an
  outfit record carries more than one addon in its armature — a helmet plus its
  hair-hiding piece, a costume plus a bundled base — CFW took the first one that
  matched your race and never checked *which body part it draws*. So a captured
  item could inject a sibling addon's mesh instead of its own, which is how a
  pair of horns ends up rendering as a plain hide helmet while the helm from the
  same mod captures correctly. CFW now prefers an addon that actually covers the
  slots the captured item occupies, with race as the tiebreaker, and logs which
  addon it picked and why whenever there was more than one candidate.

  The skin path was fixed this way in July 2026, after the same bug served up a
  pair of hands instead of a body; the content path never got the same
  treatment. Reported on Nexus 2026-07-27 ("the Helms would Persist but the
  Horns would turn into Hide helmets") — **not confirmed against that user's
  setup yet**, but it is the defect their description points at.

- **CFW's stored state is no longer read and written from two threads at once.**
  The injection registry carried a "main thread only" note, but the MCM reads it
  from the Papyrus VM thread and the SMF page reads it every frame from the
  render thread, while CFW's own background tasks were adding to it, removing
  from it and rewriting its entries. Box and persist definitions were worse:
  they are written *synchronously* on whichever thread the UI ran on, because
  the MCM needs the accept-or-refuse answer before it moves your item. Nothing
  guarded any of it. Every store function now takes one shared lock.

  This was found while investigating the persist crash and is **not** its cause —
  that crash happens entirely on the main thread. It is a real defect on its own
  and is fixed on its own merits.

- **A costume that can never get physics no longer retries forever.** When a
  content's custom bones fall back to static, CFW schedules a re-injection in
  case the physics carrier was still attaching — but a content that nifcarrier
  *excluded* from the carrier (no inline HDT xml: not SMP, or driven by
  defaultBBPs, which it cannot detect) can never bind, and every reconcile
  re-armed the retry budget. Measured in-game: 35 retry rounds over two and a
  half minutes, growing to seven items, each one a full detach, NIF load, clone,
  rebind and reattach — constant scene-graph churn for work that cannot succeed.
  Such a content is now parked once diagnosed, and re-armed automatically the
  moment it binds physics again (a carrier rebuild or a 3D rebuild).
- **The "0 of N bones bound" diagnostic no longer sends you the wrong way.** It
  told you to check the carrier file and re-equip the token. The most common
  cause is that the content was skipped when the carrier was built — which
  `CEF_sync.log` states plainly (`skipped for the carrier`), and which
  re-equipping cannot fix. The message now points there first.

- **The newest persist item could stay without physics forever.** Reported as
  "the newest entry disappears", and reproduced here to the letter: the item is
  there, it just hangs dead still, and toggling *Active on this save*, flipping
  the master switch, even save-and-reload never wakes it — while adding *yet
  another* SMP item sometimes does. The record-level verification found every
  stage healthy — manifest entry present, bones baked into the carrier, head
  part repointed, FSMP merge completed — and still `0 of 237 bound`. The cause
  is a race: injection binds bones a fraction of a second **before** FSMP
  finishes merging the freshly rebuilt carrier, every bone falls back to
  static, the four automatic retries burn out in ~4 seconds, and then nothing
  ever tries again — the dead-bind watchdog only watches bonds that *existed*,
  and re-injection skips an intact attachment. Every toggle re-ran the same
  losing race; adding another item rebuilt the carrier and sometimes won it.
  That is the whole "roulette".

  CFW now **re-arms the retries by itself** at the moments new carrier bones
  can actually appear — a few seconds after a head rebuild, and after a load
  settles. A lost race heals in seconds instead of never; an item whose carrier
  genuinely has no bones just re-parks after its retries, so this cannot loop.
  The `carrier diagnostic` message now explains the race first, since it is the
  common, self-healing case.

- **Costume carriers carried hundreds of phantom bones.** An outfit NIF built
  with BodySlide can ship its whole reference skeleton and sibling-outfit bone
  tree; one persist **earring** was carrying **619** nodes that neither its
  physics XML nor its skins ever referenced, and the carrier build unioned all
  of them. The persist carrier weighed 1330 bones; FSMP had to merge every one
  of them onto the actor, which is real per-frame and rebuild-time cost — and
  the longer merge widened the very race above. Carriers now bake **only what
  the physics XML and the skins actually reference, plus ancestors** —
  everything else is dropped, and the drop is logged per content. The same
  persist set went from 1330 bones to 207, and its rebuild from **355 seconds
  to 11**. Live-skeleton classification also stopped guessing from name
  prefixes: the builder reads the real skeleton NIFs (male/female, normal/
  beast) once and matches against what is actually there.

- **A damaged costume node is contained instead of crashing the game.** The
  crash logs that drove this release showed CFW's holder nodes with child
  arrays whose buffer pointer held values like `0x0001000500050005` — not an
  address at all, but a run of 16-bit integers, bone-index-shaped, written over
  a pointer by something still unidentified. Every value observed fails an
  8-byte alignment test, so CFW now validates **pointer values** (alignment,
  canonical range, null-page floor) everywhere it reads its own attachments:
  before detaching, before walking, and on every child slot it touches. A node
  that fails is logged loudly with the raw value (writer evidence), repaired to
  a safe empty state, quarantined out of the scene, and its item re-injects —
  a log line instead of a CTD, whoever the writer turns out to be. A synthetic
  probe (`cef slottest`) plants the observed poison value and verifies the
  whole containment path in seconds, no mods needed.

- **The Diagnostics bind counters no longer zero out.** "CFW content needs /
  Heaviest shape" reset to zeros on every routine re-apply pass that had
  nothing new to bind; the page now keeps the last real numbers.

### Changed

- **On the first launch of this version, every carrier is rebuilt once.** The
  carrier content fingerprint changed (the phantom-bone shed above), so the
  first sync rebuilds the whole set — expect the rebuild notification and, on
  a large setup, a minute or two, exactly once. Unchanged syncs stay instant.

- **Injected costumes allocate their node once instead of once per shape.** The
  holder node's child array was created empty and grown by the engine, which
  reallocates and copies on every single attach — a 20-shape costume did 20
  reallocations. It is now sized from the shape count up front. (Measured with
  the new `cef arraytest`, which was built to test something else entirely.)

### Added

- **Carrier rebuilds show a heartbeat.** A rebuild that takes a while was
  indistinguishable from a hang — the watchdog even used to declare a healthy
  2-minute run "wedged". Now: nothing at all for the common instant pass, a
  screen notification at 2 seconds — deliberately *before* the "is it stuck?"
  doubt sets in — then at 7, 17 and 47 seconds and every 30 after, each with
  the stage, step and elapsed time ("Costume carriers: rebuilding (box 57,
  step 8/9, 17s)..."). The Diagnostics page shows the same live
  (`carrier auto-sync: RUNNING (persist, step 9/9, 45s)`), and a rebuild that
  actually rewrote something announces `rebuilt N item(s)` once, on screen and
  in the log.

- **An always-on flight recorder, sized for the one log that matters.** A
  roulette crash cannot be asked to reproduce, so the evidence minimum is
  always on and logs on change only: an environment census at startup (which
  physics/skeleton DLLs are present, with size and timestamp), a registry roll
  call after loading (resolve failures named instead of silent), a settings
  mirror written next to the log (`CEF_settings.mirror.json` — one folder now
  holds everything a report needs), a save-identity line, and the corruption
  transitions above. The verbose tier — per-tick health polls, memory lines,
  equip traces — stays behind `bDebugMode=1` (`CostumeExpansionFW.ini`,
  `[Diagnostics]`, or the session-only checkbox on the SMF Diagnostics page)
  and changes logging only, never behavior.

- **A "Physics bones" section on the Diagnostics page** (both the MCM and the
  SMF UI), because "my costume lost its physics" turned out to be a race nothing
  surfaced. It covers two separate things that are easy to confuse:

  **Heaviest shape — N / 80.** Skyrim SE skins on the GPU and passes a shape's
  bones in a ~3840-byte DX11 constant buffer, so **80 bones per shape, per draw**;
  a shape over that crashes when it is copied into the buffer, which is exactly
  what **Bone Limit Extender** (Nexus 177636) lifts. The page reports your
  heaviest injected shape against that 80, names the costume it belongs to, and
  warns loudly if you are over it without the extender installed.

  **FSMP merge — asked vs granted.** How many custom bones your shown content
  needs, how many got physics, how many fell back to static, and how many bones
  FSMP merged on your character — split into CFW's own and everyone else's. If
  none of CFW's carrier bones were merged it says so: FSMP takes a carrier whole
  or not at all, so something else on the actor won. Measured while chasing this:
  with one popular body-collision mod enabled, FSMP merged 102 bones on the test
  character and **not one was CFW's**; disabling it took the same character to
  3453 CFW carrier bones. This axis has no citable ceiling — it depends on your
  whole load order — so it reports the gap rather than inventing a denominator.

### Diagnostics (for the persist-CTD investigation)

- **Stage markers for adding to Persist.** The path now logs
  `persist-add[catalog] → [register] → [reconcile] → [ability] → [manifest] →
  [done]` at info level. The log is flushed line by line, so after a crash the
  **last marker names the stage that was running** — which separates a crash in
  the UI-thread half from one in the main-thread half, and both from the carrier
  rebuild tail that follows `[done]`. `capture[enchant]` marks the inventory read,
  and `persist head: rebuild requested (...)` (was debug-only) marks the entry to
  the head-rebuild chain. Documented in `LOG_REFERENCE_EN/JA.md`.
- **`cef arraytest` / `cef nodediag` / `cef slottest` console commands.**
  `arraytest` builds holder nodes the way injection does and reports the child
  array after every attach — it needs no mods and no costume, and it is what
  cleared the empty-array suspect above. `nodediag` scans both player skeletons
  and reports the child array of every CEF node, plus any node that fails the
  sanity guard. `slottest` sweeps the pointer-validator's boundary cases and
  plants the actually-observed corruption value into a synthetic node to prove
  the guard rejects it, the walk skips it, and teardown stays safe.
- **`bPersistHeadRebuild` troubleshooting switch** (`CostumeExpansionFW.ini`,
  `[Diagnostics]`, default `1`). Set to `0` to skip the facegen head rebuild CEF
  fires a few seconds after a persist change. Not a fix and not a supported mode
  — persist costumes get no SMP physics while it is off — but it turns "does the
  head rebuild cause this crash?" into a test a reporter can actually run.

> Full analysis, including the disassembly that identified the crash site and the
> two hypotheses the logs ruled **out**, is in
> `BUGREPORT_2026-07-27_persist_ctd.md`. The stage markers and the
> `bPersistHeadRebuild` switch below were built to chase this before the logs
> arrived; they are kept because they are useful instrumentation, not because the
> crash still needs them.

## v1.5.0 (2026-07-26)

> Version numbering note: this is the successor to **v1.3.1**. The number jumps
> to 1.5.0 because **v1.4.0-beta** (NPC support, GitHub pre-release only) already
> occupies 1.4.x — the released line skips past it rather than colliding with it.
> Everything below was developed as "v1.3.2" and shipped under this number; no
> v1.3.2 was ever released.

### Fixed (in-game test run, 2026-07-26)

- **Blocked page: the Add button was unreachable.** The kind selector, the value
  field and **Add** were chained on one line with no width hints, so a narrow SMF
  window pushed **Add** off-screen — adding a deny-list entry was effectively
  impossible (only removal worked). Widths are now pinned, **Enter** commits the
  entry, and committing an empty field says so instead of doing nothing silently.
- **Capture refusals are no longer silent in the SMF picker.** The reason was
  written to a status line at the *top* of the Boxes page while the picker sits
  deep inside a box's tree node — off-screen exactly when you need it. Refusals
  now also raise a notification, matching the MCM's behaviour.
- **"Reload settings from disk" left the carrier manifest stale.** The manifest is
  now re-emitted at the end of every settings reload, not only inside the
  deny-list transaction, so a hand-edited `CEF_settings.json` can no longer leave
  the manifest describing the previous content set. (Still exactly one manifest
  write per operation.)
- **A permanently static costume now says why.** When an item's custom bones fall
  back to the static ancestor remap and stay there after the rebind retries are
  spent, CFW logs the **carrier file it is actually using** plus how many of the
  content's custom bones bound to a physics node (`0 of 25`). Previously "the
  carrier is still attaching" and "the carrier attached but carries none of these
  bones" were indistinguishable and CFW simply went quiet — which is how a
  masked carrier file (a higher-priority mod overriding `meshes\CostumeFW`) could
  silently freeze every SMP costume.
- **Deliberate deny-list refusals log at warn, not error.** `has no admitted ARMA`
  now only reaches error level when the mesh genuinely cannot be resolved; a
  refusal by policy is a decision and reads as one.

### Added
- **Capture blacklist** (MARA compatibility - `MARA_COMPAT_PLAN.md`,
  `MARA_CRASH_CLASS_AUDIT.md`). Origin report (CEF Nexus posts, 2026-07-22):
  with MARA installed, picking its runtime "CORE Carrier" item in
  `+ Add worn item` crashed instantly. The capture pickers now skip, and the
  capture gate now refuses, three layers of items:
  - **L1 structural (hard)** — runtime-created (FF) and no-defining-file
    forms (their data is the crash surface, and the colon-id persistence
    model could never restore them anyway; not user-liftable) and
    non-playable armors (hidden by the vanilla UI; a raw inventory picker
    must hide them too; switchable). Structural checks run **inside the
    `GetInventory` filter**, so a blocked form's `InventoryEntryData` is
    never even copied; `CaptureEnchant` now touches only the captured
    form's entry.
  - **L2 deny-list** — shipped defaults (`CORE Carrier` by name, `MARA` by
    plugin prefix) + user entries (name / plugin / colon-id) managed on the
    new SMF **Blocked** page or in `CEF_settings.json` (`captureBlacklist`).
  - **L3 opt-out keyword** — any armor carrying `CEF_NoCapture` is excluded;
    mod authors and users can distribute it via the shipped
    `CostumeFW_NoCapture_KID.ini` template (KID auto-creates the keyword).
  Every capture entrance funnels through the same gate (MCM, SMF, presets,
  the `AddBox`/`AddPersist` Papyrus natives), and a **hard admission check at
  the registration boundary** (`RegisterBoxById`/`RegisterArmaById`/
  `InjectArmaById`) covers the remaining routes — settings load, co-save
  restore, `RegisterPersist`/`DefineBox`, console `cef box`. A refused id is
  KEPT in config/co-save (quarantined: not registered, one log line).
- The blacklist policy is published as an **immutable atomic snapshot**
  (readers never see a mutating list), and its pure layers are covered by a
  new host-side unit-test target (`policy_tests`, dev-only).
- Startup logs `compat: MARA.dll detected` when MARA is present, and — the
  one behavior that does branch on it — **capturing WORN jewelry is refused
  while MARA runs** (unequip first, or capture from inventory): stripping a
  worn ring/amulet out from under MARA crashes inside MARA's own bookkeeping
  (field-proven during release testing; MARA cannot reliably survive even a
  regular unequip). Unworn jewelry, non-jewelry, already-captured contents
  and MARA-less setups are unaffected.
- All of the above hardened per an adversarial review and re-review
  (`MARA_GUARD_ADVERSARIAL_REVIEW.md` / `..._REREVIEW.md` — all findings
  accepted; responses in `MARA_GUARD_IMPL.md` §R/§R2). Re-review round adds:
  the **selected ARMA** itself passes the hard/plugin layers inside
  `ResolveArmaModels` (a permitted ARMO cannot smuggle a denied or runtime
  ARMA); **full quarantine** — stats/keywords/abilities/carrier manifest/UI
  summaries all skip blocked-but-configured contents, and a blacklist edit
  immediately detaches newly-blocked actives (and re-admits freed ones)
  without touching the stored config; the raw `cef inject` console path and
  the public `CaptureEnchant` native now pass the same admission; the policy
  unit tests are CTest-registered.

### Fixed
- **The picker CTD mechanism itself.** `TESForm::GetLocalFormID()`
  dereferences `GetFile(0)` unchecked; v1.3.1's pickers called it (via
  `MakeColonId`) on every foreign armor, so a runtime form = instant null
  deref. `MakeColonId` is now no-file-safe, and runtime forms never reach it.
- **Runtime (FF) form ids no longer truncate.** `MakeColonId` /
  `CanonicalizeColonId` wrote 8-digit local ids into `char[8]` buffers
  (`%06X` is a *minimum* width), producing corrupt `"FF00080:"`-style ids.
  Formatting unified in the new pure `CapturePolicy` module; normal 6-digit
  ids are byte-identical.

## v1.3.1 (2026-07-17)

### Fixed
- **Persist head-carrier "teeth drop" watchdog now actually works** — 3-tier
  mouth detection (top-level part / face extra part / race chargen default)
  and, when a plain rebuild re-drops it, promotion of the mouth to an explicit
  head part (save-persistent). See `NEXUS_CHANGELOG_v1.3.1.txt`.

### Added
- **Skyrim VR runtime support (community beta).** The DLL has always been
  built multi-runtime (CommonLibSSE-NG, SE/AE/VR); this release removes the
  two lookups that were fatal on VR and adds diagnostics:
  - deleted the dead `NiSkinInstance::UpdateBoneMatrices` relocation
    (`src/Offsets.h`, SE id 75655 — never called, but resolved at DLL load,
    and the id is absent from the VR address library = instant VR crash);
  - the `cef` console hook now uses the raw VR call-site offset
    (`0x90E1F0 + 0xE2`, matching ConsoleUtil-Extended's shipping VR branch
    and the VR address library auto-diff) instead of SE id 52065, which the
    VR database does not map;
  - startup logs the detected runtime (`runtime: Skyrim SE/AE/VR x.y.z`) and
    the skee BodyMorph interface version (RaceMenu VR 0.4.14 exports v4).
  VR requires SKSEVR, VR Address Library, Skyrim VR ESL Support, SkyUI VR and
  RaceMenu VR; ships as a separate small **VR Patch** archive
  (`tools/package_vr_patch.ps1`, dll + `README_VR.txt` only — esp/meshes/
  scripts stay runtime-shared in the main package).

## v1.3.0 (2026-07-13)

- SKSE Menu Framework UI (SMF becomes the primary UI), RMSS base-skin fix,
  vanilla-slot token fold. Recorded retroactively - see
  `NEXUS_CHANGELOG_v1.3.0.txt` for the shipped notes.

## v1.2.1 (2026-07-07)

### Fixed
- **Removing an ACTIVE persist entry can no longer double-return.** The
  catalog remove now also deactivates the entry (return pairs with
  deactivation), and the "Active but not in catalog" [deactivate] row guards
  against stale page snapshots - previously the removed-but-still-active
  entry resurfaced there and its deactivate returned a second copy (review
  round 4).
- **Co-save restore honors the forced-gender NIF.** The token-less persist
  restore resolved models with the raw player sex, so a forced Male/Female
  NIF reverted on every load; it now uses the same effective sex as live
  registration.
- **A persist catalog remove keeps the per-content settings.** Hide rules,
  forced gender, body-morph opt-in and the captured enchant snapshot used to
  be erased with the catalog entry - but another save may keep the entry
  uncataloged-active and still display it, and lost its look/enchant on the
  next load. The maps now survive (a re-capture re-snapshots the enchant).
- **Preset validation checks displayability and drops duplicates.** Entries
  are validated with the same gate captures use (usable ARMO/ARMA model, not
  mere form existence), and duplicate ids no longer double the stats/enchant
  aggregation.
- **"Reload settings from disk" keeps uncataloged actives.** The reload
  re-registers every persist entry active on this save - including entries
  another character removed from the shared catalog (a supported state the
  co-save restore also preserves). They were silently dropped with no item
  return (review round 3).
- **Assigning a preset returns the items it replaces.** Box and persist
  preset assignment now returns the physical items of every content the
  preset drops (old minus new; overlapping entries keep their stored
  custody) - previously the old captured items were stranded in the hidden
  store with no UI path back.
- **Presets respect the cross-holder guard.** A preset containing an id
  already captured in another box (or persist) is rejected with the reason
  logged, matching the manual-capture guard - it used to silently steal the
  display and share per-content settings.
- **Removing an INACTIVE persist entry no longer fabricates an item.** The
  single-row "Remove from catalog" returns store-only for entries this save
  never displayed (the capturing character keeps the original); the new-copy
  fallback stays for active entries, per the 2026-07-06 decision.
- **The carrier manifest resolves content NIFs by effective sex** (player sex
  + per-content forced-gender override), matching the injection - it was
  hardwired female-first, so a forced-Male item or a male PC could get a
  carrier built from the other sex's NIF with a mismatched bone set. A Body
  menu change now also rebuilds the carrier when the shown NIF switches.
- **The Stats row shows the captured enchant snapshot** (what the synthesized
  ability actually applies) instead of only the base enchantment - a captured
  player enchant used to look unapplied.
- **Multi-content boxes: per-content bone/shape namespace isolation.** Outfit
  series that reuse custom bone names across items (e.g. COCO's shared
  cocoa01... chains - two COCO skirts share 104 custom bone names) collapsed
  onto a single node under ONE parent in the merged carrier: the first content
  won, so another item's chains could end up hanging from a veil's NPC Head
  root ("the outfit bunches up behind the head"), and the losing items'
  physics systems never built (static, no sway). Each content's custom bones
  and collision shapes are now renamed into a per-content namespace
  (`C<hash>_<name>`) in the carrier NIF and its physics XML, and
  the bind side resolves the prefixed name first (plain names still work for
  single-content carriers). The carrier hash salt was bumped, so every
  existing carrier rebuilds once on the next sync.
- **Carrier builds no longer skip non-SMP contents silently.** Items with no
  inline HDT physics xml in the NIF - including items whose physics is wired
  through FSMP's global defaultBBPs.xml, which CEF cannot detect yet - now log
  why they will inject without physics.
- **Capture is guarded across holders:** capturing an item that another box
  (or persist) already holds is rejected with "already captured in ..." —
  previously the second registration silently stole the display, and deleting
  either holder wiped the shared per-content settings (review P1-1).
- **A failed duplicate capture no longer overwrites settings:** the gender-mode
  pick and the enchant snapshot are written only after the capture actually
  succeeded (review P1-3).
- **Bulk persist returns are active-set only:** "Remove all persist" and
  "Prepare for uninstall" return items only for entries active on THIS save —
  catalog entries another character captured are no longer fabricated as fresh
  copies by bulk flows (review P1-4).
- **"Reload settings from disk" actually reloads:** the button re-reads
  CEF_settings.json and re-applies it live (boxes, catalog, hide/gender/morph/
  enchants; this save's persist actives survive). It previously only rewrote
  the current in-memory state (review P2-5).
- **The Armor type menu opens on the box's current value** instead of always
  box 0's (option-index mixup).
- **Capture pre-validates the mesh:** when a content's model cannot be
  resolved (no usable ARMA/3P model), the capture is refused up front with a
  message - previously the queued registration failed AFTER the item had
  already been moved into the hidden store ("success" notice, item gone,
  nothing shows) (review item 2).
- **Race-matched armor addon selection:** both the injection and the carrier
  manifest now pick the ARMO's addon matching the player's race (exact race,
  then additionalRaces, then the first addon) instead of blindly the first
  one - race-/sex-specific addon lists resolved to the wrong mesh before
  (review item 6).
- **The "has attached scripts" capture warning fires only for real scripts.**
  Passing a form through the MCM's own Papyrus flow binds a plain VM wrapper
  object ("Armor"/"Form") to the form's handle, which the check misread as an
  attached script - so EVERY capture warned. Native wrapper classes are now
  filtered out (the real script's class name is logged when one is found), and
  the message is phrased as a heads-up ("appears to have ... may not run")
  rather than a verdict.

### Changed
- **Capture menus list each item once.** The forced-gender NIF pick moved from
  the tripled capture rows to a per-content **"Body"** menu on box/persist
  rows (changeable any time now, not only at capture; new captures follow the
  player's gender). An **"Inventory filter"** input narrows the "+ Add from
  inventory" list by name (case-insensitive substring), and the native list
  cap rose from 40 to 120 entries.
- **One ESL-flagged plugin.** `CostumeFW_Boxes.esp` and
  `CostumeFW_Boxes_FSMPCarrier_001.esp` were merged into a single espfe plugin,
  **`CostumeFW.esp`** (52 records, no load-order slot consumed; built by
  `tools/espmerge`). Base-plugin FormIDs are unchanged; the carrier-patch's own
  records were renumbered +0x100 (persist head-part pool `0x909`-`0x911`,
  slot-31 wig token `0x913`). Box/persist definitions, ability assignments and
  per-content settings stored in `CEF_settings.json` migrate automatically on
  first load (ids are healed to the new plugin and written back). The three
  approach-C PoC head parts were dropped instead of carried over. A `SEQ` file
  now ships so the MCM quest starts when the plugin joins an existing save.
  The LoreBox KID ini was renamed to `CostumeFW_KID.ini` and now also covers
  the slot-31 wig token.
- **Known limitation (documented):** multiple individuals of the same base
  item cannot be captured separately - catalog/registry keys are base-form
  ids, so the second copy is rejected as a duplicate. The captured copy's
  player-enchant effects are snapshotted and applied; the item itself is
  preserved in the hidden store.
- **Upgrading mid-save:** the old plugins vanish from the load order, so box
  tokens disappear from the inventory once (CEF re-distributes them) and
  captured originals held by the old hidden container are returned as fresh
  copies on demand. A save that ever used persist on a pre-merge DEV build
  should do one TRANSITION load (new plugin enabled, old plugins still
  enabled): a one-shot sweep deregisters the old plugins' head parts
  automatically; save, then disable the old plugins.

## v1.2.0 (2026-07-07)

### Changed
- **The carrier builder now runs inside the plugin — no external tool.** The FSMP
  physics-carrier build (the former external C# `nifcarrier`) was ported to C++
  ([nifly](https://github.com/ousnius/nifly)) and statically linked into the DLL.
  Everything that setup used to need is gone: no .NET runtime, no
  `sync_carriers.cmd` path editing, no `CEF_sync_command.txt` — install the mod
  and auto-sync just works. Box/persist changes rebuild carriers in-process (2 s
  debounce, background thread), and content paths resolve through the same VFS
  the game sees — so any mod manager works, with no per-mod path maintenance.
  Power users: a present `CEF_sync_command.txt` still hands the build to the
  external tool (compat mode, kept for one release). This also retires the
  "plugin executes a command line read from a text file" surface by default.
- **Veil-class contents keep their collision shapes in merged carriers.** The
  C# tool's NIF library corrupted some contents' SSE vertex data during
  cross-file shape cloning, so such contents were baked bones-only (physics
  moved, collision inert). Upstream C++ nifly does not have that bug — those
  contents now carry their collision meshes. The per-content
  validate-or-bones-only safety gate remains in place.

### Fixed
- **Persist enchant effects now come from this save's ACTIVE set,** not the
  shared catalog — you no longer get the effects of persist entries another
  character cataloged but this save never activated.
- **Corrupt co-save data fails small:** record reads are bounds-checked
  (string/count caps, short-read detection), so a damaged co-save logs and
  skips instead of driving a huge allocation or a near-endless restore loop.
- **Carrier publish is failure-checked end to end:** every publish write
  (carrier, revision slot, physics XML, `carriers.json`) is verified, and the
  input hash is recorded only after a successful slot publish — a copy that
  fails (file locks, antivirus, VFS quirks) can no longer strand a box in a
  "hash says up-to-date, files say otherwise" state that never rebuilds. A
  wedged in-proc build is surfaced in MCM Diagnostics (code -2) after 120 s.

### License
- The distributed `CostumeExpansionFW.dll` binary is now **GPLv3** (it
  statically links nifly, GPL-3.0). The project's own source stays MIT. See
  `THIRD-PARTY-NOTICES.md` (component list) and the bundled GPLv3 text.

### Dependencies
- **.NET Runtime is no longer needed.** The optional `CEF-nifcarrier` download
  is legacy — only for the external-tool compat mode or development.

## v1.1.0 (2026-07-06)

### Changed
- **Body morph is now per-content OPT-IN (default OFF).** It used to be applied to
  every injected mesh; on a non-body mesh it is wasted work, and on a large one (a
  full SMP wig) a single skee vertex-diff pass allocated ~15GB that SSE Engine
  Fixes' allocator then retains. Turn it on per content in the MCM (persist page /
  box pages) or with `cef morph <id> on` — only BodySlide body-conforming meshes
  need it.
- **Persist is per-save now (shared catalog + per-save activation):** the persist
  list in `CEF_settings.json` is a catalog shared by every save; each save
  activates entries for itself (MCM toggle per row, or `cef persist on|off <id>`).
  A new character starts with nothing shown; deleting a catalog entry no longer
  strips other characters (their active items survive "uncataloged" and can be
  deactivated — with the item returned — from the MCM). Existing saves keep what
  they showed: the co-save already stored it, no migration.
- **Capture is transactional:** the worn item is moved into the holding container
  only after its registration succeeds; a duplicate capture leaves the item worn
  and says so in a modal message (a corner notification was easy to miss with the
  MCM open).
- **"Prepare for uninstall" now also disables CEF persistently** (writes
  `enabled=false`), so playing on / reloading no longer re-applies everything.
  Re-enable from MCM → Main if you change your mind.
- **Auto-sync hardening:** a timed-out nifcarrier child is terminated instead of
  being left to race the next sync, and an `.exe`-form sync command now runs
  without the `cmd /c` shell layer (`.cmd`/`.bat` wrappers still use it).
- **Carrier apply is user-driven (re-equip to apply):** when a box's content set
  changes, CEF now rebuilds the carrier and repoints the token ARMA at the new
  revision, then asks you to *re-equip the box token until the outfit sways* — it
  no longer tries to swap the token for you. Programmatic re-equips coalesce or
  stall in the equip queue, and FSMP recycles carrier ids while leaving the old
  carrier alive for a few seconds, so an auto-swap could bind the injected mesh to
  a dying carrier and leave it floating. A manual re-equip reloads cleanly once
  FSMP has settled. The old two-token flip-flop machinery has been removed.

### Fixed
- **A duplicate capture could silently swallow a physical copy** of the item into
  the holding container, with the UI still reporting success and no return path
  ever giving that copy back.
- **Skin-only physics bones dropped from merged carriers (veil didn't sway):**
  merging a multi-content box only walked the node hierarchy, so a content whose
  physics bones are referenced *only* by skin data (e.g. a Pharaoh veil's
  `PhSVeil_*`) lost those bones and went static in-game. `nifcarrier` now unions
  every named bone (skin-referenced included, parent chains preserved). When
  NiflySharp's cross-file shape clone would corrupt a specific content's SSE vertex
  data, that content is baked in bones-only rather than dropped, so the rest keep
  their collision meshes.
- **Carrier divide-by-zero CTD (Box44 class):** a box whose merged FSMP carrier NIF
  contained non-SSE geometry (`NiTriShape`) or a degenerate skin partition crashed the
  game with `EXCEPTION_INT_DIVIDE_BY_ZERO` in the vanilla skin-partition loader whenever
  the box token was worn — unstoppable from CEF config, since the engine (not CEF) loads
  a worn token's mesh. `nifcarrier` now validates skin data: `sync` excludes an
  individual crashing content, builds into a temp, runs a final validation gate on the
  assembled carrier, and only atomically publishes a passing build (keeping the previous
  good carrier + revision otherwise). New `nifcarrier validate <nif>` diagnostic. CEF's
  `ApplyCarrierOverrides` no longer repoints a token ARMA at a carrier file missing on
  disk (falls back to the ESP-default empty carrier).
- **One broken content no longer sinks a whole box:** a content that can't be
  resolved under the data roots (or whose physics XML is missing) is now *excluded*
  with a warning and the carrier is rebuilt from the rest, matching how a
  crash-prone content is already handled — instead of failing the entire box. A box
  that declared contents but resolved none keeps its previous carrier rather than
  clobbering it down to empty (guards a transient path miss). Auto-sync also
  captures nifcarrier's output to `Data\SKSE\Plugins\CEF_sync.log` (truncated each
  run) so the `[sync]`/`[merge]` decisions are visible instead of discarded.

### Added
- **Capture from inventory:** the persist page and every box page gain
  **"+ Add from inventory"** — pick any carried armor (name-sorted, first 40)
  without equipping it first, so no throwaway FSMP physics build happens just
  to capture the item. Player-enchanted items keep their enchantment (the
  snapshot now reads the carried entry too). "+ Add worn item" remains for
  reaching items past the list cap.
- **Custom-bone SMP cloth physics on persist items (head-carrier pool):** persist
  accessories with outfit-specific SMP bones (veils, dangling jewelry) now sway:
  nifcarrier builds an invisible head-part carrier pool from the active persist
  set and CEF registers/repoints it automatically — no re-equip needed, load-
  persistent, self-healing (bind watchdog, generation-aware re-bind, `cef repair`).
- **Wig support via a slot-31 box ("Costume Box 31: Hair (Wig)"):** capture an
  equipment wig into it; wearing the token masks your real hair natively (the
  standard equipment-wig mechanism) and the wig's SMP cloth + collision work as
  authored. Wigs should NOT go into Persist (the facegen path rebuilds them
  repeatedly at load and the retained allocations balloon memory).
- **MCM Diagnostics page:** master/skee/FSMP status, last carrier auto-sync
  result, per-box carrier revision (+ on-disk check), persist catalog/active
  counts + head-part registration, and churn counters.
- **Settings safety:** `CEF_settings.json` and the carrier manifest are written
  atomically (a crash mid-write can no longer truncate them), and a last-known-
  good `CEF_settings.json.bak` is restored automatically when the main file
  fails to parse — a corrupted settings file no longer silently wipes your boxes.
- **Console:** `cef recover <FormID:Plugin.esp>` (explicitly grant one copy of a
  content item), `cef persist on|off <id>` (per-save activation).
- **External hard kill-switch (crash recovery):** CEF can now be fully disabled from
  outside the game, read once at startup before any hook runs. Set `bEnabled=0` in
  `Data\SKSE\Plugins\CostumeExpansionFW.ini`, or just drop an empty
  `Data\SKSE\Plugins\CEF_DISABLE.txt` (existence forces off). When disabled, CEF
  registers no hooks/sinks, loads no boxes, and does no mesh injection or carrier/FSMP
  work — so a save a CEF crash left unloadable opens with the plugin inert. Distinct
  from the MCM master toggle, which only hides meshes while the hooks keep running.

## v1.01 (2026-07-01)

### Added
- **Hide when worn (per item):** an injected item can now be hidden while a real
  equipped item occupies chosen vanilla biped slots, and reappears automatically
  when that slot is freed. Set it per content in the MCM (e.g. `37` to hide foot
  nails under boots, or `30 31 42` to hide a wig under an auto-equipped helmet).
- **Inventory tooltips (LoreBox integration):** hovering a Costume Box token in
  your inventory shows its packed item names. Updates live as you add/remove
  contents. *Optional — requires "LoreBox - Item and Spell Tooltips" + KID.*
- **Forced-gender NIF mode:** when capturing a worn item you are asked which body
  mesh to inject — *Use player's gender / Force Male / Force Female* (a simple
  message box; no extra dependency). Lets a costume captured on one body show the
  other body's mesh.
- **Presets for the Persist class:** the always-on Persist set can now adopt a
  preset just like a box, using the same `CEFP_*.json` files. A preset can only be
  assigned to one box or to Persist at a time (shared exclusivity). Includes
  "Export as preset" for the Persist set.
- **Male body support (experimental):** injected content now resolves to the
  player character's body sex, with an automatic fallback to the other sex's mesh
  when an item ships only one. *Female PC remains the primary tested path.*

### Changed
- **MCM: one page per box.** Each box now has its own page (listed on the left),
  fixing the page option/scroll limit that was hit with many boxes. The "Boxes"
  page is now a short overview with "+ New box". Adding or deleting a box updates
  the page list after you close and reopen the MCM.
- **Presets now carry their per-content settings.** Exporting a box/Persist set as
  a preset also saves each item's hide-when-worn slots and forced-gender mode, so
  a distributed costume keeps that behavior when imported.

### Removed
- Unused leftover `costume_seed.json` (the seed system was retired in v1.0).

### Dependencies
- **Required:** SKSE64, Address Library, SkyUI, RaceMenu (skee).
- **Optional (feature add-ons):** Keyword Item Distributor (KID) + LoreBox
  (inventory tooltips).

---

## v1.0 (2026-06-27)
- Initial public release. Skin-rebind injection framework: attach skinned
  accessories (nails / piercings / costumes) outside the biped slot system to free
  real equipment slots. Persist class (always on) + box class (token-gated), body
  morph follow, keyword/stat passthrough, MCM, presets, clean uninstall.
