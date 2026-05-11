# Atlas setup

What you need on the Atlas side before the Pico will talk to it. The
short version of all this lives in [getting-started.md](getting-started.md);
this is the detailed version with the surprises called out.

## 1. Create a cluster

Free-tier M0 is fine. Anything bigger works too.

Make sure your cluster is running MongoDB **4.0 or newer** — that's when
SCRAM-SHA-256 became the default mechanism and OP_MSG became the only
wire protocol. The driver should work against 3.6 (OP_MSG was introduced
there) but only via SCRAM-SHA-1.

## 2. Create a database user

**Database Access** → **Add new database user.**

### Username / password

- Pick anything. The username goes into the URI between `://` and `@`.
- Password: alphanumeric works without any escaping. If you use special
  characters (`@`, `:`, `/`, `#`, `&`, `?`, `+`, `=`), the URI will
  percent-encode them. The driver URL-decodes before SCRAM, so this works,
  but it's easier to just use alphanumeric for the demo.
- **No** non-ASCII characters in the password — we punt SASLprep (full
  RFC 7613 OpaqueString is ~100 KB of Unicode normalization tables).

### Role

Critical: **the user needs write permission on whatever database the
demo writes to.** The default is `micro_mongodb`. Three reasonable options:

| Role | Scope | Notes |
|---|---|---|
| `readWriteAnyDatabase` | all DBs | Simplest. What the demo expects. |
| `readWrite` + database `micro_mongodb` | one DB | Tighter. Edit the demo's `TELEMETRY_DB` constant if you use a different name. |
| `atlasAdmin` | everything | Way more than needed; only useful if the user will also be running admin commands. |

If you give the user `read`-only or `atlasReadAnyDatabase`, you'll see this
in the log on first insert:

```
[E] [mongo] insert rejected: code=13 codeName=Unauthorized
    errmsg=not authorized on micro_mongodb to execute command {...}
```

That's not a driver bug — it's Atlas correctly enforcing the role.

### Auth mechanism (the SCRAM-SHA-1 footgun)

Atlas's UI normally creates users with both `SCRAM-SHA-256` and `SCRAM-SHA-1`
credentials, so either one works. **However:** users created via the legacy
mongo shell with `db.createUser({..., mechanisms: ["SCRAM-SHA-1"]})`, or
users migrated from older Atlas clusters, may only have SCRAM-SHA-1 stored.

The driver handles this automatically — it asks the server which mechanism
your user supports (`saslSupportedMechs` in the hello reply) and uses
whichever is available. If it falls back to SHA-1, you'll see:

```
[W] [auth] server only stores SCRAM-SHA-1 for this user; using legacy mechanism
```

To force SCRAM-SHA-256, delete and recreate the user via the Atlas UI on a
modern cluster — new users get both.

## 3. Allowlist your IP

**Network Access** → **Add IP Address.**

The Pico's outbound traffic comes from your network's NAT (your home or
office router's egress IP), not from the Pico itself. Whatever IP your
laptop appears as from the public internet (try `curl ifconfig.me`) is
the same IP your Pico will appear as.

For testing: `0.0.0.0/0` (everywhere) works but obviously don't ship that.

If the IP isn't allowlisted, the Pico will get a TLS connection refused
or timeout at handshake time. Log will look like:

```
[E] [xport] altcp_connect: err=-N
[E] [client] connect failed: rc=-3
```

## 4. Get the connection string

**Database** → **Connect** → **Drivers** (you can pick any driver — the
connection string format is the same).

You'll get something like:

```
mongodb+srv://your-user:<password>@your-cluster.ata6b.mongodb.net/?retryWrites=true&w=majority&appName=Cluster0
```

Substitute `<password>` with the actual password. For the Pico:

```bash
cmake -DMONGO_URI="mongodb+srv://your-user:your-pass@your-cluster.ata6b.mongodb.net/" ..
```

You can drop the `retryWrites=true&w=majority&appName=...` query string —
the driver ignores `retryWrites` (we don't implement it), the default
write concern is fine, and `appName` is overridden by what you pass to
`mongo_client_config_t.app_name`.

## 5. (Optional) Pre-create the time-series collection

The demo's `mongo_client_ensure_timeseries` will create the collection on
first boot. If you'd rather create it yourself (or want to set
`bucketMaxSpanSeconds` / other options not exposed by the simple
`mongo_timeseries_opts_t`):

```js
// In Atlas Data Explorer or mongosh:
db.createCollection("telemetry", {
  timeseries: {
    timeField: "ts",
    metaField: "board",
    granularity: "seconds"
  }
})
```

The driver's `mongo_client_ensure_timeseries` then becomes a no-op
(`NamespaceExists` code 48 is silently treated as success).

## 6. Verify

Once the firmware is flashed and running, in Atlas:

- **Browse Collections** → `micro_mongodb` → `telemetry` should show
  documents with `ts`, `board`, `value` fields. The collection type
  shown in the UI should be **Time Series**, not the generic
  **Collection**.
- **Atlas Charts** → use `ts` as the X axis (time), `value` as Y, and
  group by `board` for a free line chart. Telemetry from `pico_w` and
  `pico2_w` show up as separate series.

## Connection-string options the driver understands

The URI parser handles a small allow-list of query-string options;
unrecognised ones are ignored:

| Option | Effect |
|---|---|
| `tls=true` / `false` | Force TLS on/off. `mongodb+srv://` implies `true`. |
| `ssl=true` / `false` | Alias for `tls`. |
| `replicaSet=NAME` | Pinned replica set name. Mostly informational; the driver follows primary hints regardless. |
| `authSource=DB` | Database to authenticate against. Default `admin`. Atlas TXT records may set this. |

Anything else (`retryWrites`, `w`, `appName`, `readPreference`, etc.) is
quietly dropped.
