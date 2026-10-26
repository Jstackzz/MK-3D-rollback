# MK3D Online protocol (v1)

This is the specification that `server/` (Python, asyncio) and `launcher/`
(Qt 6) implement, and that the game-side netplay in `recomp/<game>/src/game`
implements for its half of section 3. It is versioned by one integer,
`PROTO = 1`; any incompatible change bumps it.

Three links are involved:

```
 launcher A  -- TLS/TCP, NDJSON -->  server  <-- TLS/TCP, NDJSON --  launcher B
     |  ^          (section 1)        ^  |           (section 1)          |  ^
     |  +--- UDP hello / hello_ack ---+  +---- UDP hello / hello_ack -----+  |
     |                               (section 2)                             |
     +--------------- UDP punch + ping, then the game's own traffic ---------+
     |                                                                       |
  game A  <-- env + cmd.txt / status.txt -->  launcher A   (section 3)    game B
```

Games and modes (ids are used on every link):

| game id | title | online modes | offline |
|---|---|---|---|
| `mkda` | Mortal Kombat: Deadly Alliance | `kombat` | `practice` |
| `mkd` | Mortal Kombat: Deception | `kombat`, `chess`, `puzzle` | `practice` |
| `mka` | Mortal Kombat: Armageddon | `kombat`, `motor` | `practice` |

Display names and rosters live in `launcher/data/<game>.json`.

---

## 1. Client <-> server

### 1.1 Transport

- One TCP connection per launcher, TLS 1.2 or newer (the server prefers 1.3).
  Default port **7777**.
- The server uses a **self-signed certificate** (`python -m server.makecert`).
  The launcher does not use a CA: it pins the **SHA-256 fingerprint of the
  DER-encoded certificate** (64 hex digits; colons, spaces and case are
  ignored) and accepts the handshake only if the presented certificate
  matches. Host-name and chain errors are ignored when, and only when, the
  fingerprint matches. With no fingerprint configured the launcher shows the
  presented one and asks the user whether to trust it (stored afterwards:
  trust on first use).
- Framing: **newline-delimited JSON**. Every message is one JSON object encoded
  as UTF-8 on a single line terminated by `\n` (a preceding `\r` is
  tolerated). Maximum line length 16 384 bytes; the server closes connections
  that exceed it.
- Every object has a `"type"` string. Unknown fields are ignored by both sides
  (forward compatibility). Unknown types get `error{code:"unknown_type"}`.
- A request may carry `"id"` (any JSON scalar). The direct reply to that
  request, or the `error` it caused, echoes the same `"id"`. Unsolicited
  server messages carry no `id`.
- Keepalive: the client sends `ping` every 30 s; the server drops a connection
  that has sent nothing for 120 s.
- A development-only plain-TCP mode exists (`python -m server --plain`,
  launcher address `tcp://host:port`); never use it on the internet.

### 1.2 Errors

```json
{"type":"error","code":"bad_login","message":"Wrong name or password.","re":"login","id":7}
```

`re` names the request type that failed. Codes used in v1:

| code | meaning |
|---|---|
| `bad_json`, `too_long`, `unknown_type`, `bad_request` | malformed input (field missing or out of range) |
| `proto_mismatch`, `client_outdated` | reply to `hello`; the server then closes |
| `need_hello` | anything but `hello` before `welcome` |
| `not_logged_in` | the request needs an account |
| `bad_name`, `bad_password`, `name_taken` | `register` validation |
| `bad_login`, `bad_token`, `rate_limited` | `login` / `auth` |
| `logged_in_elsewhere` | the account logged in on another connection; this one is closed |
| `bad_game`, `bad_mode`, `bad_region` | unknown game, mode or region |
| `busy` | already queued, in a lobby or in a match |
| `no_such_match`, `no_such_lobby`, `lobby_full`, `bad_lobby_password`, `not_host`, `not_ready`, `not_in_lobby` | match / lobby requests |
| `slow_down` | chat sent faster than the limit |
| `not_admin` | `admin_notice` from a normal account |
| `no_such_account` | `profile` for an unknown name |
| `idle`, `internal` | connection idle for 120 s (then closed); unexpected server error |

### 1.3 Session start and version check

