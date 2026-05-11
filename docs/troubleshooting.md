# Troubleshooting

How to read the log and figure out where things went wrong. The driver
is verbose on purpose — failures should be diagnosable from a single
serial session.

## Boot pipeline cheat sheet

A successful cold-start log progresses through these stages:

```
[I] === micro-mongodb boot (board=...) ===          ← stdio + logger up
[I] [wifi] connecting to '...' ...
[I] [wifi] connected                                ← associated + DHCP done
[I] [time] starting SNTP against pool.ntp.org ...
[I] [time] SNTP synced: unix=... offset=...
[I] [app] wall clock: 2026-...Z                    ← real time available
[I] [client] parsed URI: ...                       ← URI parser OK
[I] [client] connecting to ...:27017 (TLS)
[I] [auth] saslSupportedMechs for admin.user: [...]
[I] [client] redirecting to primary ...            ← optional, only if SRV gave us a secondary
[I] [client] connecting to ...:27017 (TLS)
[I] [auth] saslSupportedMechs for admin.user: [...]
[W/I] using legacy mechanism                       ← only on SHA-1-only users
[I] [client] ready (host=...)
[I] [client] created time-series collection ...    ← first boot, subsequent are silent
[I] [telemetry] sample=... ts=...
[I] [agg] last N samples: avg=... min=... max=...
```

If yours stops earlier than that, find the last `[I]` line below and
read the section that matches.

---

## WiFi never connects

```
[I] [wifi] connecting to '...' ...
(...long pause, never "connected"...)
[E] [wifi] connect failed: rc=-2
```

- `rc=-1`: no SSID configured. You forgot `-DWIFI_SSID="..."` on the
  cmake command line.
- `rc=-2`: `cyw43_arch_init` failed. Usually a hardware issue (board
  not seated, no power to the radio).
- Numeric `rc` (positive, from cyw43): association/auth failure. Wrong
  password, WPA3-only network (we only support WPA2 PSK), 5 GHz-only
  AP (the CYW43439 is 2.4 GHz only), or signal too weak.

Fixes:

- Reflash with correct `-DWIFI_SSID` and `-DWIFI_PASSWORD`. Special
  characters in the password may need shell escaping at cmake time.
- Move the board closer to the AP / use a known-working 2.4 GHz SSID.

## SNTP times out

```
[I] [time] starting SNTP against pool.ntp.org ...
[E] [time] SNTP sync timeout after 15000 ms (no reply from pool.ntp.org)
[W] [app] continuing without wall-clock time
```

The firmware continues without wall-clock time, but the telemetry task
will skip all inserts (it requires a synced clock for Date-typed `ts`).

Most likely causes:

- Network blocks UDP 123 outbound (corporate networks sometimes do this).
- DNS for `pool.ntp.org` failing (try `cmake -DNTP_SERVER=time.cloudflare.com ..`).
- DHCP didn't hand us a nameserver. Log earlier would say
  `[dns] no nameserver from DHCP`.

Fixes:

- Pick a different NTP server: `cmake -DNTP_SERVER=time.google.com ..`,
  or your local network's NTP server, or an IP-addressed server to skip
  DNS entirely.
- Open UDP 123 outbound on your firewall.

## TLS handshake fails

```
[I] [client] connecting to ...:27017 (TLS)
[E] [xport] altcp_connect: err=-13
[E] [client] connect failed: rc=-3
```

Various sub-cases. Look at the `err=` code (lwIP `err_t`):

- `err=-1` (ERR_MEM): out of memory. Probably need more PBUF_POOL_SIZE.
- `err=-3` (ERR_RTE): no route to host. Network connectivity issue.
- `err=-9` (ERR_ABRT): connection aborted by peer. Could be IP allowlist
  (the most common Atlas-side rejection) or a TLS cert validation
  failure.
- Timeout (no further log lines, then "connect timeout after Nms"):
  the peer didn't respond. Wrong hostname, firewall blocking, peer down.

Fixes:

- Check your Atlas IP allowlist. Add `0.0.0.0/0` while testing if needed.
- Confirm DNS resolution works: log earlier should show the SRV hosts.
- Confirm your network allows outbound TCP 27017.

## "bad auth" / SERVER_REJECTED

```
[E] [auth] saslStart/Continue rejected: code=8000 codeName=AtlasError
    errmsg=bad auth : authentication failed
[E] scram failed: SERVER_REJECTED (auth failed; check username/password/authSource) (rc=-7, Nus)
```

The crypto succeeded (we sent a syntactically valid SCRAM proof) but
Atlas computed a different proof on its side. Three common causes:

### 1. Wrong password

