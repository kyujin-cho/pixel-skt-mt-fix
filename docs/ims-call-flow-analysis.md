# Why incoming SKT VoLTE calls drop on Pixel, and how Samsung avoids it

On SK Telecom (SKT, Korea), an incoming VoLTE call to a Pixel rings normally and then drops within half a second of being answered. Outgoing calls work. A Samsung Galaxy on the same network, called from the same caller, works.

In short: when the incoming INVITE advertises `Supported: 100rel`, the Pixel's IMS stack always sends its SDP answer early, in a reliable `183 Session Progress`, and then sends a final `200 OK` with no SDP. SKT's network rejects that 200 OK and tears the call down with `Reason: SIP;cause=200;text="Received reject SDP"`. Samsung's stack, given the same INVITE, sends a plain `180 Ringing` with no SDP and puts the answer in the `200 OK`, which SKT accepts. On the Pixel the choice is hardcoded in `libimsstack.so`; no carrier-config key reaches it.

This repository's KernelSU module changes that one decision in the running IMS process (see [The fix](#the-fix)).

## Contents

- [Environment](#environment)
- [The failure on the wire](#the-failure-on-the-wire)
- [The working call on the wire (Samsung)](#the-working-call-on-the-wire-samsung)
- [Side-by-side](#side-by-side)
- [Pixel: why it answers in a reliable 183](#pixel-why-it-answers-in-a-reliable-183)
- [Samsung: why it answers in the 200 OK](#samsung-why-it-answers-in-the-200-ok)
- [Why SKT drops the Pixel's call](#why-skt-drops-the-pixels-call)
- [What was ruled out](#what-was-ruled-out)
- [The fix](#the-fix)
- [Other defects found along the way](#other-defects-found-along-the-way)
- [Method](#method)
- [Limits of this analysis](#limits-of-this-analysis)

## Environment

| | Pixel | Samsung |
|---|---|---|
| Device | Pixel 11 Pro XL (`kodiak`) | Galaxy Z Fold8 (`SM-F971N`, Korean model) |
| Build | Android 17, `CD1A.261005.003` | Android 17, `CP2A.260605.016` (`F971NKSS2AZH7`) |
| Modem | MediaTek, baseband `a900a-MP_260716-260716-M-15880348` | Qualcomm, baseband `F971NKSS2AZH5` |
| IMS stack | `com.android.imsstack` (Java) + `/system_ext/lib64/libimsstack.so` (native SIP/IMS core) | `com.sec.imsservice` (Java) + `/system/lib64/libsec-ims.so` (native SIP stack) |
| SKT SIM | eSIM, MCC/MNC 450-05, carrier ID 1891 | SIM, MCC/MNC 450-05 |
| VoLTE on SKT | Not officially supported; enabled through carrier-config overrides (Pixel IMS) | Native |

`libimsstack.so` SHA-256: `ec66ef900b4840e6a550ae3775e5d28c8cd83d6283655ec7bfdea21967891a06`. All Pixel addresses below refer to this build.

## The failure on the wire

Pixel, incoming call from an iPhone on SKT (captured on the phone with `tcpdump`; SKT's SIP to this Pixel is plain UDP, no IPsec):

```
SKT  →  Pixel   INVITE            Supported: 100rel,path,replaces,timer   SDP offer
Pixel → SKT     183 Session Progress   Require: 100rel   SDP answer
SKT  →  Pixel   PRACK
Pixel → SKT     200 OK (PRACK)
Pixel → SKT     180 Ringing            (no SDP)
                ... user answers ...
Pixel → SKT     200 OK (INVITE)        Content-Length: 0
SKT  →  Pixel   ACK
SKT  →  Pixel   BYE   Reason: SIP;cause=200;text="Received reject SDP"
```

The BYE arrives in the same instant as the ACK, about 0.35 s after the 200 OK. Android records the call end as `CODE_USER_TERMINATED_BY_REMOTE` (510).

SKT's offer (identical across every capture):

```
m=audio <port> RTP/AVP 110 100 98 103 101
b=AS:42  b=RS:625  b=RR:1875
a=rtpmap:110 EVS/16000      a=fmtp:110 br=9.6-24.4; bw=nb-swb; max-red=220
a=rtpmap:100 AMR-WB/16000   a=fmtp:100 octet-align=1; mode-change-capability=2
a=rtpmap:98 AMR/8000        a=fmtp:98 octet-align=1; mode-change-capability=2
a=rtpmap:103 telephone-event/16000
a=rtpmap:101 telephone-event/8000
a=ptime:20  a=maxptime:120  a=sendrecv
```

There are no QoS precondition attributes (`a=curr`/`a=des`) and no ICE candidates in the offer.

The Pixel's answer (in the 183):

```
m=audio 7012 RTP/AVP 110 103
a=rtpmap:110 EVS/16000      a=fmtp:110 max-red=220;br=9.6-24.4;bw=nb-swb
a=rtpmap:103 telephone-event/16000
a=sendrecv  a=ptime:20  a=maxptime:240
a=candidate:1, 1, UDP, 1119400811, 10.3.210.77, 7010, typ, host
```

The answer is a valid selection from the offer. The odd `a=candidate` line is a separate defect ([below](#other-defects-found-along-the-way)), and removing it does not fix the drop.

## The working call on the wire (Samsung)

Same caller, same network, captured on the Fold (rooted). The Fold's SIP to SKT runs over IPv6 inside IPsec ESP; it was decrypted with the phone's own security associations (`ip xfrm state`).

```
SKT  →  Samsung INVITE            Supported: 100rel,path,replaces,timer   SDP offer (same as above)
Samsung → SKT   180 Ringing        Supported: timer, 100rel   Content-Length: 0
                ... user answers ...
Samsung → SKT   200 OK (INVITE)    Content-Length: 434   SDP answer
SKT  →  Samsung ACK
                ... call proceeds ...
```

Samsung's answer in the 200 OK:

```
m=audio 7010 RTP/AVP 110 103
b=AS:50  b=RS:0  b=RR:800
a=rtpmap:110 EVS/16000      a=fmtp:110 bw=nb-swb;br=9.6-24.4;max-red=220
a=rtpmap:103 telephone-event/16000
a=ptime:20  a=maxptime:120  a=sendrecv
a=candidate:1 1 UDP 2130706431 <own IPv6 address> 7010 typ host
```

## Side-by-side

| | Samsung (works) | Pixel (drops) |
|---|---|---|
| First response to INVITE | `180 Ringing`, unreliable, no SDP | `183 Session Progress`, reliable (`Require: 100rel`), SDP answer |
| PRACK exchange | None | Yes |
| `180 Ringing` | The only provisional response | Sent after the PRACK, no SDP |
| SDP answer location | **200 OK** | **183** |
| 200 OK body | SDP (434 bytes) | Empty (`Content-Length: 0`) |
| Codec chosen | EVS | EVS (AMR-WB also tested; same result) |
| `maxptime` | 120 (copies the offer) | 240 |
| `a=candidate` | Well formed, own address and port | Malformed template (commas, foreign IP, wrong port) |
| Transport to SKT | IPv6, IPsec ESP | IPv4, plain UDP |
| User-Agent | `TTA-VoLTE/4.0 SM-F971N/...` | `TTA-VoLTE/3.0 ...` (spoofed or template; both tested) |
| Result | Call proceeds | BYE `cause=200 "Received reject SDP"` |

Of these differences, only the SDP answer location was not ruled out by testing ([What was ruled out](#what-was-ruled-out)), and changing only that, with the module in this repository, fixes the call.

## Pixel: why it answers in a reliable 183

Static analysis of `libimsstack.so` (AArch64, exported C++ symbols) with `llvm-objdump`.

### The decision: `IdleState::OnAttached()`

When an incoming INVITE is attached to a new call, `IdleState::OnAttached()` (0xa836c0) handles the received SDP (`MtcCallState::HandleReceivedSdp`), sets up the media session, and then makes exactly one decision:

```
a83abc: adrp  x1, 0xc63000
a83ac0: ldr   x1, [x1, #0x2d0]          ; &MtcExtensionSet::OPTION_TAG_RPR  ("100rel")
a83ac4: bl    MtcExtensionSet::IsAvailableOnBoth(AString const&)
a83ac8: tbz   w0, #0, 0xa83bac          ; not available -> MtcCallState::OnReadyToAlert()  (plain 180 path)
a83acc: ...                             ; available:
a83ad8: ldr   x8, [x22, #0x28]          ;   MtcSession::SendProvisionalResponse
a83ae0: mov   w1, wzr                   ;   is180 = false  -> 183
a83ae4: mov   w2, #0x1                  ;   reliable = true
a83aec: blr   x8
        ...   StartTimer(8)             ;   then on to IncomingState, waiting for PRACK
```

Between the SDP handling and this branch there are no carrier-config reads, no carrier-ID checks and no call-type or QoS checks.

### Both inputs to "available on both" are fixed

- **Local side.** `MtcSession::GetSupportedExtensions()` (0xa6fed0) builds the phone's own extension list. It adds `100rel` unconditionally (0xa70464); the only conditional entry is the precondition tag, gated by `MtcSession::IsPreconditionSupportedInLocal()`.
- **Remote side.** `MtcExtension::UpdateFromRequireAndSupportedHeader()` records a tag as available when it appears in the INVITE's `Require` or `Supported` header, and `MtcExtension::IsAvailableOnRemote()` (0xa792b0) just returns that. No config is read on this path, so `Supported: 100rel` from SKT always counts.

So for any network that advertises `100rel`, the Pixel takes the reliable-183 branch.

### A reliable provisional response always carries the answer

`MtcSession::SendProvisionalResponse(bool is180, bool reliable)` (0xa722b0) picks 180 or 183 with a `csel`. If `reliable` is set it calls `SetSdpToSend(0,0,0)` and attaches the SDP; an unreliable response never carries SDP. Reliable responses go to `SessionEx::SendRpr` (0x76d820, log `RPR: SENDING 183 "Session Progress"`); unreliable ones go to `Session::SendProvisionalResponse` (0x6e37f0, log `PR: SENDING 180 "Ringing"`).

### The 200 OK has nothing left to send

On answer, `MtcSession::Accept()` (0xa73680) calls `SetSdpToSend(false, false, false)` with constant arguments. `MtcSession::SetSdpToSend()` (0xa75550) checks the offer/answer state from `MtcMediaManager::GetNegotiationState()`:

- `STATE_OFFER_SENT`: logs `SetSdpToSend - already STATE_OFFER_SENT`, attaches nothing.
- negotiation complete (the answer already went out in the 183): logs `SetSdpToSend - nothing to update`, attaches nothing.
- otherwise: forms the SDP and attaches it.

`Session::CheckNSetSdpBodyPart()` (0x6c6ae0) only adds a body when the step above produced one. There is no "repeat the last answer in the 2xx" branch, and no configuration is read on this path (the only config call in the accept path is the session-timer check `SipConfigProxy::IsSessionTimerUpdateRequiredByReInvite`).

### What the nearby carrier-config keys actually do

These keys look relevant but do not reach the 183 decision or the 200 OK body:

| Key (SKT value) | Read in | Effect |
|---|---|---|
| `imsvoice.prack_supported_for_18x_bool` (false) | `MtcCallState::IsRprRequired()` | Whether the later 180 is reliable |
| `imsvoice.require_prack_for_alert_bool` (false) | `MtcCallState::OnReadyToAlert()` | Whether the 180 waits for a PRACK |
| `imsvoice.force_183_before_alerting_on_non_100rel_invite_bool` (false) | `MtcSession::Send183BeforeAlertingForNon100rel()` | Only when 100rel is *not* available; sends a 183 without SDP |
| `ims.sdp_negotiation_required_for_non_rpr_bool` (true) | `SipConfigProxy::IsSdpNegotiationRequiredForNonRpr()` | Tested false: no change |
| `imsvoice.wait_qos_for_incoming_invite_without_precondition_bool` (true) | | Tested false: no change |
| `imsvoice.policy_for_sdp_preview_mode_int` (3) | | Tested 0, 1, 2: no change |
| `imsvoice.sdp_answer_full_capability_bool` (false) | | Tested true: no change |

The Pixel IMS stack keeps its effective per-SIM configuration, including vendor keys that `dumpsys carrier_config` does not show, in `/data/user_de/0/com.android.imsstack/files/carrier_config_slot<N>_<iccid>_<carrierId>.xml`. Its built-in SKT profile is a full, SKT-specific profile, not a generic default.

## Samsung: why it answers in the 200 OK

Static analysis of `libsec-ims.so` (AArch64, exported symbols) and the decompiled `imsservice.apk` (jadx). Field offsets were mapped from the `ImsUserProfile` constructor's own log strings.

### The Java layer only passes flags down

`ResipRegistrationManager` copies the per-carrier profile into `UaProfile`, and `RegistrationRequestBuilder` sends it to the native stack, including:

- `setUseProvisionalResponse100rel`
- `setUse183OnProgressIncoming`
- `setSupport183ForIr92v9Precondition`
- `setSend18xReliably`
- `setUse200offerWhenRemoteNotSupport100rel`
- `setPrecondEnabled`

The incoming-call request (`ResipVolteHandler.proceedIncomingCall` → `UserAgent.progressIncomingCall`) carries only the session ID and extra headers. Java does not choose between 180 and 183.

### The relevant profile fields, and SKT's values

The "SKT VoLTE" profile (`mnoname` `SKT_KR`) in `res/raw/imsprofile.json` sets none of these, so SKT gets the schema defaults:

| Profile field | Default (SKT) | Native use |
|---|---|---|
| `use_183_on_progress_incoming` | false | `CallUasSession::OnProgressIncomingCall` |
| `use_provisional_response_100rel` | false | 180/183 reliability |
| `send_18x_reliable` | false | `Is180PrackRequiredByPolicy` |
| `use_precondition` | false | precondition branch |
| `support_183_for_ir92v9_precondition` | true | precondition branch only |
| `use_200offer_when_remote_not_support_100rel` | true | only when the INVITE has no SDP |

### The decision: `CallUasSession::OnProgressIncomingCall()` (0x5404dc)

Samsung sends a 183 for an incoming call only if one of these holds:

- precondition is enabled in the profile and the INVITE offers or requires it;
- `use_183_on_progress_incoming` is set and the INVITE has 100rel in `Supported` or `Require`;
- the INVITE has no SDP (delayed offer) and the delayed-offer flags call for it;
- one of several carrier-specific branches applies (DCM, KDDI, ATT, Sprint, RWC, DTM; VZW has its own handling).

SKT matches none, so the code falls through to `SendInviteResp(180, …, sdp=null)`.

### Why that 180 is unreliable and empty

In `CallUasSession::SendInviteResp()` (0x538c38), the 180 is reliable only if the INVITE has 100rel in `Require`, or `use_provisional_response_100rel` is set, or `Is180PrackRequiredByPolicy()` is true (`send_18x_reliable` and remote 100rel). For SKT all three are false. The 180 carries SDP only for a delayed-offer INVITE.

### Why the answer lands in the 200 OK

The `CallUasSession` constructor sets an "offer received, not yet answered" flag when the INVITE has SDP. Any 18x that carries SDP clears it. In `SendInviteResp(200)`, if the INVITE had a body and that flag is still set, the stack calls `SdpCreator::CreateSdpAnswer` and attaches the answer to the 200 OK. Since no reliable 18x carried the answer, it lands in the 200 OK.

SKT gets no special handling in this path. The only SKT branch nearby is an unrelated header (`P-SKT-CC-Info`, video conference). Samsung's behaviour on SKT comes from its defaults.

### The difference in one line each

- **Pixel:** "The remote supports 100rel, so use it": an early answer in a reliable 183. Hardcoded.
- **Samsung:** "Use 100rel only if the remote requires it or the carrier profile asks for it": a plain 180 and the answer in the 200 OK. A profile option, off for SKT.

## Why SKT drops the Pixel's call

Both flows are valid by the SIP standards. Under RFC 3261 and RFC 3262, an answer delivered in a reliable provisional response completes the offer/answer exchange, and the 2xx does not need to repeat it. SKT's network is stricter: its call server (the `MSS`/`ABCM` elements in the Contact and Call-ID values) releases the call when the final 200 OK carries no SDP, with the reason text `Received reject SDP`.

Supporting evidence:

- On the Pixel's outgoing calls, SKT's own 200 OK repeats the SDP (307 bytes) even though it already sent an answer in its 183. SKT's network does this itself.
- Samsung's 200 OK always carries the answer.
- With the module in this repository applied, the Pixel sends `180 Ringing` and then `200 OK` with SDP (375 bytes). Calls then proceed normally with two-way audio, and nothing else changes.

What SKT's call server checks internally is not visible from the handset. The behaviour above is what the captures show.

## What was ruled out

Each was tested with a real incoming call from the same caller, with packet capture.

| Hypothesis | Test | Result |
|---|---|---|
| EVS codec choice | Codec list limited to AMR-WB/AMR, octet-aligned, matching SKT's payload types; the Pixel answered AMR-WB | Still dropped |
| EVS unsupported | EVS removed with no other change | Pixel rejected the call with `488 Not Acceptable Here` (SKT's offer has no AMR-WB the stack accepts without that profile), so EVS must stay |
| Malformed `a=candidate` | `imsvoice.audio_candidate_attribute_string_array` set empty; the line disappeared from the answer | Still dropped; the working outgoing call also carries this line |
| No dedicated voice bearer (QCI 1) | Compared with the outgoing call | The outgoing call has none either and works |
| Audio path | Media HAL logs compared between incoming and outgoing calls | Identical: modem-offloaded, `onFirstMediaPacketReceived` at answer |
| QoS preconditions | SKT's offer carries none; the Pixel's SKT profile disables them | Not a factor |
| SIP transport/MTU | `ims.ipv4/ipv6_sip_mtu_size_cellular_int` = 4000 | Still dropped; incoming SIP is UDP either way |
| User-Agent | Spoofed LG string and the stack's own template | Still dropped with both |
| The SDP-related keys in the table above | Each tested | No change in signalling |
| IPsec (Samsung uses it) | `ims.sip_over_ipsec_enabled_bool` = true | SKT accepts sec-agree in the 401, but never answers the protected REGISTER over IPv4 (`REG_RESP_NETWORK_TIMEOUT`). Explains why the Pixel's SKT profile disables IPsec. Not testable for calls. |

## The fix

The module in this repository changes one instruction, in memory only, in the running `com.android.imsstack` process:

```
libimsstack.so + 0xa83ac8
  before: 20 07 00 36   tbz  w0, #0, 0xa83bac
  after:  39 00 00 14   b    0xa83bac
```

With the branch always taken, `IdleState::OnAttached()` goes straight to `MtcCallState::OnReadyToAlert()`, the same path it takes for an INVITE without 100rel:

- The phone sends a plain, unreliable `180 Ringing` with no SDP.
- The offer stays unanswered until the user answers.
- `MtcSession::Accept()` → `SetSdpToSend()` then forms the answer, and it goes into the 200 OK.

This matches Samsung's flow.

Scope and safety:

- **Only this call site changes.** `IsAvailableOnBoth` itself is untouched, so other uses of 100rel (for example PRACK handling on outgoing calls) behave as before.
- **It applies to every SIM.** The stack serves all subscriptions from one process, so incoming calls on other carriers also switch to "180, then answer in the 200 OK". That is ordinary SIP behaviour, but it has not been tested on every carrier.
- **Nothing on disk changes.** A watcher started by KernelSU writes the patch through `/proc/<pid>/mem` and re-applies it whenever the IMS process restarts.
- **The site is found, not hard-coded** (since v0.2.0). The `bin/imspatch` helper (`src/imspatch.c`):
  - opens the mapped `libimsstack.so` through `/proc/<pid>/root`;
  - looks up `IdleState::OnAttached`, `MtcExtensionSet::IsAvailableOnBoth` and `MtcExtensionSet::OPTION_TAG_RPR` in its dynamic symbol table;
  - scans the function's code in the file for `bl` followed by `tbz w0, #0, <target>`.

  Before writing anything it checks, in the live process, that:
  - the `bl` reaches `IsAvailableOnBoth`, directly or through a PLT stub whose GOT slot holds that function's runtime address;
  - within the four instructions before the `bl`, an `adrp`/`ldr x1` pair loads a GOT slot holding the runtime address of `OPTION_TAG_RPR`;
  - exactly one site matches;
  - the word in memory is either the original `tbz` from the file or the patched `b`.

  Any mismatch means nothing is written. This lets the module survive firmware updates as long as Google keeps these symbols exported and the function's shape intact. The first version (v0.1.0) was pinned to the library's SHA-256 and a fixed offset.
- **It is reversible at runtime.** The KernelSU **Action** button toggles the patch on the running process, and uninstalling the module writes the original instruction back.

An alternative that rewrites the incoming INVITE instead of patching the stack is recorded as an open design in [design-sip-rewrite.md](design-sip-rewrite.md).

The proper fix belongs in Google's IMS stack. Either don't use a reliable 183 for the answer when the remote only *supports* 100rel (as Samsung does by default), or repeat the SDP answer in the 200 OK, at least for SKT.

## Other defects found along the way

These don't cause the drop, but they are wrong in the Pixel's SKT profile:

- **Malformed ICE candidate.** `imsvoice.audio_candidate_attribute_string_array` is the literal template `1, UDP, 1119400811, 10.3.210.77, 7010, typ, host`. It is inserted verbatim: comma-separated, a fixed foreign IP address, and a port that doesn't match the media port. Samsung generates `candidate:1 1 UDP 2130706431 <own address> <media port> typ host`.
- **User-Agent template.** The SKT template `TTA-VoLTE/3.0 #MODEL#/#BUILD# Device-Type/#TYPE# #OPERATOR#` is sent with `#OPERATOR#` replaced by the literal word `OPERATOR`.
- **`maxptime`.** The Pixel answers 240 when the offer says 120. Samsung copies 120.
- **IPsec.** The SKT profile disables IPsec, and enabling it fails registration over the Pixel's IPv4 IMS connection. Samsung registers with IPsec over IPv6.

## Method

- **Signalling.** Captured on each rooted phone with `tcpdump`, including IP fragments, because SKT's INVITE exceeds one packet. The Pixel's SKT SIP is plaintext UDP. Samsung's is IPsec ESP and was decrypted in Wireshark with the security associations read from `ip xfrm state` on the phone.
- **Logs.** Full `logcat -b all` alongside each capture. The Pixel IMS stack logs SIP start lines but redacts bodies, so the captures were needed for SDP.
- **Configuration experiments.** `ICarrierConfigLoader.overrideConfig(subId, bundle, persistent=false)` called as root through `app_process`, then confirmed in the stack's own per-slot config XML before each test call.
- **Static analysis.**
  - Pixel: `libimsstack.so` with `llvm-objdump`, symbols and relocations resolved.
  - Samsung: `libsec-ims.so` the same way, plus `imsservice.apk` decompiled with jadx.
  - The Pixel decision branch at 0xa83ac4/0xa83ac8 was checked against the live process before patching.

## Limits of this analysis

- **SKT's side is inferred.** Why SKT's call server rejects a 200 OK without SDP is inferred from its behaviour. Its implementation was not visible.
- **Samsung's profile getters were not inspected.** They live in the Samsung framework outside `imsservice.apk`; they were assumed to be plain JSON reads with the schema defaults.
- **One build analysed.** The Pixel analysis was done on the single `libimsstack.so` build named above. Addresses will differ in other builds. The module locates the site by symbol and pattern, but it has only been run against this build.
- **Other Pixel models.** The fix only applies to phones whose IMS stack is `com.android.imsstack` with `libimsstack.so`. Other Pixel generations may ship a different IMS stack, which would need its own analysis.
- **Narrow testing.** The patched flow was verified with incoming calls on SKT from one caller (an iPhone on SKT). Incoming calls on other carriers in the same phone, and callers from other Korean networks, were not tested.