```json
C: {"type":"hello","proto":1,"client":"mk3d-launcher","version":"0.1.0","os":"windows"}
S: {"type":"welcome","proto":1,"server":"mk3d-server","version":"0.1.0","udp_port":7778,
    "motd":"Welcome!","online":12,"min_client":"0.1.0",
    "games":{"mkda":["kombat"],"mkd":["kombat","chess","puzzle"],"mka":["kombat","motor"]},
    "regions":["na","sa","eu","me","af","as","oc"]}
```

- `proto` must equal the server's `PROTO`, else `error{code:"proto_mismatch"}`.
- `version` is compared component-wise with `min_client` (server option
  `--min-client`); older clients get `error{code:"client_outdated"}`. Both
  errors close the connection.
- Regions: `na` North America, `sa` South America, `eu` Europe, `me` Middle
  East, `af` Africa, `as` Asia, `oc` Oceania.

### 1.4 Accounts

Names: 3-16 characters `[A-Za-z0-9_-]`, unique case-insensitively. Passwords:
6-128 characters. The server stores `scrypt$n$r$p$<salt hex>$<hash hex>`,
computed with `hashlib.scrypt(password, salt=<16 random bytes>, n=16384, r=8,
p=1, dklen=32)` and compared in constant time. Ten failed `login`/`auth`
attempts from one IP within 5 minutes give `rate_limited` until the window
has passed.

```json
C: {"type":"register","name":"Raijin","password":"..."}
C: {"type":"login","name":"Raijin","password":"..."}
C: {"type":"auth","token":"<64 hex>"}            (resume with a remembered token)
C: {"type":"logout"}                             (revokes the token server-side)
S: {"type":"account","name":"Raijin","token":"<64 hex>","token_expires":1767225600,
    "udp_token":"<32 hex>","admin":false,"profile":{"...":"as in 1.5"}}
S: {"type":"logged_out"}
```

- `register` logs the new account in (same `account` reply).
- `token` is a persistent login token (30 days, stored only as its SHA-256 on
  the server). The launcher keeps it when "remember me" is checked.
- `udp_token` is a **per-connection session token** for the UDP rendezvous
  (section 2). It is not a login credential (it cannot be used with `auth`),
  so seeing it on the wire does not expose the account.
- Logging in on a second connection closes the first one with
  `error{code:"logged_in_elsewhere"}`.

### 1.5 Profile

```json
C: {"type":"profile"}                    (own profile; or {"type":"profile","name":"Raijin"})
S: {"type":"profile","name":"Raijin","created":1761300000,
    "stats":{"mkd":{"kombat":{"ranked":{"elo":1532,"wins":4,"losses":2},
                              "casual":{"wins":10,"losses":7}}}}}
```

Stats exist per game per mode: ranked Elo (start 1500) with ranked
wins/losses, and casual wins/losses. A missing entry means "never played"
(Elo 1500, 0-0).

### 1.6 Online count and notices

```json
C: {"type":"online"}
S: {"type":"online","count":12,"connected":14,"queued":3,"playing":4,
    "queues":{"mkd/kombat/casual":2,"mkd/kombat/ranked":1}}
S: {"type":"notice","text":"Server restarts in 10 minutes.","level":"warn"}
C: {"type":"admin_notice","text":"...","level":"info"}      (admins only, server --admins)
```

`count` = logged-in accounts, `connected` = open connections, `playing` =
players in matches past the accept step. The server pushes `online` to every
connection when the numbers change, at most once per 2 s. `notice` is pushed
to everyone; `welcome.motd` carries the message of the day. `level` is
`info` or `warn`.

### 1.7 Matchmaking queue

```json
C: {"type":"queue_join","game":"mkd","mode":"kombat","ranked":false,"region":"eu",
    "max_ping":150,"strict":false,"character":"Scorpion","delay":2,"server_rtt":24}
S: {"type":"queued","game":"mkd","mode":"kombat","ranked":false,"region":"eu",
    "max_ping":150,"strict":false,"since":1761300000.5,"in_queue":3,"elo":1500}
C: {"type":"queue_leave"}
S: {"type":"queue_left","reason":"left"}
```

- `max_ping` is the highest acceptable round trip in ms; `0` means any.
- `delay` is the player's preferred input delay in frames (0-4).
- `server_rtt` (optional) is the client's last measured UDP round trip to the
  server in ms (section 2.2); it only orders otherwise equal ranked candidates.
- `character` is a roster id from the launcher data (e.g. `SubZero`); it may
  be empty or `-` for modes where the team is picked in the game (`chess`).
