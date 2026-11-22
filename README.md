# sc64-netplay

Experimental N64 online play without a game server.

Players interact on their N64s. A phone or PC only acts as a **modem**: a static web page connects to the [SummerCart64](https://github.com/Polprzewodnikowy/SummerCart64) flashcart over USB (Web Serial, or WebUSB on Android) and links consoles directly browser-to-browser via WebRTC. Peer discovery uses [Trystero](https://github.com/dmotz/trystero) over public Nostr relays, so no backend of your own is needed.

**Modem page:** https://knedlsepp.github.io/sc64-netplay/

## How it works

```
N64 ROM ⇄ SC64 (USB) ⇄ modem page ⇄ WebRTC ⇄ modem page ⇄ SC64 ⇄ N64 ROM
```

- The ROM drives everything: it asks the modem to list lobbies, create or join one, and send game data. The modem page has nothing to configure – open it, connect the SC64 once, and it reconnects by itself next time.
- The messages between ROM and modem are documented in [PROTOCOL.md](PROTOCOL.md).
- Lobbies are found through a shared "directory" room where every host announces its lobby every couple of seconds.

## ROMs

- **Lobby demo** (`rom-lobby/`): enter your name (saved on the cart), browse online lobbies, create one (name, player count, cup) or join one. In the waiting room everyone steers a dot with the stick in real time, A waves, and the host starts a countdown. The modem page loads it onto the SC64 with one button.
- **N64 Chat** (`rom/`): a small chat client with an on-screen keyboard, used with the older test page.

## Pages

- `index.html` – the modem.
- `test.html` – the earlier test page: rooms, chat, player list with ping and connection route, ROM upload, raw USB bridge.
- `sc64.js` – the SummerCart64 USB driver shared by the modem (Web Serial + an FTDI driver on top of WebUSB).

## Browser support

| Platform | SC64 connection |
| --- | --- |
| Chrome / Edge on desktop, Firefox 151+ | Web Serial |
| Chrome on Android (USB OTG cable) | WebUSB (FTDI driver in JavaScript) |
| iOS / Safari | not supported |

Web Serial and WebUSB require `https://` or `http://localhost`. A locked phone drops its connections, so the modem keeps the screen on.

## Building with Nix

```sh
nix run github:knedlsepp/sc64-netplay     # serve the pages at http://localhost:8000 (PORT=… to change)
nix build .#web                           # pages + freshly built ROMs
nix build .#lobby-rom                     # just the lobby demo ROM
nix build .#chat-rom                      # just the chat ROM
nix develop                               # shell with N64_INST set; then: make -C rom-lobby
```

The flake uses libdragon's prebuilt mips64 toolchain. Upstream publishes it as a continuous pre-release under a fixed URL, so its hash in `flake.nix` needs updating when it is rebuilt.

## CI

- **Build ROMs:** rebuilds `chat.z64` and `lobby.z64` on changes under `rom/` or `rom-lobby/` and commits them next to the pages for GitHub Pages.
- **Check Nix flake:** builds the flake on every push.
