# Fallout 4: Reclamation Day

Co-op multiplayer for Fallout 4. This repository is the source code of **v1.6.7-308**.

Download the mod and find its install steps on the [Nexus page](https://www.nexusmods.com/fallout4/mods/110854).

Reclamation Day is built on [TiltedEvolution](https://github.com/tiltedphoques/TiltedEvolution) by Tilted Phoques and carries it on under the same licence. It runs on Fallout 4 version 1.10.163 with F4SE 0.6.23.

## What's here

| Folder | Contents |
| --- | --- |
| [`Code`](./Code) | The client, launcher, server and overlay |
| [`Libraries`](./Libraries) | The Tilted libraries, at the versions this release was built with |
| [`Tools`](./Tools) | The packaging scripts and the tools that write the plugins |
| [`GameFiles`](./GameFiles) | The README, credits, Papyrus script sources and settings |

The plugins, models, animations, textures and sounds aren't in this repository. They come with the mod itself.

## Building

You need Windows, Visual Studio 2022, xmake, Node.js and pnpm. From the repository root:

```
xmake config -c -p windows -a x64 -m release --vs=2022 -y
xmake build FalloutImmersiveLauncher
xmake build FalloutServerRunner
xmake build FalloutTogetherServer
xmake build TPProcess
xmake build ReclamationDayStarter
```

Then the overlay's UI, in `Code\skyrim_ui`:

```
pnpm install
pnpm deploy:develop
```

The tests are the `TPTests` target.

The version number comes from git tags, and this repository has none. A build from here calls itself "none", so its server and game only match each other.

## Credits

See [`GameFiles/Fallout4/CREDITS.txt`](./GameFiles/Fallout4/CREDITS.txt).

## Licence

GPL-3.0 or later, as with TiltedEvolution. The full text is in [`LICENSE`](./LICENSE). [`NOTICE`](./NOTICE) has the upstream licence statement, including the component under the LGPL.
