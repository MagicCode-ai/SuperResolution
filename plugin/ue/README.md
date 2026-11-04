# MagicSR UE Android/iOS Plugin

This bridge reuses the same core API (`mc_interface.h` + `libmagic_sr.a`) as Unity.

## Documentation

- Full usage manual: `plugin/ue/USAGE_GUIDE.md`

## Location

- `plugin/ue/MagicSR/MagicSR.uplugin`
- `plugin/ue/MagicSR/Source/MagicSR/`
- Verified snapshots are stored under `plugin/ue/verified/UE_<version>/<Platform>/MagicSR`.

## Exposed Blueprint API

**Preferred (GPU texture):**
- `Enable` / `Disable` / `IsEnabled` — **one-call integration**
- `CreateSessionEx`
- `ProcessUTexture` (UTexture / RenderTarget → native on render thread)
- `ProcessNativeTexture` (raw native handles)

**Fallback (CPU Y8/buffer):**
- `CreateSession`
- `ProcessY8`

Also:
- `GetVersion`
- `SetParam`
- `QueryStatus`
- `DestroySession`

See `USAGE_GUIDE.md` for InputType/Backend pairing.

## Platform notes

- The module links `build/android/build/libmagic_sr.a`.
- On iOS the module links `build/ipad/magic_sr/Release-iphoneos/libmagic_sr.a` and adds Metal/CoreVideo/Foundation frameworks.
- Android targets the `arm64-v8a` workflow aligned with the existing project build scripts.
- Before packaging a UE project, ensure the matching platform `libmagic_sr.a` exists at the expected path.

## Validation

For runtime validation workflow and troubleshooting, see:

- `plugin/ue/USAGE_GUIDE.md`
