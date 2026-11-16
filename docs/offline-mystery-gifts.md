# Offline Mystery Gifts

On the supported offline build, open **Link Play -> Mystery Gift -> Get via
Internet** to see the local gift archive. Select a gift to read its reward and
availability, then choose **Claim gift** or press **+ / Start**. You can claim
multiple available gifts before choosing **Back** or pressing **B / Esc**.

**After leaving the picker, return to the normal game menu and press R: Save
Progress.** A claim updates the running game's rewards and native receipt
tracking. The picker does not request an automatic save. Back keeps gifts already
claimed; it does not undo them. Use the normal save before quitting so the gifts
and their one-time receipts persist.

The catalog contains 19 selections across all four reward types:

| Type | Count | Selections |
|---|---:|---|
| Mission unlocks | 3 | Project M; Shine Bright like a Gemstone; Special distortion detected! |
| Item sets | 6 | Newsletter items; Mega Shards; Poke Balls; Luxurious Poke Balls; Adventure Set; Ultra Balls |
| Pokemon | 9 | Two birthday Audino distributions; Kagawa Slowpoke; Worlds26 Greninja; Ralts with Gardevoirite; Alpha Charizard; shiny Xerneas; random-pattern Spewpa; Poke Ball Pattern Vivillon |
| Clothing | 1 | Beige Trench Coat and Pants Set, one combined outfit |

Pokemon rewards are labelled **Offline recreation**. They use authored reward
choices and the game's construction routines; missing original event attributes
use local defaults. They are not copies of original distributed event Pokemon.
Spewpa is one gift with one of 16 randomly selected evolution patterns, not 16
separate gifts. The clothing reward adds ownership without changing your current
outfit.

Mission gifts unlock the encounter and grant their listed items. Project M grants
both Mewtwonites and the mission flag; Diancie's gift grants Diancite and its flag.
Both encounters still require completion of the main story. Garchomp Z's gift
unlocks the special distortion: entering it still requires owned Mega Dimension
DLC and a completed hyperspace adventure. Defeat the encounter to earn its stone.
No gift grants DLC ownership or marks a mission complete.

Successful claims use the game's native one-time receipt tracking. Previously
received gifts are unavailable, and storage or satchel capacity can prevent a new
claim. Already-owned clothing is shown as unavailable and retains its existing
ownership because the native grant rejects duplicates. The local archive includes
historical, password and venue rewards; listing
them here does not imply that their original online distribution is still active.
It does not contact Nintendo's gift service or require a network login. DLC
purchase rewards, HOME rewards and ranked-battle progress remain separate from
this catalog.

## Supported build

The adapter currently checks the first eight bytes of the game main build ID:
`b1f12fd919eae86a` (the supported Z-A v2.0.2 build). It also requires the normal
main-plus-SDK offline session. Lumiose/online intent, including the presence of
`LUMIOSE_TEST_MODULE`, disables this local adapter. Other builds keep the original
game entry rather than calling these build-specific routines.

The repository ships authored source definitions and public numeric metadata.
It does not ship Nintendo Wonder Cards, game assets, extracted game code or
Pokemon files. Build and run with your own matching game extraction.

## Playtest diagnostics

The [isolated macOS launcher](local-playtest.md) enables the `gift` developer-log
subject alongside its existing diagnostics. It prints a unique diagnostic file
inside the private test profile's `logs/` directory. The filename retains the
legacy `-framegen.txt` suffix; look for the `gift` subject's claim and receipt
messages. Launching the runtime directly can enable the same subject with
`LARECOMP_DEVLOG=gift` and an explicit `LARECOMP_DEVLOG_FILE` path.
