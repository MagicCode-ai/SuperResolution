# UE Bridge Verification Report

- Scope: bridge code, dependency layout, and Android smoke project setup
- Output: `plugin/ue/MagicSR` plugin skeleton with Blueprint API wrappers and `plugin/ue/Samples/AndroidSmokeTest`

## Automated checks executed

- `bash plugin/ue/tests/verify_layout.sh`
- `nm -g build/android/build/libmagic_sr.a | rg "MC_(Init|Process|Control|Uninit|GetVersion)"`
- `plugin/ue/Samples/AndroidSmokeTest/sync_plugin_files.sh`
- `plugin/ue/Samples/AndroidSmokeTest/check_environment.sh`
- `plugin/ue/Samples/AndroidSmokeTest/build_and_run_android_smoke.sh`

## Result

- PASS: required UE bridge files exist.
- PASS: `mc_interface.h` and `libmagic_sr.a` resolve from the UE module path.
- PASS: `libmagic_sr.a` exports `MC_Init`, `MC_Process`, `MC_Control`, `MC_Uninit`, and `MC_GetVersion`.
- PASS: Android smoke sample synced the UE plugin and CPU model.
- PASS: UE 5.3 Android APK builds, installs, launches on device, and runs the native smoke path through `MC_Init` -> `MC_Control(QUERY_STATUS)` -> `MC_Process` -> `MC_Uninit`.
- PASS: Smoke test writes input/output images on device, pulls them to `plugin/ue/Samples/AndroidSmokeTest/Saved/AndroidSmokeImages`, converts them to PNG, and validates the output dimensions/non-zero content.
- Runtime result: `[MagicSRUESmoke] result=PASS query_ret=0 process_ret=0 destroy_ret=0 output=128x128 nonZero=16189 wrote_input=1 wrote_output=1 err=0x0`.
- Output image: `plugin/ue/Samples/AndroidSmokeTest/Saved/AndroidSmokeImages/output_128x128.png` (`128x128`, `nonZero=16189`).

## iOS UE status

- PASS: UE plugin descriptor and module rules now include iOS support.
- PASS: The iOS module path links `build/ipad/magic_sr/Release-iphoneos/libmagic_sr.a`, adds the required Metal/CoreVideo/Foundation frameworks, and defines `MAGIC_SR_IOS=1`, `SYS_IOS=1`, and `HAVE_NEON=1`.
- PASS: Native iOS smoke code is included in the UE plugin. It runs `MC_Init` -> `MC_Control(QUERY_STATUS)` -> `MC_Process` -> `MC_Uninit`, writes `Documents/MagicSRSmoke/input_64x64.pgm` and `Documents/MagicSRSmoke/output_128x128.pgm`, logs `[MagicSRUESmoke] result=PASS/FAIL`, and exits with the matching status.
- PASS: `plugin/ue/Samples/AndroidSmokeTest/build_and_run_ios_smoke.sh` was added. When the iOS UE toolchain is buildable, it packages the sample for iOS, installs it on a physical device, pushes the CPU model into the app container, launches through `devicectl --console`, pulls PGM outputs, converts them to PNG, and validates output dimensions/non-zero content.
- BLOCKED in the current environment: UE 5.3 registers IOS as `buildable: False` and `RunUAT BuildCookRun -platform=IOS` fails with `Platform IOS is not a valid platform to build. Check that the SDK is installed properly.`
- Root cause: the installed UE 5.3 Apple SDK rules require Xcode 14.1-15.x, while the current selected Xcode/Apple SDK is newer. Install a compatible Xcode 15.x and select it with `sudo xcode-select -s /Applications/Xcode_15.x.app/Contents/Developer`, then rerun `plugin/ue/Samples/AndroidSmokeTest/build_and_run_ios_smoke.sh` for the same strict PASS/PNG validation as Android.
