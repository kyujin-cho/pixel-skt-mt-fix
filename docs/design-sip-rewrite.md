# Open design: fix the call flow by rewriting the incoming INVITE

Status: **open, not implemented.** The shipped module patches the IMS process instead (see [The fix](ims-call-flow-analysis.md#the-fix)). This document records an alternative that doesn't depend on the IMS implementation, for later.

## Idea

The in-memory patch works by making the Pixel's IMS stack act as if the incoming INVITE did not offer `100rel`. The network side can produce the same effect without touching any code: remove `100rel` from the INVITE's `Supported:` header before the IMS stack reads it.

With `100rel` absent, the stack takes its standard path: a plain `180 Ringing` with no SDP, then the SDP answer in the `200 OK`. That is the flow SKT accepts.

```
SKT ── INVITE (Supported: 100rel,path,replaces,timer) ──▶ netfilter ──▶ NFQUEUE daemon
                                                                            │ rewrite to
                                                                            ▼ Supported: path,replaces,timer
                                                         IMS stack ◀── reinjected INVITE
```

## Why it might be preferable

| | In-memory patch (shipped) | INVITE rewrite (this design) |
|---|---|---|
| Depends on IMS stack code | Yes: exported symbols in `libimsstack.so` and the shape of `IdleState::OnAttached()` | No |
| Depends on firmware build | Partly (survives builds while the symbols and code shape last) | No |
| Works on other IMS stacks | No | Yes, if they answer in the 200 OK when 100rel is absent (standard behaviour) |
| Scope | Every SIM on the phone | Can be limited to SKT's IMS traffic only |
| Failure mode | Refuses to patch; the phone behaves as stock | With queue bypass, packets pass unchanged if the daemon is down |

## Kernel prerequisites (checked on Pixel 11 Pro XL, kernel 6.12 GKI)

- `CONFIG_NETFILTER_XT_TARGET_NFQUEUE=y`, `CONFIG_NETFILTER_NETLINK_QUEUE=y` (`nfnetlink_queue` present).
- `NFQUEUE` listed in `/proc/net/ip_tables_targets` and `/proc/net/ip6_tables_targets`.
- `CONFIG_NF_CONNTRACK=y`, `CONFIG_NF_DEFRAG_IPV4=y`, `CONFIG_NF_DEFRAG_IPV6=y`. SKT's INVITE arrives in two IP fragments; with conntrack's defragmentation the queue receives the reassembled datagram.

## Sketch

1. **Find the SKT IMS path.** The interface of the SKT IMS PDN (e.g. `wwanN`) changes on every PDN restart. The daemon has to track it, for example from `ip addr` / netlink link events or from `dumpsys telephony.registry` / `dumpsys connectivity` for the IMS network of SKT's subscription. A simpler first version can match SKT's P-CSCF addresses instead.
2. **Queue only what's needed.** Install a rule in a dedicated chain, hooked as early as possible and before Android's own `INPUT` chains, matching:
   - inbound UDP to the local SIP port (5060) on that interface;
   - optionally only packets whose payload starts with `INVITE `.

   For example:
   `iptables -t mangle -I PREROUTING -i <wwan> -p udp --dport 5060 -m string --algo bm --from 28 --to 36 --string "INVITE " -j NFQUEUE --queue-num <n> --queue-bypass`
   (and the `ip6tables` equivalent).
3. **Rewrite conservatively.** For each queued packet:
   - Only touch SIP **requests** whose method is `INVITE` and that carry an SDP body. Re-INVITEs inside an existing dialog can be left alone: they have a `To` tag, and the problem only concerns the initial INVITE.
   - Remove the `100rel` option tag from `Supported:` (and its compact form `k:`), keeping the other tags. If the header becomes empty, drop it.
   - **Never** touch an INVITE with `Require: 100rel`. The caller then insists on reliable provisional responses, and removing the tag would break the call. Pass those unchanged.
   - Recompute the IP total length, the IP header checksum and the UDP checksum. The SIP `Content-Length` is unaffected, since only headers change.
   - Return the verdict `NF_ACCEPT` with the modified payload. Any parse doubt means `NF_ACCEPT` with the original payload.
4. **Lifecycle.** Run as a KernelSU service. Remove the rules on exit and on module disable or uninstall. Use `--queue-bypass` so a crashed daemon never blocks calls.

The daemon is small enough to be a single static C binary using `libnetfilter_queue`, or raw `nfnetlink` sockets to avoid the dependency.

## Open questions

- **IPsec.** On a phone where the carrier's SIP is protected with IPsec (Samsung on SKT, for example), the inbound packet seen at `PREROUTING` is still ESP. Decrypted transport-mode packets pass through netfilter again after XFRM input, so the rule would have to match the decrypted pass (e.g. `-m policy --dir in --pol ipsec`). The Pixel's SKT profile doesn't use IPsec, so this doesn't matter for the current case.
- **TCP.** SIP over TCP would need stream reassembly and sequence-number fixups, which is a much bigger job. SKT's incoming INVITEs to the Pixel arrive over UDP.
- **SIP integrity mechanisms.** If a network signs or hashes headers (unusual for IMS access), rewriting would break that. Not observed on SKT.
- **Stack behaviour without 100rel.** It must be checked per IMS stack that "no 100rel" really produces "180, then the answer in the 200 OK". This is confirmed by code reading for the Pixel's `libimsstack.so` (`IdleState::OnAttached()` → `MtcCallState::OnReadyToAlert()`, then `MtcSession::Accept()` forms the answer). It's untested on other stacks.
- **Interaction with the shipped patch.** The two approaches are redundant. If both are active, the rewrite simply means the patched branch is never reached. A combined module should pick one.
- **Rule placement.** Android's netd rebuilds its own chains, and the custom chain must survive that. It needs to hook ahead of netd's chains without being reordered by it.