Re-check your URI. If your password contains special characters
(`@`, `/`, `:`, `#`, `&`, `+`, `=`), they must be percent-encoded in
the URI — Atlas's UI does this for you, but if you typed by hand,
check. The driver URL-decodes before SCRAM, so the URI form is what
matters.

### 2. SHA-256 vs SHA-1 mismatch

The auth log line just above shows what mechanism the server reported:

```
[I] [auth] saslSupportedMechs for admin.your-user: [SCRAM-SHA-1]
```

If your user only has SHA-1 stored, the driver picks SHA-1
automatically. If you'd expect SHA-256 but the server says SHA-1 only,
your user was created with explicit `mechanisms: ["SCRAM-SHA-1"]` or
migrated from an old cluster. Recreate the user via the modern Atlas
UI to get both mechanisms.

### 3. Wrong authSource

The default is `admin`. If you've put the user in `$external` (for
LDAP, AWS IAM, etc.), the driver doesn't support those mechanisms.
You'll need a normal SCRAM user in `admin`.

## "not authorized" on insert/update/delete

```
[I] [telemetry] sample=...
[E] [mongo] insert rejected: code=13 codeName=Unauthorized
    errmsg=not authorized on micro_mongodb to execute command {...}
```

Auth succeeded but the user doesn't have write permission on the
database. In Atlas → Database Access → your user → edit, change the
role to `readWriteAnyDatabase` (or `readWrite` on the specific
database).

## "NotWritablePrimary"

```
[E] [mongo] insert rejected: code=10107 codeName=NotWritablePrimary
    errmsg=not primary
```

This shouldn't happen in steady state — the driver's mini-SDAM
follows the primary hint from hello. If you're seeing it, the
primary stepped down mid-operation (replica set re-election). The
next reconnect cycle will find the new primary; the failed op will
need to be retried by your app.

Atlas free-tier clusters re-elect rarely, but it does happen during
patch days.

## Connection drops every cycle

```
[I] [telemetry] sample=...
[E] [telemetry] insert failed: rc=-3
[I] [client] connecting to ...:27017 (TLS)
[I] [client] ready
[I] [telemetry] sample=...
[E] [telemetry] insert failed: rc=-3
... repeating ...
```

`rc=-3` is `MONGO_WIRE_ERR_TRANSPORT`. The connection is dying after
each operation. Likely causes:

- WiFi signal flapping (move closer to AP).
- Atlas idling out the connection. Atlas times out idle connections
  after some interval; if your task sleeps longer than that between
  operations, this is expected — each insert pays the reconnect cost.
- Server-side disconnect for some other reason (maintenance window,
  account suspension).

## time-series collection doesn't get created

```
[I] [client] ready
(...no "created time-series collection" line follows...)
[W] [client] could not create time-series collection micro_mongodb.telemetry --
    if it exists as a regular collection, drop it in Atlas and reboot
[E] [mongo] create timeseries rejected: code=...
```

If the code is `48` (NamespaceExists), a regular collection with the
same name already exists. You can't convert in place — drop it in
Atlas (`db.telemetry.drop()` in mongosh, or the Atlas UI), then reboot.

If the code is something else, the user probably lacks
`createCollection` permission. Grant `dbAdminAnyDatabase` or, more
narrowly, `dbAdmin` on the specific database.

## panic: sys_timeout pool empty

```
sys_timeout: timeout != NULL, pool MEMP_SYS_TIMEOUT is empty
*** PANIC ***
```

lwIP ran out of timer slots. Already mitigated by bumping
`MEMP_NUM_SYS_TIMEOUT` to 16 in `lwipopts.h` (see commit `9b30c9f`).
If you're hitting it on a fork that disabled that bump, raise it
further or trace where timers are leaking.

## panic: Stack overflow

```
*** PANIC ***
Stack overflow in task: tel
```

A FreeRTOS task ran out of stack. By default the demo tasks use 4096
words (16 KB on a 32-bit MCU); if you've added significant local
state or recursion, bump `APP_TASK_STACK_WORDS` in `main.c` or the
per-task stack size in `xTaskCreate` calls.

## Atlas Charts doesn't show data

If the documents are landing in `telemetry` but Atlas Charts says
"no data":

- Make sure the chart's time field is set to `ts`, not `_id`.
- The collection must be **Time Series** (the type label in Browse
  Collections). If it's a regular collection (because you created it
  manually before the firmware did), Atlas Charts can still query it
  but defaults to non-time-aware ordering. Drop and let the firmware
  recreate it.
- The `ts` field must be a BSON Date (type 9), not an int64. Verify
  via Browse Collections: a Date field shows as
  `"2026-05-11T20:32:45.000Z"`, not `1778531565000`.
