# sc64-netplay

Experimental N64 netplay over the internet without a game server.

A static web page connects to a [SummerCart64](https://github.com/Polprzewodnikowy/SummerCart64) flashcart over USB (Web Serial, or WebUSB on Android) and links players directly browser-to-browser via WebRTC. Peer discovery uses [Trystero](https://github.com/dmotz/trystero) over public Nostr relays, so no backend of your own is needed.

**Live page:** https://knedlsepp.github.io/sc64-netplay/

## What it does

- **Rooms & chat:** everyone who opens the page with the same room name (`?room=…`) finds each other. The player list shows the ping and whether the connection is direct (LAN or hole punching) or relayed via TURN.
- **SC64 control:** connect, read the firmware version and upload ROMs (`.z64`, `.v64` and `.n64` byte orders are converted automatically).
- **N64 ⇄ internet bridge:** data a homebrew ROM sends with `USB_WRITE` goes to all peers, and data from peers is written to the N64. Text (USB type 1) becomes chat messages.
- **N64 Chat ROM** (`rom/`): a small libdragon program with an on-screen keyboard that lets you chat from the console. The page can load it onto the SC64 with one click.

## Browser support

| Platform | SC64 connection |
| --- | --- |
| Chrome / Edge on desktop, Firefox 151+ | Web Serial |
| Chrome on Android (USB OTG cable) | WebUSB (FTDI driver in JavaScript) |
| iOS / Safari | not supported |

Web Serial and WebUSB require `https://` or `http://localhost`.

## Building with Nix

```sh
nix run github:knedlsepp/sc64-netplay     # serve the page at http://localhost:8000 (PORT=… to change)
nix build .#web                           # page + freshly built chat ROM
nix build .#chat-rom                      # just the ROM
nix develop                               # shell with N64_INST set; then: make -C rom
```

The flake uses libdragon's prebuilt mips64 toolchain. Upstream publishes it as a continuous pre-release under a fixed URL, so its hash in `flake.nix` needs updating when it is rebuilt.

## CI

- **Build chat ROM:** rebuilds `chat.z64` on changes under `rom/` and commits it next to the page for GitHub Pages.
- **Check Nix flake:** builds the flake on every push.