- One queue entry per account; joining again replaces the entry. A player in
  a lobby or a match gets `busy`. `queue_left.reason` is `left` or
  `removed` (declined or let the accept window run out).
- **Casual pairing (first come, first served).** The queue is scanned in join
  order; each waiting player is paired with the earliest-joined compatible
  player. Compatible = same game and mode, both casual, different accounts,
  same region (or, if neither is `strict`, different regions once both have
  waited 20 s), and no cached ping for this pair above either player's
  `max_ping` (ping cache below).
- **Ranked pairing (Elo window).** The same rules plus an Elo window: a pair
  is allowed when `|elo_a - elo_b| <= min(window_a, window_b)`, with
  `window = 100 + 50 * floor(waited / 10 s)`, capped at 600. `strict` pins the
  window at 100 and the region to the player's own. Among the allowed
  partners of the longest-waiting player the server takes the smallest Elo
  difference, then the lowest estimated ping (sum of both players' UDP round
  trips to the server, a rough proxy), then the earliest join time. Ranked
  never pairs two connections from the same public IP (server option
  `--allow-same-ip` lifts this for LAN events and development).
- **Ping cache (measured ping).** Every `ping_report` stores the measured
  round trip for that pair of accounts for 10 minutes; a pair whose punch
  failed is stored as unreachable. The matchmaker skips cached pairs that are
  unreachable or above either player's `max_ping`. When a match is cancelled
  for a ping or punch reason, both players go back to the queue with their
  original join time.
- The matchmaker runs on every `queue_join` and once per second.

### 1.8 Match setup (queue and lobby matches)

```
 found --accept x2--> accepted --fresh UDP hello x2--> punching --ping_report x2--> playing --result x2--> done
   | decline / 15 s      | 10 s                           | fail / too slow / 15 s       | 120 s after the 1st report
   +------------------> cancelled <-----------------------+                              +--> unconfirmed
```

1. **Found.**
   ```json
   S: {"type":"match_found","match":"5f2c9a01d4e3","game":"mkd","mode":"kombat","ranked":false,
       "side":1,"accept_timeout":15,"accept_required":true,"lobby":null,
       "opponent":{"name":"Fujin","elo":null,"region":"eu","character":"Raiden"}}
   ```
   In ranked, `opponent` is `null` until both have accepted (no dodging by
   name). Lobby matches have `accept_required:false` and `accept_timeout:0`
   (the players already readied up) and continue at step 3.
2. **Accept window: 15 s.**
   ```json
   C: {"type":"match_accept","match":"5f2c9a01d4e3"}
   C: {"type":"match_decline","match":"5f2c9a01d4e3"}
   ```
   A decline or a missing accept cancels the match. The player who declined
   or did not answer leaves the queue (`queue_left{reason:"removed"}`); the
   other goes back with their original join time (`requeued:true`).
3. **UDP endpoints.** On `match_found` the launcher binds its game UDP port
   and sends UDP `hello` packets (section 2) until `hello_ack` arrives. The
   server needs a hello from each side received after `match_found`; when
   both have accepted and both endpoints are known it sends:
   ```json
   S: {"type":"match_peer","match":"5f2c9a01d4e3","side":1,"key":"<32 hex>","punch_timeout":10,
       "same_nat":false,
       "peer":{"public":"203.0.113.7:7001","lan":"192.168.1.20:7001"},
       "opponent":{"name":"Fujin","elo":1488,"region":"eu","character":"Raiden"}}
   ```
   No IP address is revealed before both players have accepted. If an
   endpoint is still missing 10 s after acceptance the match is cancelled
   (`no_udp`).
4. **Punch and measure** (section 2.3), then:
   ```json
   C: {"type":"ping_report","match":"5f2c9a01d4e3","ok":true,"rtt_ms":38,
       "remote":"203.0.113.7:7001","via":"public"}
   C: {"type":"ping_report","match":"5f2c9a01d4e3","ok":false,"rtt_ms":0}
   ```
