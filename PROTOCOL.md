# N64 ⇄ modem protocol (version 1)

The web page (`index.html`) acts as a **modem**: it connects to the SummerCart64 over USB and to the internet over WebRTC (Trystero). It has no UI of its own beyond a connect button. Everything players see and do happens in the ROM, which drives the modem with the messages below.

## Transport

- All modem traffic uses the UNFLoader/libdragon USB datatype **`0x40`**.
- **N64 → modem:** one message per `usb_write(0x40, msg, len)`.
- **Modem → N64:** messages are batched. One USB transfer carries one or more frames, each `u16 length` followed by `length` bytes of message. The modem sends at most one transfer every 40 ms and keeps each transfer ≤ 2048 bytes.
- All integers are big-endian (the N64's native byte order).
- `str` = `u8 length` + that many bytes (ASCII/UTF-8, no terminator, ≤ 63 bytes).
- Byte 0 of every message is its type.

## Player slots

The modem gives every player in the current lobby a **local slot number**: `0` is always yourself, other players get `1…7` in the order they appeared. Slots are local to each console. If a game needs a shared order (e.g. grid positions), the host should decide it and send it as game data.

## N64 → modem

| Type | Name | Payload | Meaning |
| --- | --- | --- | --- |
| `0x01` | `HELLO` | `u8 version`, `str app`, `str player_name` | Start a session. `app` separates games (e.g. `lobbydemo`, `mk64`). The modem answers with `STATUS`. Repeat about once per second until a `STATUS` arrives; repeats within 3 s count as retries. A later `HELLO` starts a fresh session and leaves any lobby, so only send it at boot, after the modem was lost, or to change the name outside a lobby. |
| `0x02` | `BROWSE` | – | Join the lobby directory for `app` and start sending `LOBBY_LIST` updates. |
| `0x03` | `CREATE` | `u8 max_players`, `str lobby_name`, `str info` | Create a lobby and become its host. `info` is free text shown in lists (e.g. `150cc Mushroom Cup`). Answered with `JOINED`. |
| `0x04` | `JOIN` | `str lobby_id` | Join a lobby from the list. Answered with `JOINED` or `LEFT`. |
| `0x05` | `LEAVE` | – | Leave the current lobby (answered with `LEFT`, reason 0). |
| `0x06` | `SEND` | `u8 target`, `u8 channel`, payload… | Send game data. `target` = slot or `0xFF` for everyone. `channel` is up to the game. Payload ≤ 1024 bytes. |
| `0x07` | `SET_INFO` | `u8 open`, `str info` | Host only: update the lobby's `info` and whether it is listed as open (e.g. `open = 0` once the race started). |

## Modem → N64

| Type | Name | Payload | Meaning |
| --- | --- | --- | --- |
| `0x81` | `STATUS` | `u8 version`, `u8 flags`, `u8 players_online` | Sent after `HELLO` and every 2 s. `flags`: bit 0 = internet ready, bit 1 = browsing, bit 2 = in a lobby, bit 3 = host. `players_online` = peers seen in the directory. If no message arrives for 5 s, the modem is gone. |
| `0x82` | `LOBBY_LIST` | `u8 count`, then per lobby: `str id`, `str name`, `str host_name`, `u8 players`, `u8 max_players`, `u8 open`, `str info` | Full list of lobbies for `app` (at most 12), sent whenever it changes, at most once per second. |
| `0x83` | `JOINED` | `u8 is_host`, `str lobby_id`, `str lobby_name`, `u8 max_players`, `str info` | You are now in the lobby. |
| `0x84` | `PLAYER_JOINED` | `u8 slot`, `u8 is_host`, `str name` | Another player is in the lobby (also sent for everyone already there when you join). Sent again if a name changes. |
| `0x85` | `PLAYER_LEFT` | `u8 slot` | A player left. |
| `0x86` | `DATA` | `u8 from_slot`, `u8 channel`, payload… | Game data from another player. |
| `0x87` | `LEFT` | `u8 reason` | You are no longer in a lobby. Reasons: `0` you left, `1` host left, `2` lobby full, `3` lobby not found, `4` connection lost. |
| `0x88` | `ERROR` | `str message` | Something went wrong; show it to the player. |

## How the modem implements it

- **Directory:** a Trystero room `<app>/directory`. Every host announces its lobby there every 2 s (`id`, name, host name, player count, max, open, info). Entries that have not been announced for 6 s are dropped.
- **Lobby:** a Trystero room `<app>/lobby/<id>`. On joining, everyone exchanges names. The host tells newcomers it is the host, and rejects them with "full" when the lobby has no free slot.
- If the host leaves, everyone else gets `LEFT` with reason 1.
