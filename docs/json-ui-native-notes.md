# JSON UI reference checks

Reference: the local symbol-rich `china-1.16.201.i64` database. IDA reports its
original input as `Windows Client 1.19`; addresses below belong to that client,
not to the current server or the installed modern client.

`TextComponent::getFontScale`, `0x140c03eb0`, multiplies
`mFontScaleFactor` by 0.5, 1, 2, or 4 for small, normal, large, or extra-large.
Both intrinsic measurement and RML emission use these factors. The
`ui_font_*` fixtures check the resulting label dimensions.

A bound `#alpha` supplies the alpha property. Multiplying it into an already
resolved `alpha: "#alpha"` applied the value twice: a 0.3 background became
0.09. `ui_bound_alpha` checks the emitted opacity.

Level exit schedules resource disposal for the next frame boundary. It joins
UI builders and mesh workers, clears HUD/form/chest/screen trees and animation
state, releases RML textures and level atlas/sky resources, and invalidates
pending form-image downloads. The GPU regression checks that old geometry is
gone and a new atlas and mesh render after repeated unloads.

These are bounded checks. Full UI parity, binding scheduling and every server
screen are not established by these fixtures.
