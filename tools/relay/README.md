# KVM relay

A VPS with a public IP that KVM dongles dial *out* to, so a dongle behind any
home router is reachable from any network: no port forward at home, no
always-on machine. The phone opens `https://<relay host>/?id=<board id>`.

| file | goes to | what |
|---|---|---|
| `relay.py` | `/opt/argon-relay/relay.py` | the relay (stdlib asyncio, no deps) |
| `argon-relay.service` | `/etc/systemd/system/` | runs it as user `argonrelay` |
| `kvm.caddy` | `/etc/caddy/conf.d/` | HTTPS for phones (Caddy terminates TLS, proxies to 127.0.0.1:8080) |
| `argon-relay-cert-sync` (+ `.service`, `.timer`) | `/usr/local/sbin/`, `/etc/systemd/system/` | copies Caddy's cert to the board TLS port, daily |
| `argon-push` | `/usr/local/sbin/` | pushes an update to a board |

Secrets live only in `/etc/argon-relay/relay.env` (chmod 600), never here:

```
RELAY_SECRET=<what the boards present; the 3rd line of their KVMRLY.TXT>
WEB_PASS_SHA256=<fallback for boards too old to send their own hash>
PUBLIC_HOST=127.0.0.1
PUBLIC_PORT=8080
BOARD_PORT=7000
```

Ports open to the world: 443 (Caddy), 7443 (boards, TLS), 7000 (boards, plain,
for old firmware). The admin port 7999 is bound to 127.0.0.1.

## How it behaves

- **Many boards.** Each announces an id (the owner's choice at agent setup, or
  derived from the secret). ids are not unique and not stored: a board holds its
  id while it is connected. A second board claiming a held id is refused
  (`AGKVM1-IDBUSY`) and its agent tells the owner to pick another. Boards are told
  apart by a token each keeps in `C:\KVMNODE.TXT`, not by the secret they share.
- **Access is the password, per board.** Reaching someone else's board through an
  id clash gets you its password prompt, nothing more.
- **One viewer at a time.** A second browser gets "control is busy" and may take
  over with the board's password (checked here against the SHA-256 the board sent
  in its hello). The board is then reserved for the taker so the old tab cannot
  grab it back, and the board is told the session is already authorised - one
  password, not two.
- **The board side is always dropped with a TCP reset.** A polite TLS close leaves
  a board that is busy writing a picture blocked for half a minute.

## Updating a board over the air

```
scp KVM.AXE vps:/tmp/ && ssh vps argon-push <board-id> app /tmp/KVM.AXE
ssh vps argon-push <board-id> os   /tmp/argonos.bin      # spare app slot
ssh vps argon-push <board-id> disk /tmp/usbdisk.img      # the agent's flash drive
```

The board takes it only if its owner put a 4th line `update` in `KVMRLY.TXT`,
checks the SHA-256, installs and restarts (about 12 s for an app, 35 s for the
system). When the ABI moves, push `os` first, then `app`: a newer KVM.AXE on an
older system does not load, and the board drops off the relay.
