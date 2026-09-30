# Magic Magnifier Demo

`magnifier_demo` is a camera magnifier demo that links the current `libmagic_sr.a` and calls `mc_nscaler_control` / `mc_nscaler_enable` / `mc_nscaler_disable` from `interface/mc_interface.h`.
- Android (OpenGLES backend) links `lib/android/libmagic_sr.a`
- iOS/iPadOS (Metal backend) links `lib/ios/libmagic_sr.a`
- Continuous zoom by slider in range `[1.0, 8.0]`
- `speed` and `balanced` modes

If any required file is missing or SR init/process fails, the app reports error and stops.

## 1) Directory Layout

```text
magnifier_demo/
  android/                 # Android Studio project
  ios/                     # Xcode project
  model/                   # SR model bin files
  build_magnifier_demo.bat # Windows one-click prepare/build script
```

Libraries and the header live in the repo, not inside this demo:

- `interface/mc_interface.h`
- `lib/android/libmagic_sr.a`
- `lib/ios/libmagic_sr.a`

## 2) Required Files

Before build, ensure these files exist:

### Android
- `lib/android/libmagic_sr.a`
- `interface/mc_interface.h`
- `demo/magnifier_demo/model/magic_gles_speed_gpu_params.bin`
- `demo/magnifier_demo/model/magic_gles_balanced_gpu_params.bin`

### iOS
- `lib/ios/libmagic_sr.a`
- `interface/mc_interface.h`
- `demo/magnifier_demo/model/magic_metal_speed_gpu_params.bin`
- `demo/magnifier_demo/model/magic_metal_balanced_gpu_params.bin`

> iOS project has a build phase that copies all `model/*.bin` into app bundle.

## 3) One-Click Build (Windows, Android)

Run from `magnifier_demo` root:

```bat
build_magnifier_demo.bat
```

What it does:
1. Verifies required libs/models exist
2. Creates `android/app/src/main/assets/model` if missing
3. Copies GLES model bins into Android assets
4. Runs `android\gradlew.bat :app:assembleDebug`
5. Prints APK output path

APK output:
- `android\app\build\outputs\apk\debug\app-debug.apk`

## 4) Manual Build

### Android (macOS/Linux/Windows)

```bash
cd android
./gradlew :app:assembleDebug
```

Install:

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

### iOS (macOS + Xcode)

Open:
- `ios/MagicCameraSR.xcodeproj`

Or CLI:

```bash
cd ios
xcodebuild -project "MagicCameraSR.xcodeproj" \
  -scheme "MagicCameraSR" \
  -configuration Debug \
  -sdk iphoneos build
```

## 5) Runtime Usage

1. Launch app and grant camera permission
2. Select mode: `speed` or `speed`
3. Drag slider to adjust scale `1.0x ~ 8.0x`
4. Output is SR frame from live camera input

## 6) Notes

- Android target ABI is `arm64-v8a`.
- iOS deployment target is 18.4, matching `lib/ios/libmagic_sr.a`.
- This demo is intentionally strict: missing model/library or SR errors will stop processing (no fallback).
