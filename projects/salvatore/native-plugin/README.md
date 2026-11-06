# Salvatore native deployment plugin v1.2

Required with xNVSE 6.4.9+ (6.x), Steam FNV 1.4.0.525. MIT code licence in LICENSE; media has separate terms. No ShowOff/JIP/JohnnyGuitar dependency.

- **Deployment (v1.1):** intercepts normal Weapons inventory clicks before equip, reads actual Salvatore stack condition, refuses duplicate/broken items, and commits deployment after Pip-Boy closes. Leaves the selected gun instance untouched.
- **VATS (v1.2):** Salvatore's sprites are unlit, so VATS's limb visibility query could never count him. During his two query passes the plugin also draws a hidden lit shape from his v1.2 skeleton, and it keeps VATS from finishing his single limb before its result arrives (redirects at 0x80161F, 0x874A82, 0x8741AF and 0x7ED349).
- **Theme (v1.2):** adds the engine's pause-in-menus flag to his theme sound when it is created (0x889EDF), and skips the random start position Play gives a loop sound when his theme is paused or already playing (0xAED0DA), so it carries on where it paused.

Every redirect checks the exact original call and refuses, rather than overwrites, a site another plugin has already changed. The log (game folder/SalvatoreGanacci-native.log) records each one.

Build from repository root with Python 3.12 and Zig 0.14.1:

```powershell
py -3.12 projects/salvatore/native-plugin/build.py --zig C:/path/to/zig.exe --out build/salvatore-native/v12
```

The build runs native host unit checks, including the theme hook's assembly run against stand-ins, and writes ignored build outputs. Host checks do not test engine hooks in the game. The released v1.2 DLL was tested in game; its SHA256 is bfdca568bb673a60e6b20ecea12fa521e09f64791ecd8cd3de98922a2038ea2d. A rebuild from this source gives the same code and data; only the linker's build ID and timestamp differ.
Install the released DLL in Data/NVSE/Plugins. Read the release readme for supported interfaces, safeguards and limits.
