# Salvatore native deployment plugin v1.1

Required with xNVSE 6.4.9+ (6.x), Steam FNV 1.4.0.525. MIT code licence in LICENSE; media has separate terms.
Intercepts normal Weapons inventory clicks before equip, reads actual Salvatore stack condition, refuses duplicate/broken items, and commits deployment after Pip-Boy closes. Leaves the selected gun instance untouched. No ShowOff/JIP/JohnnyGuitar dependency.

Build from repository root with Python 3.12 and Zig 0.14.1:

```powershell
py -3.12 projects/salvatore/native-plugin/build.py --zig C:/path/to/zig.exe
```

Build runs native host unit checks and writes ignored build/salvatore-native/candidate1 outputs. Host checks do not test engine hooks. Release DLL was tested in game; its SHA256 is 9983fa8cf1f4dba4f65f13ced4b2fb3cb4172c342df0b6a84c72421869ffe13d.
Install released DLL in Data/NVSE/Plugins. Read the release readme for supported interfaces, safeguards and limits.
