# emu: emulator-side work (secondary track)

xemu is the reference oracle: it runs the real XBE with the real NV2A and
MCPX emulated, so it is where we capture savestates for memory analysis,
compare behaviour against the native build, and trace the renderer. Nothing
is planned here beyond helper scripts unless the native track stalls.

Assessment: `docs/research/xemu-emulator-track.md` and
`docs/research/cxbx-and-xbsymboldatabase.md`.
