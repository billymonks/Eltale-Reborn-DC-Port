# Playing

## Buttons

| Dreamcast | Quest 64 | Eltale Reborn |
| --- | --- | --- |
| Stick | Walk, and menu cursors | same |
| A | Talk, confirm, attack | same |
| B | Cancel, close menus | same |
| D-pad | Spirits. Up opens the spell menu | same |
| Start | Status and items | Opens on the map. The stick still changes page |
| Left trigger | | Previous item filter |
| Right trigger | Z | Next item filter, open chests |
| Y | | Item menu |
| X | | Sprint while held |

Menus (the title screen's NEW / LOAD / DELETE included) move with the stick.

The stick's deadzone is 14 counts out of ±128, and full deflection is 110, scaled onto the N64 stick's ±80. Direction is kept.

X is an N64 button bit nothing on a real pad sets (`0x0040`). Quest 64 never reads it, so the gameplay mod can bind sprint there and still leave Z for chests. Sprint length is the mod's `sprint_speed`, 1.25 by default. On the Quest 64 disc, X does nothing.

## Saves

The game sees a Controller Pak. Files live on the first VMU found, usually controller A's first slot.

| Disc | VMU file | Description |
| --- | --- | --- |
| Quest 64 | `QUEST64.PAK` | Quest 64 saves |
| Eltale Reborn | `Q64ELT.PAK` | Eltale Reborn saves |

One save is 3 blocks. The whole 0x200-byte note is stored, including 0x160-0x1FF, which Eltale uses. The two discs do not share a file.

The VMU is read at boot and when a different one is plugged in. It is written about 300 ms after the game's last write (around 0.7 s in Flycast). Leave it inserted through that. With no VMU the game reports no Controller Pak and plays on, as it would on an N64. A VMU with no room fails with the game's own "full" error.

Flycast names its VMU images from the disc serial (`Q64-DC`, Eltale `Q64-ELT`; test discs `Q64-DC-T` and `Q64-TELT`). The serials stay fixed for that reason.

## Running

Open the CDI in Flycast. 2.6 and 2.7, OpenGL and Vulkan, have been used, with the HLE BIOS and a real one. For the serial log, set `Debug.SerialConsoleEnabled = yes` in `emu.cfg` and start Flycast from a terminal. Test discs print once a second (retraces, tasks, map, renderer, audio) and draw a red bar across the top. Release discs stay quiet. The serial port is slow enough to cost frames.

The runtime looks for the ROM at `/cd/quest64.z64`, then `/pc/quest64.us.z64` (dcload), then `/sd/quest64.z64`.

From the repo, with dcload-ip:

```
dc-tool-ip -t <dreamcast ip> -c . -x build/quest64-dc.elf
```

`-c .` serves this directory as `/pc`, so the ROM is `quest64.us.z64` over the link. That is slow, worse over serial. Use the disc for anything long. `dc-tool-ser` takes the same `-c` and `-x`.

`build/quest64-dc.cdi` is about 740 MB and fits an 80-minute CD-R. mkdcdisc writes raw 2336-byte sectors and pads the data track to the outer edge, where a real drive reads fastest. The disc is about 73 minutes (the data track is 316,212 sectors, the ELF and ROM in the last 8,700). A GDEMU or similar can take the CDI as it is. Passing mkdcdisc `-N` would put the files at the slow inner edge in a 22 MB image. The build leaves them on the outer edge.

If Flycast's list shows the wrong name or a Quake box, an older title (`Quest 64 DC`) was matched to TheGamesDB and cached. Close Flycast, back up `data/boxart/flycast-gamedb.json` (next to `flycast.exe`, or in Flycast's home folder), and find the object whose `file_name` is `quest64-dc.cdi`. Delete that object, or set `"name"` to `Quest 64` and `"boxart_path"` to a PNG or JPG in that folder. Leave `parsed` and `scraped` true.
