# Custom block fixtures

A behavior and resource pack for the local BDS that mirror custom-block cases
seen on servers:

| Block | Exercises |
|---|---|
| `perlin:stool` | custom geometry, a `custom:facing_direction` state and per-permutation `minecraft:transformation` |
| `perlin:crumbling` | `minecraft:geometry.full_block` with the vanilla `cobblestone` texture key |
| `perlin:boombox` | per-face vanilla keys `tnt_top`, `tnt_side` and `tnt_bottom` |
| `perlin:plant` | two zero-thickness crossed planes with `render_method: alpha_test` (must draw both sides) |

Install by copying `perlin_test_bp` to `development_behavior_packs/perlin_test`
and `perlin_test_rp` to `development_resource_packs/perlin_test`, then list both
pack UUIDs (from their manifests) in the world's `world_behavior_packs.json` and
`world_resource_packs.json`. Place blocks from the server console, for example
`setblock 244 100 -240 perlin:stool ["custom:facing_direction"=1]`.

The Hive's own textures and geometry are not included; copy them from
`server_packs_extracted` locally when reproducing a Hive case.
