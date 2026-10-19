# Formal models

ProVerif models of Sharepoint's protocol 2 (#472): the first link (`Noise_XXhfs` with ML-KEM-768,
the nonce commitment and the six-digit code) and the rejoin (`Noise_NNhfs+psk0` keyed by the
resume secret, the tag, `resume_at`). What they show and what they leave out is in
[docs/design/28 "The wire"](../../docs/design/28-sharepoint.md#the-wire).

| File | What it checks |
|---|---|
| `sharepoint.pvl` | The primitives and both handshakes, token by token from `src/addons/sharepoint/noise.cpp` and `channel.cpp` |
| `handshake_code.pv` | An attacker in the middle with a collision oracle on the code cannot get matching codes; after a match both sides share keys and the resume secret, and the files stay secret |
| `handshake_nonce.pv` | The client's nonce is secret until its h2 is fixed (the other half of the ordering argument) |
| `code_order.pv` | Sanity, on the first link's skeleton: the collision oracle wins without the commitment check (ProVerif finds the attack) and loses with it. The full first link without the check does not finish |
| `handshake_x25519_later.pv`, `handshake_mlkem_later.pv` | A recorded first link stays secret when X25519, or ML-KEM-768, is broken later |
| `handshake_both_later.pv` | Sanity: with both broken later, ProVerif reads it |
| `rejoin.pv` | Only a holder of the resume secret gets a rejoin adopted, a replay never is, link keys are fresh per rejoin, and a rejoined link stays secret when the resume secret and one of X25519 / ML-KEM-768 leak later |

Run (ProVerif 2.05; build it from source with OCaml, see docs/DEVELOPMENT.md):

```bash
tools/formal/run.sh
```

`run.sh` runs the models in parallel (one core each) and checks every `RESULT` line against the
expected one, exiting non-zero on a difference; `run.sh rejoin` runs one. All of them take about
5.5 minutes on an M-series Mac (`handshake_code` is the longest).