5. **Go, relay or cancel.** When both reports are in:
   - either `ok:false`, the first time: the match moves to the server's relay
     (section 2.4): both get a second `match_peer` with `"relay":true` and
     empty `peer` endpoints, punch and ping through the server's UDP port, and
     report again (`via:"relay"`, `remote` = the server's UDP endpoint);
   - either `ok:false` again, or with the relay off (`--no-relay`):
     `match_cancelled{reason:"punch_failed"}`;
   - the worse `rtt_ms` above the smaller non-zero `max_ping` of the two:
     `match_cancelled{reason:"ping_too_high"}`;
   - otherwise both get `match_go`:
   ```json
   S: {"type":"match_go","match":"5f2c9a01d4e3","game":"mkd","mode":"kombat","ranked":false,
       "side":1,"remote":"203.0.113.7:7001","rtt_ms":38,"delay":2,"seed":2718281828,"rounds":3,
       "p1":"Scorpion","p2":"Raiden","p1_name":"Raijin","p2_name":"Fujin","lobby":null}
   ```
   `delay` = the larger of both preferences, at least 1 in ranked. `seed` is
   a fresh 32-bit value. Sides are random for queue matches; in a lobby the
   host is side 1. `remote` echoes the endpoint this side reported. A missing
   report 15 s after `match_peer` cancels with `ping_timeout`.
6. **Cancellation** at any step before `playing`:
   ```json
   S: {"type":"match_cancelled","match":"5f2c9a01d4e3","reason":"declined","requeued":true}
   ```
   Reasons: `declined`, `accept_timeout`, `no_udp`, `punch_failed`,
   `ping_too_high`, `ping_timeout`, `peer_left`. `requeued` tells this
   player whether they are back in the queue.
7. **Playing.** The launcher writes the `start` line (section 3) and may tell
   the server what the game does:
   `{"type":"match_state","match":"...","state":"in_match"}` (informational).

### 1.9 Result reporting

Both launchers report what their game wrote to `status.txt`:

```json
C: {"type":"result","match":"5f2c9a01d4e3","winner":1,"p1_rounds":2,"p2_rounds":1}
C: {"type":"result","match":"5f2c9a01d4e3","outcome":"desync","frame":1234}
C: {"type":"result","match":"5f2c9a01d4e3","outcome":"disconnected"}
S: {"type":"result_ack","match":"5f2c9a01d4e3"}
S: {"type":"match_result","match":"5f2c9a01d4e3","status":"agreed","winner":1,
    "p1_rounds":2,"p2_rounds":1,"ranked":true,
    "elo":{"Raijin":{"before":1500,"after":1516,"delta":16},
           "Fujin":{"before":1500,"after":1484,"delta":-16}}}
```

- **Agree** (same `winner`, `p1_rounds`, `p2_rounds`): status `agreed`.
  Ranked: Elo update with `K = 32`, `E_a = 1 / (1 + 10^((R_b - R_a) / 400))`,
  `d = round(K * (S_a - E_a))` (half away from zero), `R_a' = R_a + d`,
  `R_b' = R_b - d` (zero-sum, ratings stay integers), plus ranked
  wins/losses. Casual: casual wins/losses only. `elo` is present for ranked
  matches.
- **Disagree**: status `disputed`; nothing changes, both reports stay in the
  database and the server logs a `DISPUTE` line.
- **Desync or disconnect** reports: status `no_contest`; nothing changes;
  logged.
- **Only one report** within 120 s of the first: status `unconfirmed`;
  nothing changes; logged. (v1 has no disconnect penalty.)
- Lobby matches count as casual. After any final status the lobby returns to
  `open` and both players' `ready` flags are cleared.

### 1.10 Lobbies

A lobby has a 5-character code from `ABCDEFGHJKLMNPQRSTUVWXYZ23456789`, at
most two players, an optional password, and is always casual in v1.

```json
C: {"type":"lobby_create","name":"Friday sets","game":"mkd","mode":"kombat",
    "password":"","rounds":3,"region":"eu","character":"Scorpion"}
C: {"type":"lobby_list","game":"mkd"}                          (game optional)
S: {"type":"lobbies","lobbies":[{"id":"K7WQ2","name":"Friday sets","host":"Raijin",
    "game":"mkd","mode":"kombat","region":"eu","players":1,"max_players":2,
    "locked":false,"state":"open","rounds":3}]}
C: {"type":"lobby_join","lobby":"K7WQ2","password":"","character":"Kenshi"}
C: {"type":"lobby_leave"}
C: {"type":"lobby_chat","text":"gg"}
C: {"type":"lobby_character","character":"Kenshi"}
C: {"type":"lobby_ready","ready":true,"delay":2}               (delay optional, 0-4)
C: {"type":"lobby_start"}                                       (host only)
S: {"type":"lobby","lobby":{"id":"K7WQ2","name":"Friday sets","host":"Raijin","game":"mkd",
    "mode":"kombat","region":"eu","rounds":3,"locked":false,"state":"open","match":null,
    "members":[{"name":"Raijin","character":"Scorpion","ready":true,"host":true},
               {"name":"Fujin","character":"Kenshi","ready":true,"host":false}],
    "chat":[{"from":"Fujin","text":"gg","ts":1761300042.1}]}}
S: {"type":"lobby_chat","lobby":"K7WQ2","from":"Fujin","text":"gg","ts":1761300042.1}
S: {"type":"lobby_left","lobby":"K7WQ2"}
```

- The full `lobby` state is pushed to every member after any change; `chat`
  holds the last 30 messages. Chat text: 1-200 characters, at most one
  message per 0.5 s per player (`slow_down`).
- `lobby_create` and `lobby_join` reply with `lobby`; errors: `busy`,
  `no_such_lobby`, `lobby_full`, `bad_lobby_password`. `rounds` is 1, 3 or 5.
- `lobby_start`: host only (`not_host`), needs two members who are both ready
  (`not_ready`). It starts the match flow of 1.8 at step 3 with the host as
  side 1; the lobby `state` becomes `in_match` with `match` set, and the reply
  is that `lobby` state. Changing character clears your `ready` flag.
- If the host leaves, the other member becomes host; an empty lobby is
  deleted. A member who disconnects leaves the lobby (and cancels a match
  that has not reached `playing`).
- States: `open`, `full`, `in_match`. `locked` = has a password.

### 1.11 Leaderboard

```json
C: {"type":"leaderboard","game":"mkd","mode":"kombat","limit":100,"offset":0}
S: {"type":"leaderboard","game":"mkd","mode":"kombat","total":57,
    "rows":[{"rank":1,"name":"Raijin","elo":1712,"wins":40,"losses":12}],
    "me":{"rank":9,"elo":1580,"wins":8,"losses":3}}
```

Accounts with at least one ranked game in that game and mode, ordered by Elo,
then wins, then name; `limit` at most 200. `me` is present when the asking
account is ranked there.

### 1.12 Keepalive and disconnects

```json
C: {"type":"ping"}      S: {"type":"pong","t":1761300050.0}
```

Closing the connection leaves queues and lobbies, cancels matches that have
not reached `playing` (`peer_left` for the other side, who is requeued when
the match came from the queue), and keeps `playing` matches open for the
other side's result report.

---

## 2. NAT traversal: UDP rendezvous

### 2.1 Server endpoint

The server listens on one UDP port (default **7778**) next to its TCP port.
Datagrams are single JSON objects, UTF-8, at most 512 bytes. The server never
answers a datagram that does not carry a valid token, so it cannot be used
as a reflector, and it never answers with more bytes than it received
(clients always include `lan` and `t`, which keeps the hello larger than the
answer).

### 2.2 Punch hello

Sent by the launcher **from the game UDP port** (the port the game binds
later, default 7001), so the NAT mapping the server observes is the one the
game will use:

```json
L->S: {"type":"hello","token":"<udp_token>","match":"5f2c9a01d4e3","lan":"192.168.1.20:7001","t":123456}
S->L: {"type":"hello_ack","public":"203.0.113.7:7001","t":123456}
```

- `token` is `account.udp_token` of this connection; `match` is optional
  (empty while queued). `lan` is the launcher's LAN address (the local address
  of its TCP connection) with the game port. `t` is echoed so the client can
  measure its round trip to the server; the client sends it in later
  `queue_join` messages as `server_rtt` (used only as a tie-breaker).
