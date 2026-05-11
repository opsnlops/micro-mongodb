# Security notes

This document captures the driver's threat model, what it does protect
against, and where the known gaps are. Most of this was settled during
a deliberate adversarial security review pass; see commits `029d543`
(Criticals), `1b984bd` (Highs), `413c9ca` (Mediums), and `95e6bb0`
(Lows) for the specific changes.

## What's protected

**End-to-end TLS to Atlas.** The driver does a real TLS 1.2 handshake
against Atlas's cert chain, including X.509 verification against the
embedded ISRG Root X1 (Let's Encrypt). SNI is set explicitly. mbedTLS
rejects an invalid chain — there's no "skip verify" flag.

**Credentials in the URI are URL-decoded** before reaching SCRAM, so
passwords containing `@`, `/`, `:`, etc. (which Atlas's UI percent-encodes)
work correctly.

**SCRAM proofs are computed correctly.** The implementation was
cross-checked against a reference Python implementation byte-for-byte
when we debugged the SHA-1 vs SHA-256 mech-selection bug; salted_password,
client_key, stored_key, client_signature, and client_proof all match.

**SCRAM server signature is verified in constant time.** The final
`v=` server proof check uses a constant-time `memcmp` to avoid leaking
timing information about the password-derived `server_key`.

**SCRAM mechanism is auto-selected** based on the server's
`saslSupportedMechs` for the specific user. We prefer SCRAM-SHA-256
and only fall back to SHA-1 if the user account doesn't have SHA-256
credentials stored.

**Bounded server-controlled work.** PBKDF2 iteration count is clamped
to `[4096, 600000]` so a malicious server can't request 2³¹ iterations
and peg the M0+ for days. Reply size is capped at 16 MB.

**BSON from the wire is validated** structurally (`bson_validate`)
before libbson's iterator gets to walk it, closing the surface area
to libbson's wire-parser bugs.

**SCRAM intermediates are zeroized** on the success path —
`salted_password`, `client_key`, `stored_key`, `server_key`,
`client_signature`, `client_proof`, the prepped password, and the
nonce raw bytes all get wiped before the SCRAM function returns OK.

**SNTP errors don't cascade.** If NTP fails (server unreachable,
network blip), the firmware logs a warning and continues with
monotonic-clock-only operation rather than refusing to run.

## What's not protected (known gaps)

### Non-ASCII passwords (SASLprep punt)

The SCRAM-SHA-256 RFC requires that the password be processed through
RFC 7613's OpaqueString profile (a stringprep variant). For pure ASCII
passwords this is a no-op, but proper SASLprep would need ~100 KB of
Unicode normalization tables — more than the entire driver. We reject
non-ASCII passwords up-front with `MONGO_AUTH_ERR_ASCII_ONLY` and a
clear error log.

Don't use non-ASCII characters in your Atlas user passwords.

### Error-path zeroization (H5 partial)

The SCRAM intermediates are wiped on the *success* return path. Error
paths between PBKDF2 and return don't currently zeroize — if the
function bails out (e.g. server returns an unexpected reply), salted-
password-derived material may be left on the stack until that memory
is reused.

To fix properly would require a full `goto cleanup;` refactor of the
~500-line SCRAM function. On the to-do list; low practical risk on a
device with no MMU / no swap / no remote-attach debug surface.

### libbson allocation failure paths

`bson_append_*` calls in our CRUD/cursor helpers don't all check return
values. A failure would result in malformed BSON sent to the server,
which the server would reject — locally we'd see a `MONGO_WIRE_ERR_BSON`
or `MONGO_WIRE_ERR_PROTOCOL` and recover. No remote exploit surface,
just slightly fragile error reporting.

### DNS trust

`mongodb+srv://` resolves SRV and TXT records over plain UDP DNS
against whatever resolver lwIP got from DHCP. An attacker who can
spoof DNS replies can:

- Redirect `replicaSet` to an arbitrary string (mostly cosmetic — we
  don't enforce it).
- Change `authSource` (real impact: SCRAM proofs go to a different
  database — though TLS+SNI still pins which server we actually talk to).
- Change which hosts the client tries.

The TLS handshake verifies the cert chain, so even with poisoned DNS
the connection still lands on a server with a valid Atlas-issued cert.
But the SCRAM exchange itself can be steered to the wrong database.

**Mitigation:** if your deployment's threat model includes hostile DNS,
either use a trusted resolver (DoT/DoH at the network level) or skip
`mongodb+srv://` and use `mongodb://host1,host2,...` with explicit
host lists.

### Plain TCP SCRAM (`mongodb://` without TLS)

If you connect with `mongodb://` and credentials, the driver logs a
warning:

```
[W] [client] SCRAM over plain TCP -- saslSupportedMechs unauthenticated,
    proofs offline-attackable
```

Specifics: the `saslSupportedMechs` array is unauthenticated, so an
attacker can strip `SCRAM-SHA-256` from it and force a SHA-1 downgrade.
The proof itself isn't a password but it's offline-bruteforceable given
sufficient compute.

**Mitigation:** don't run SCRAM over plain TCP. Either use TLS or use
a local mongod with `--noauth` for development.

### `MONGO_SCRAM_DEBUG` leaks credentials

The driver has a build-time flag `-DMONGO_SCRAM_DEBUG=1` that dumps
every SCRAM intermediate (`salted_password`, `client_key`, the literal
`auth_message`, etc.) to the log. This material is sufficient to
impersonate the user — anyone who reads the log has effective control
of the account.

The flag defaults to **off**. Only turn it on when debugging an auth
failure. The flag was the very first thing fixed in the security
review (C1) for a reason.

### Single-connection client

`mongo_client_t` keeps one TCP/TLS connection at a time. If the
connection drops mid-cycle, the next CRUD call reconnects from scratch
(another TLS handshake + SCRAM). There's no fast-resumption mechanism;
this is acceptable for telemetry-rate workloads but not for high-RPS
applications.

### No mTLS / X.509 client cert auth

The driver does server cert verification but doesn't present a client
cert. MongoDB's X.509 auth mechanism (which uses the cert subject DN
as the user identity) isn't implemented. SCRAM is the only supported
auth path.

For most embedded telemetry use cases, baking a per-device SCRAM
password into firmware is fine. For higher-stakes deployments,
X.509 mutual auth would be safer.

### No `lsid` / `$clusterTime` / sessions

The driver doesn't track logical session IDs or cluster time. Each
command is independent. For replica-set deployments this means no
causal consistency guarantees: a `find` immediately after an `insert`
isn't guaranteed to see the inserted document if it routes to a
secondary (the driver currently always targets the primary, so in
practice this doesn't bite — but it's not an *enforced* invariant).

## Reporting issues

Open an issue on the repo. For credential exposure or anything that
looks like remote code execution, please don't include the actual
exploit material in the public issue — email the maintainer first.
