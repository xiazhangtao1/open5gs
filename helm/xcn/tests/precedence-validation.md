# QoS precedence validation (2026-09-10)

Runtime: `localhost:5000/xcn-runtime:qos-precedence-0910`, Helm revision 145.
Registry digest: `sha256:b52c0955d98364b23ff1cc3302d0d999a12fee84f107c6dbd4223688dac947fe`.
Environment: local CN + OAI gNB + OAI UE, TUN dataplane (not memif).

## Executed tests

- Built PCF, SMF and unit executable; Meson unit suite passed.
- Unit allocation tests cover per-session isolation, reserved/default values,
  occupied values, deletion, DB rules, exhaustion and reuse.
- `verify_dedicated_precedence.py` against the deployed PCF passed:
  sequential auto allocation 100/101, existing rule retention, duplicate HTTP 409,
  invalid input HTTP 400, failed PCF creation cleanup, concurrent allocation
  102/103, deletion/reuse of 100, and cleanup of test applications.
- Restarted the test UE for an independent two-flow test with 5QI 3 and 4.
  `xcnctl show rate --level bearer --supi imsi-460110000000100 --json`
  showed QFI 1, 2 and 3 together, including a second observation 56 seconds later.
- Captured NAS QoS rules contained QRI 2 / QFI 2 / precedence 100 and
  QRI 3 / QFI 3 / precedence 101. There was no 32-to-8-bit mapping collision.
- gNB logged three QoS flows and created DRB 2 and DRB 3; UE logged both DRBs added.

## Limits

The OAI UE used here does not provide full NAS modification completion/rejection
coverage. This is not a validation of a commercial UE's cause-83 behavior, nor
of the other server's memif deployment or dedicated-flow throughput.
Different-session isolation was exercised by unit tests, not two live UEs.
Failed creation testing covers PCF error cleanup, not asynchronous UE-rejection
rollback. The XCN endpoint has no new PATCH operation; POST remains creation.

## Repeat the API test

Use an online, idle test PDU session without existing dedicated applications.
The script creates and deletes its own test application IDs.

```bash
python3 helm/xcn/tests/verify_dedicated_precedence.py \
  --url http://10.2.0.119:30777 \
  --supi imsi-460110000000100 --psi 2
```