- The server records the packet's source address as the client's **public
  endpoint** and the `lan` field as its **LAN endpoint**.
- Clients send a hello when they join a queue, every 15 s while queued (keeps
  the NAT mapping and the round-trip figure fresh), and every 250 ms after
  `match_found` until `hello_ack` arrives (giving up after 10 s).
- The launcher binds the game port on all interfaces, so a player across the
  internet reaches it even when the server runs on the same PC; one-PC tests
  set `[dev] udp_loopback=true` to stay on 127.0.0.1.
- A peer endpoint at a loopback address, or a private one while this launcher
  reaches the server at a public address, belongs to a player on the server's
  own network: the launcher punches it at the server's address with the
  peer's port instead.
- `match_peer` (1.8 step 3) carries the other side's `public` and `lan`
  endpoints. `same_nat:true` means both public IPs are equal; then the LAN
  endpoint is the one expected to work (many routers do not hairpin).

### 2.3 Peer punch and ping

Both launchers, still on the game port, send to **both** candidate endpoints
of the peer every 100 ms until one answers, for at most `punch_timeout`
seconds:

```json
{"type":"punch","match":"5f2c9a01d4e3","key":"<32 hex>","side":1}
{"type":"punch_ack","match":"5f2c9a01d4e3","key":"<32 hex>","side":2}
```

