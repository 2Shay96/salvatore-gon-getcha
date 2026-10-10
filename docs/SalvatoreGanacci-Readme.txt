SALVATORE GANACCI
Version 1.0 | Author: 2Shay

A dancing Salvatore finds you in the Mojave, fights you, and becomes a
collectible companion when defeated. Includes sprite animation, music,
defeat and release sounds, and the Ganacci Remote.

REQUIREMENTS
Fallout: New Vegas and FalloutNV.esm. No DLC or NVSE required.

INSTALL
Install the release ZIP with your mod manager and enable SalvatoreGanacci.esp.
For a manual install, copy SalvatoreGanacci.esp and the meshes, textures and
sound folders into Fallout New Vegas/Data, keeping the folder structure.
Enable the plugin in your launcher or mod manager, then restart the game.
Use a save made before installing the mod for a fresh start.

HOW TO PLAY
After 1-3 in-game days, Salvatore appears at a distant Mojave location and
comes after you. The encounter starts while you are outside in the Mojave;
if the timer expires indoors, it waits until you return outside. Waiting,
sleeping and fast travel advance the timer. Test mode is off by default.

Defeat him and a small dancing sprite appears where he fell. Activate it
to collect the Salvatore Ganacci weapon item at full condition. The small
sprite stays in place until collected.

Equip the item to deploy Salvatore as a companion. He follows and fights
enemies, and explores nearby with idle pauses while you stand still. Walking
away resumes following; combat interrupts exploration. Activate the companion to put him back in your inventory.
Each companion defeat reduces condition to about 66%, then 33%, then 0%.
At 0%, he is too battered to deploy. Repair the item, then equip it again.
The condition/repair loop was tested; repair-bag compatibility is untested.

ANIMATION AND AUDIO
Main and standing-idle sprites use forward/backward loops at 24 fps
(main 5.75 seconds, idle 2.75 seconds). Attacks use a separate forward
animation. He has an occasional attack quip and two
random idle clips with 1-10 second gaps after each clip finishes. The theme plays continuously while he is active, including idle and
combat; speech plays over it. Storing or defeating him stops his theme. A short
idle clip may finish when combat begins; speech clips do not overlap.

GANACCI REMOTE
The weightless Remote is added to the Aid tab when the mod starts. Use it
and close the Pip-Boy to open its menu:
- Show his status: current state, condition and test mode.
- Summon him near me: brings him nearby.
- Send him far away: sends him to a distant map marker.
- Reset him: restarts his encounter state. After the original timer has
  expired, release-mode Reset returns him far away without a fresh wait.
- Switch test mode: toggles release timing versus immediate near spawn.
- Close: exits the menu.

Known Remote issue: summoning can produce another Salvatore while the
broken item remains in your inventory. Send away can also allow another
version to be summoned; collected duplicates can share the broken condition.
Avoid Remote relocation while carrying
him; use the normal repair/equip loop. A lifecycle fix is planned for v1.1.
The Remote is the recommended control; console commands are unnecessary.

KNOWN ISSUES
- Music continues in menus and the Pip-Boy.
- A brief sprite flicker or stutter can occur on first appearance.
- Scripted pickup can be silent.
- The Remote issue above is accepted for v1.0.
- Repair-bag compatibility has not been tested; investigation is planned
  for v1.1. No specific repair-bag support is promised.
Existing saves retain their test-mode setting; updating the plugin does
not automatically switch a previously initialized save to release mode.

UNINSTALL
Prefer returning to a save made before installation. Disable and remove
the mod through your mod manager. For a manual install, remove the plugin
and only this mod's files from the archive's meshes/textures/sound paths.
Keep files used by other mods. Restart the game.

CREDITS AND PERMISSIONS
Mod author and media preparation: 2Shay. The image depicts Salvatore Ganacci
and is not 2Shay's original artwork. The theme music is the artist's work,
used with satirical/parody intent; 2Shay will replace challenged music with
copyright-free work. This intent does not grant third-party reuse rights.
Fallout: New Vegas and its game assets are Bethesda/Obsidian material.
This is an unofficial fan mod. No permission to reuse third-party imagery
or audio is granted by this readme; code reuse terms remain unconfirmed.