- Packets whose `match` or `key` do not match the current `match_peer` are
  ignored. A `punch` is always answered with `punch_ack` to its source
  address. The first candidate that answers with `punch_ack` is the working
  endpoint (`via` = `lan` or `public`). A `punch` from an address that is not
  a candidate (the peer's router chose another public port) becomes a
  candidate too.
- Then 5 pings, 100 ms apart, to the working endpoint:
  `{"type":"ping","match":"...","key":"...","seq":n,"t":<ms>}`, answered with
  `{"type":"pong",...}` carrying the same `seq` and `t`. `rtt_ms` = median of
  the answered pings; at least 3 must come back.
- The launcher keeps answering the peer's punches and pings until it gets
  `match_go` or `match_cancelled`.
- The result goes to the server as `ping_report` (1.8 step 4); `ok` is
  `false` when no candidate answered or fewer than 3 pings came back.
- After `match_go` the launcher closes its socket and writes the `start` line
  at once; the game binds the same port and should start sending within a
  second, so the NAT mappings opened by the punch stay alive.

### 2.4 Relay

When the direct punch fails (or the server runs with `--relay-only`), the
server's UDP port carries the match. The server joins the two endpoints their
hellos came from; from then on every datagram from one of them that is not a
`hello` goes to the other one byte for byte: the launchers' `punch` / `ping`
JSON while measuring, then the games' packets. Each launcher punches the
server's UDP endpoint (the one its hellos go to, the only candidate,
`via:"relay"`), and after `match_go` its game sends to it from the same port,
so the router keeps the mapping the hellos opened.

- Only the two endpoints of a match in `punching` or `playing` are joined; a
  packet is never answered with more bytes than it had; each endpoint may send
  `relay_rate` packets a second (400, bursts of 800); a datagram over 1472
  bytes is dropped.
- The route ends 15 s after the match is done or cancelled.
- The server logs each relayed match and the packets that went each way.

### 2.5 Port mapping

The launcher opens the game UDP port on the router (UPnP Internet Gateway
Device: SSDP discovery, the device description, `AddPortMapping` with a 1 h
lease, renewed every 20 min, permanent on routers that refuse leases) unless
Settings turns it off (`online/upnp`). A router whose own address is private
or carrier-grade NAT (100.64/10) cannot be opened from outside; the launcher
says so and leaves the match to punching and the relay. The server does the
same for its TCP and UDP ports with `--upnp` (a server on a home PC).

### 2.6 Out of scope

- NAT-PMP / PCP port mapping (UPnP only).
- IPv6: the server and launcher use IPv4 only.

---

## 3. Launcher <-> game process

The launcher owns the game process. There is no socket between them, only
environment variables and two small text files in a per-session folder.

### 3.1 Environment

| variable | value |
|---|---|
| `MK3D_NET` | `1`: launcher-controlled; boot to the hold point and follow `cmd.txt` |
| `MK3D_GAME` | `mkda`, `mkd` or `mka` |
| `MK3D_SESSION_DIR` | absolute path of the per-session folder |
| `MK3D_CMD` | `<session>\cmd.txt` |
| `MK3D_STATUS` | `<session>\status.txt` |
| `MK3D_INSTANCE` | `1` normally; `1` and `2` for the two instances of a local test |
| `MK3D_LOG` | `<session>\game.log` (suggested log destination) |
| `MK3D_DISPLAY` | `windowed`, `borderless` or `fullscreen` (advisory) |
| `MK3D_RESOLUTION` | e.g. `1920x1080` (advisory) |
| `MK3D_VSYNC` | `0` or `1` (advisory) |

The working directory is the folder of the game executable. The launcher
creates one session folder per game process
(`<launcher dir>\sessions\<game>-<yyyyMMdd-hhmmss>-<instance>`) and puts game
processes into a job object that kills them when the launcher exits.

### 3.2 File rules (both files)

- ASCII, `\n` terminated (`\r\n` tolerated), no BOM. `cmd.txt` is one line.
  `status.txt` may carry further `key=value` lines after the first one
  (informational, for example `ping=38` or `rollback=2`); the launcher
  interprets only the first line.
- **Atomic replacement**: write `<file>.tmp` in the same folder, close it, then
  `MoveFileExW(tmp, file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`.
  If the replace fails because a reader holds the file open, retry every 5 ms
  for up to 1 s. Readers open with
  `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE`.
- Line grammar: a verb (or state) followed by space-separated `key=value`
  tokens. Values contain no spaces (`[A-Za-z0-9_.:-]`); unknown keys are
  ignored; keys may come in any order.

### 3.3 `cmd.txt` (launcher -> game)

The game polls for `cmd.txt` at least every 100 ms. It **claims** a command by
renaming `cmd.txt` to `cmd.txt.taken` (replacing an older one), then reads and
executes it. Claim-by-rename means a command written while the game reads
can never be lost. The launcher writes a new command only after the previous
one was taken, or after 2 s; a newer untaken command replaces the older one
(latest wins).

| command | meaning |
|---|---|
| `start game=mkd mode=kombat side=1 local_port=7001 remote=1.2.3.4:7002 delay=2 seed=123456 p1=Scorpion p2=SubZero rounds=3 ranked=1 match=<id>` | start an online match from the hold point |
| `start game=mkd mode=practice side=1 p1=Scorpion p2=SubZero` | start offline practice (no network keys) |
| `park` | leave the current match or practice and return to the hold point |
| `rematch [seed=N] [match=<id>]` | play again against the same peer with the same settings (new seed and id when given) |
| `quit` | exit the process cleanly |

`start` keys: `game`; `mode` (`kombat`, `chess`, `puzzle`, `motor`,
`practice`); `side` (local player, 1 or 2); `local_port` (UDP port to bind);
`remote` (peer `ip:port`); `delay` (input delay in frames); `seed` (32 bit,
identical on both sides); `p1`/`p2` (roster ids, identical on both sides,
`-` when the mode picks in the game); `rounds` (best of N); `ranked` (0 or
1); `match` (id, echoed in status lines). Optional: `p1_name`, `p2_name`
(account names), `stage` (absent = the game derives it from the seed). Both
sides get the same line except for `side`, `local_port` and `remote`.

### 3.4 `status.txt` (game -> launcher)

The launcher polls `status.txt` every 100 ms and acts on every change of the
first line.

| first line | meaning |
|---|---|
| `booting` | process started, loading |
| `ready` | parked at the hold point, able to take `start` |
| `in_match match=<id>` | a match (or practice: `match=practice`) is running |
| `result winner=1 p1_rounds=2 p2_rounds=1 match=<id>` | the match ended normally (`winner` is 1 or 2); the game stays on its result screen until `park`, `rematch` or `quit` |
| `desync frame=N match=<id>` | the two simulations diverged at frame N; the match is aborted |
| `disconnected match=<id>` | the peer stopped answering |
| `error <text>` | something failed (`error no connection`, `error bad command`); free text to the end of the line |

`match=<id>` (echoed from `start`) is recommended on the match states so that
identical results of two consecutive matches are still different lines.

Typical sequences: `booting` -> `ready` -> (`start`) -> `in_match` ->
`result ...` -> (`park`) -> `ready`; practice: `ready` -> (`start
mode=practice`) -> `in_match match=practice` -> (`park`, or the player leaves
practice in the game) -> `ready`.

The launcher reports `result`, `desync` and `disconnected` to the server
(1.9), shows every state in its footer and log, sends `park` once the server
has answered with `match_result` (or after 10 s), and treats an exit of the
process as `error exited`.

### 3.5 Local test

For development the launcher starts two instances on one PC. Once both
report `ready`, instance 1 gets `side=1 local_port=7001 remote=127.0.0.1:7002`
and instance 2 gets `side=2 local_port=7002 remote=127.0.0.1:7001`, both with
the same `seed`, `p1`, `p2`, `rounds`, `delay` and `match=local-<time>`. No
server is involved.
