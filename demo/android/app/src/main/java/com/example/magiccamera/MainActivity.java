package com.example.magiccamera;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.Size;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.camera.core.CameraSelector;
import androidx.camera.core.ImageAnalysis;
import androidx.camera.core.ImageProxy;
import androidx.camera.lifecycle.ProcessCameraProvider;
import androidx.core.content.ContextCompat;

import com.example.superresolution.natives.SuperResolutionLib;
import com.google.common.util.concurrent.ListenableFuture;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

public class MainActivity extends AppCompatActivity {
    private static final String TAG = "MagicMagnifierUI";
    private static final int INPUT_WIDTH = 1920;
    private static final int INPUT_HEIGHT = 1080;
    private static final int MODE_SPEED = 0;
    private static final int MODE_BALANCED = 1;
    private static final int BACKEND_OPENGLES = 5;
    private static final float MIN_SCALE = 1.0f;
    private static final float MAX_SCALE = 8.0f;
    private static final int SCALE_SEEK_MAX = 700;
    private static final long SCALE_APPLY_THROTTLE_MS = 80L;

    private final ExecutorService cameraExecutor = Executors.newSingleThreadExecutor();
    private final AtomicBoolean processingFrame = new AtomicBoolean(false);
    private final Object engineLock = new Object();
    private final Handler uiHandler = new Handler(Looper.getMainLooper());

    private ImageView outputView;
    private TextView statusText;
    private TextView modeSpeedBtn;
    private TextView modeBalancedBtn;
    private TextView scaleLabel;
    private SeekBar scaleSeekBar;
    private ScaleTickView scaleTickView;
    private boolean scaleDragging;
    private float scaleTouchDownX;
    private float scaleTouchDownY;
    private int scaleTouchSlop;

    private ProcessCameraProvider cameraProvider;
    private volatile boolean running;
    private long srHandle;
    private boolean engineDirty = true;
    private int selectedMode = MODE_SPEED;
    private volatile float selectedScale = 2.0f;
    private int engineInitWidth = -1;
    private int engineInitHeight = -1;
    private float engineInitScale = -1.0f;
    private int engineInitMode = -1;
    private String currentModelPath = "";
    private String sdkVersion = "v2.1.0";
    private byte[] rgbaFull;
    private byte[] rgbaCrop;
    private byte[] rgbaOutput;

    private final Runnable scaleApplyRunnable = this::restartEngine;

    private final ActivityResultLauncher<String> cameraPermission =
            registerForActivityResult(new ActivityResultContracts.RequestPermission(), granted -> {
                if (granted) {
                    startCamera();
                } else {
                    stopWithError("camera permission denied");
                }
            });

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        buildUi();
        try {
            ensureModelsInSandbox();
        } catch (RuntimeException e) {
            stopWithError(e.getMessage());
            return;
        }
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            startCamera();
        } else {
            cameraPermission.launch(Manifest.permission.CAMERA);
        }
    }

    @Override
    protected void onDestroy() {
        running = false;
        uiHandler.removeCallbacks(scaleApplyRunnable);
        stopCamera();
        synchronized (engineLock) {
            SuperResolutionLib.uninitSuperResolution();
            srHandle = 0;
            engineDirty = true;
            engineInitWidth = -1;
            engineInitHeight = -1;
            engineInitScale = -1.0f;
            engineInitMode = -1;
            currentModelPath = "";
        }
        cameraExecutor.shutdown();
        super.onDestroy();
    }

    private void buildUi() {
        FrameLayout root = new FrameLayout(this);
        outputView = new ImageView(this);
        outputView.setScaleType(ImageView.ScaleType.FIT_CENTER);
        outputView.setBackgroundColor(0xff000000);
        root.addView(outputView, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        try {
            sdkVersion = SuperResolutionLib.getVersion();
        } catch (Throwable ignored) {}

        LinearLayout controls = new LinearLayout(this);
        controls.setOrientation(LinearLayout.VERTICAL);
        controls.setPadding(20, 16, 20, 16);
        controls.setBackgroundColor(0x99000000);

        LinearLayout modeRow = new LinearLayout(this);
        modeRow.setOrientation(LinearLayout.HORIZONTAL);
        modeRow.setGravity(Gravity.CENTER_VERTICAL);
        modeSpeedBtn = buildModeButton("speed", true, () -> selectMode(MODE_SPEED));
        modeBalancedBtn = buildModeButton("balanced", false, () -> selectMode(MODE_BALANCED));
        modeRow.addView(modeSpeedBtn);
        LinearLayout.LayoutParams balancedParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        balancedParams.leftMargin = 12;
        modeRow.addView(modeBalancedBtn, balancedParams);

        TextView versionText = new TextView(this);
        versionText.setText("MagicSR " + sdkVersion);
        versionText.setTextColor(0xddffffff);
        versionText.setTextSize(14f);
        versionText.setTypeface(null, Typeface.BOLD);
        versionText.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        LinearLayout.LayoutParams versionParams = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f);
        versionParams.rightMargin = 4;
        modeRow.addView(versionText, versionParams);
        controls.addView(modeRow);

        scaleTouchSlop = ViewConfiguration.get(this).getScaledTouchSlop();

        LinearLayout scaleRow = new LinearLayout(this);
        scaleRow.setOrientation(LinearLayout.HORIZONTAL);
        scaleRow.setGravity(Gravity.CENTER_VERTICAL);
        scaleRow.setPadding(0, 10, 0, 0);
        scaleLabel = new TextView(this);
        scaleLabel.setTextColor(0xffffffff);
        scaleRow.addView(scaleLabel);

        LinearLayout seekCol = new LinearLayout(this);
        seekCol.setOrientation(LinearLayout.VERTICAL);
        scaleSeekBar = new SeekBar(this);
        scaleSeekBar.setMax(SCALE_SEEK_MAX);
        scaleSeekBar.setProgress(progressForScale(selectedScale));
        scaleSeekBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                selectedScale = scaleForProgress(progress);
                updateControls();
                if (fromUser) {
                    scheduleScaleApply();
                }
            }
            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override public void onStopTrackingTouch(SeekBar seekBar) {
                if (scaleDragging) {
                    scheduleScaleApply();
                } else {
                    snapScaleToNearestInteger();
                }
            }
        });
        scaleSeekBar.setOnTouchListener((v, event) -> {
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    scaleDragging = false;
                    scaleTouchDownX = event.getX();
                    scaleTouchDownY = event.getY();
                    break;
                case MotionEvent.ACTION_MOVE: {
                    float dx = event.getX() - scaleTouchDownX;
                    float dy = event.getY() - scaleTouchDownY;
                    if (!scaleDragging && (dx * dx + dy * dy) > (scaleTouchSlop * scaleTouchSlop)) {
                        scaleDragging = true;
                    }
                    break;
                }
                default:
                    break;
            }
            return false;
        });
        seekCol.addView(scaleSeekBar, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        scaleTickView = new ScaleTickView(this);
        scaleTickView.setOnTickSelectedListener(this::applyIntegerScale);
        seekCol.addView(scaleTickView, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(28)));
        scaleSeekBar.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) ->
                scaleTickView.syncPadding(scaleSeekBar.getPaddingStart(), scaleSeekBar.getPaddingEnd()));

        LinearLayout.LayoutParams seekParams = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f);
        seekParams.leftMargin = 12;
        scaleRow.addView(seekCol, seekParams);
        controls.addView(scaleRow);

        statusText = new TextView(this);
        statusText.setTextColor(0xffffffff);
        statusText.setPadding(0, 10, 0, 0);
        controls.addView(statusText);
        updateControls();

        FrameLayout.LayoutParams controlsParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP);
        root.addView(controls, controlsParams);
        setContentView(root);
    }

    private TextView buildModeButton(String text, boolean selected, Runnable onClick) {
        TextView btn = new TextView(this);
        btn.setText(text);
        btn.setPadding(24, 12, 24, 12);
        btn.setTextSize(14f);
        btn.setOnClickListener(v -> onClick.run());
        btn.setBackgroundColor(selected ? 0xffffffff : 0xff444444);
        btn.setTextColor(selected ? 0xff000000 : 0xffffffff);
        return btn;
    }

    private void selectMode(int mode) {
        if (selectedMode == mode) return;
        selectedMode = mode;
        updateControls();
        restartEngine();
    }

    private void updateControls() {
        if (scaleLabel != null) {
            scaleLabel.setText("scale=x" + formatScale(selectedScale));
        }
        if (modeSpeedBtn != null && modeBalancedBtn != null) {
            boolean speed = selectedMode == MODE_SPEED;
            modeSpeedBtn.setBackgroundColor(speed ? 0xffffffff : 0xff444444);
            modeSpeedBtn.setTextColor(speed ? 0xff000000 : 0xffffffff);
            modeBalancedBtn.setBackgroundColor(speed ? 0xff444444 : 0xffffffff);
            modeBalancedBtn.setTextColor(speed ? 0xffffffff : 0xff000000);
        }
    }

    private void scheduleScaleApply() {
        uiHandler.removeCallbacks(scaleApplyRunnable);
        uiHandler.postDelayed(scaleApplyRunnable, SCALE_APPLY_THROTTLE_MS);
    }

    private void snapScaleToNearestInteger() {
        applyIntegerScale(Math.round(selectedScale));
    }

    private void applyIntegerScale(int integerScale) {
        int snapped = Math.max(Math.round(MIN_SCALE), Math.min(Math.round(MAX_SCALE), integerScale));
        selectedScale = snapped;
        if (scaleSeekBar != null) {
            scaleSeekBar.setProgress(progressForScale(selectedScale));
        }
        updateControls();
        scheduleScaleApply();
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    /** Integer tick marks (1..8) aligned with the SeekBar track; tap selects that scale. */
    private static final class ScaleTickView extends View {
        interface OnTickSelectedListener {
            void onTickSelected(int integerScale);
        }

        private final Paint linePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private OnTickSelectedListener listener;
        private int trackPadStart;
        private int trackPadEnd;

        ScaleTickView(Context context) {
            super(context);
            float density = context.getResources().getDisplayMetrics().density;
            linePaint.setColor(0xffcccccc);
            linePaint.setStrokeWidth(Math.max(1f, density));
            textPaint.setColor(0xffdddddd);
            textPaint.setTextAlign(Paint.Align.CENTER);
            textPaint.setTextSize(11f * density);
            setClickable(true);
        }

        void setOnTickSelectedListener(OnTickSelectedListener listener) {
            this.listener = listener;
        }

        void syncPadding(int padStart, int padEnd) {
            if (trackPadStart == padStart && trackPadEnd == padEnd) return;
            trackPadStart = padStart;
            trackPadEnd = padEnd;
            invalidate();
        }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            int w = getWidth();
            int h = getHeight();
            if (w <= 0 || h <= 0) return;
            float trackLeft = trackPadStart;
            float trackRight = w - trackPadEnd;
            float trackW = Math.max(1f, trackRight - trackLeft);
            float lineTop = 0f;
            float lineBottom = Math.min(h * 0.35f, 10f * getResources().getDisplayMetrics().density);
            float textY = lineBottom + textPaint.getTextSize() + 2f;
            int minTick = Math.round(MIN_SCALE);
            int maxTick = Math.round(MAX_SCALE);
            int span = Math.max(1, maxTick - minTick);
            for (int tick = minTick; tick <= maxTick; tick++) {
                float t = (tick - minTick) / (float) span;
                float x = trackLeft + t * trackW;
                canvas.drawLine(x, lineTop, x, lineBottom, linePaint);
                canvas.drawText(String.valueOf(tick), x, textY, textPaint);
            }
        }

        @Override
        public boolean onTouchEvent(MotionEvent event) {
            if (event.getActionMasked() == MotionEvent.ACTION_UP && listener != null) {
                int w = getWidth();
                float trackLeft = trackPadStart;
                float trackRight = w - trackPadEnd;
                float trackW = Math.max(1f, trackRight - trackLeft);
                float unit = (event.getX() - trackLeft) / trackW;
                unit = Math.max(0f, Math.min(1f, unit));
                float scale = MIN_SCALE + unit * (MAX_SCALE - MIN_SCALE);
                listener.onTickSelected(Math.round(scale));
                return true;
            }
            return true;
        }
    }

    private void startCamera() {
        running = true;
        ListenableFuture<ProcessCameraProvider> providerFuture = ProcessCameraProvider.getInstance(this);
        providerFuture.addListener(() -> {
            try {
                cameraProvider = providerFuture.get();
                bindAnalyzer();
            } catch (Exception e) {
                stopWithError("camera init failed: " + e.getMessage());
            }
        }, ContextCompat.getMainExecutor(this));
    }

    private void stopCamera() {
        running = false;
        if (cameraProvider != null) {
            cameraProvider.unbindAll();
        }
    }

    private void bindAnalyzer() {
        int targetRotation = getDisplay() != null
                ? getDisplay().getRotation()
                : getWindowManager().getDefaultDisplay().getRotation();
        ImageAnalysis analysis = new ImageAnalysis.Builder()
                .setTargetResolution(new Size(INPUT_WIDTH, INPUT_HEIGHT))
                .setTargetRotation(targetRotation)
                .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                .build();
        analysis.setAnalyzer(cameraExecutor, this::analyzeFrame);
        cameraProvider.unbindAll();
        cameraProvider.bindToLifecycle(this, CameraSelector.DEFAULT_BACK_CAMERA, analysis);
        statusText.setText("camera ready (" + sdkVersion + ")");
    }

    private void analyzeFrame(@NonNull ImageProxy image) {
        if (!running) {
            image.close();
            return;
        }
        if (!processingFrame.compareAndSet(false, true)) {
            image.close();
            return;
        }
        try {
            final float scale = selectedScale;
            int inW = image.getWidth();
            int inH = image.getHeight();
            int rotationDegrees = image.getImageInfo().getRotationDegrees();
            // Magnifier: crop center (1/scale) of the frame, then SR that crop back up.
            int cropW = evenSize(Math.round(inW / scale), inW);
            int cropH = evenSize(Math.round(inH / scale), inH);
            int cropX = (inW - cropW) / 2;
            int cropY = (inH - cropH) / 2;
            /* Core output size: floor(value * scale + 0.5) in double (magic_backend.h). */
            int outW = scaledDimension(cropW, scale);
            int outH = scaledDimension(cropH, scale);
            ensureBuffers(inW, inH, cropW, cropH, outW, outH);
            yuv420ToRgba(image, rgbaFull);
            cropRgba(rgbaFull, inW, inH, cropX, cropY, cropW, cropH, rgbaCrop);

            int ret;
            synchronized (engineLock) {
                ensureEngineLocked(cropW, cropH, scale, selectedMode);
                ret = SuperResolutionLib.processImage(rgbaCrop, rgbaOutput, 4, cropW, cropH, outW, outH);
            }
            if (ret != 0) {
                // Transient failures (e.g. mid-scale rebuild) should not kill the camera.
                Log.w(TAG, "MC_Process failed ret=" + ret + " crop=" + cropW + "x" + cropH
                        + " scale=" + scale + " — skip frame");
                return;
            }
            Bitmap bitmap = rotateBitmap(rgbaToBitmap(rgbaOutput, outW, outH), rotationDegrees);
            final int displayW = bitmap.getWidth();
            final int displayH = bitmap.getHeight();
            runOnUiThread(() -> {
                if (!running) return;
                outputView.setImageBitmap(bitmap);
                statusText.setText("ver=" + sdkVersion + " mode=" + modeName() + " scale=x" + formatScale(scale)
                        + "\ncrop=" + cropW + "x" + cropH
                        + " out=" + displayW + "x" + displayH
                        + " rot=" + rotationDegrees);
            });
        } catch (Exception e) {
            Log.e(TAG, "analyzeFrame error: " + e.getMessage(), e);
            runOnUiThread(() -> {
                if (statusText != null) {
                    statusText.setText("warn: " + (e.getMessage() != null ? e.getMessage() : "frame failed"));
                }
            });
        } finally {
            image.close();
            processingFrame.set(false);
        }
    }

    private void ensureBuffers(int fullW, int fullH, int cropW, int cropH, int outW, int outH) {
        int fullBytes = fullW * fullH * 4;
        int cropBytes = cropW * cropH * 4;
        int outBytes = outW * outH * 4;
        if (rgbaFull == null || rgbaFull.length != fullBytes) rgbaFull = new byte[fullBytes];
        if (rgbaCrop == null || rgbaCrop.length != cropBytes) rgbaCrop = new byte[cropBytes];
        if (rgbaOutput == null || rgbaOutput.length != outBytes) rgbaOutput = new byte[outBytes];
    }

    /**
     * Same as product {@code scaled_dimension} in {@code src/magic_backend.h}:
     * {@code floor((double)value * (double)scaler + 0.5)}, clamped to [1, UINT32_MAX].
     */
    static int scaledDimension(int value, float scaler) {
        double scaled = (double) value * (double) scaler;
        if (scaled < 1.0) return 1;
        if (scaled > (double) 0xFFFFFFFFL) return Integer.MAX_VALUE;
        return (int) Math.floor(scaled + 0.5d);
    }

    /** Prefer even sizes for GLES textures; never exceed source size. Spatial SR needs >= 64. */
    private static int evenSize(int desired, int max) {
        int v = Math.max(64, Math.min(desired, max));
        if (v > max) v = max;
        return v & ~1;
    }

    private static void cropRgba(byte[] src, int srcW, int srcH,
                                 int cropX, int cropY, int cropW, int cropH,
                                 byte[] dst) {
        for (int y = 0; y < cropH; y++) {
            int srcRow = ((cropY + y) * srcW + cropX) * 4;
            int dstRow = y * cropW * 4;
            System.arraycopy(src, srcRow, dst, dstRow, cropW * 4);
        }
    }

    /** Caller must hold engineLock. */
    private void ensureEngineLocked(int width, int height, float scale, int mode) {
        String modelPath = modelPathForCurrentMode();
        boolean stale = engineDirty
                || srHandle == 0
                || engineInitWidth != width
                || engineInitHeight != height
                || engineInitMode != mode
                || Math.abs(engineInitScale - scale) > 0.0001f
                || !modelPath.equals(currentModelPath);
        if (!stale) return;
        SuperResolutionLib.uninitSuperResolution();
        srHandle = 0;
        srHandle = SuperResolutionLib.initSuperResolution(
                width,
                height,
                scale,
                mode,
                1,
                BACKEND_OPENGLES,
                modelPath);
        if (srHandle == 0) {
            engineInitWidth = -1;
            engineInitHeight = -1;
            engineInitScale = -1.0f;
            engineInitMode = -1;
            currentModelPath = "";
            engineDirty = true;
            throw new IllegalStateException("MC_Process init failed, model=" + modelPath);
        }
        engineInitWidth = width;
        engineInitHeight = height;
        engineInitScale = scale;
        engineInitMode = mode;
        currentModelPath = modelPath;
        engineDirty = false;
    }

    private void restartEngine() {
        // Mark dirty only. Native handle stays live until the next camera-thread
        // ensureEngineLocked() or onDestroy() — never Uninit from the UI thread
        // while a frame may be inside MC_Process.
        synchronized (engineLock) {
            engineDirty = true;
            engineInitWidth = -1;
            engineInitHeight = -1;
            engineInitScale = -1.0f;
            engineInitMode = -1;
            currentModelPath = "";
        }
    }

    private void ensureModelsInSandbox() {
        File modelDir = new File(getFilesDir(), "models");
        if (!modelDir.exists() && !modelDir.mkdirs()) {
            throw new RuntimeException("cannot create model dir: " + modelDir.getAbsolutePath());
        }
        // Combined public GPU model: BALANCED=seg0, SPEED=seg(1+sharpen).
        copyModelIfMissing(modelDir, "magic_sr_gpu_params.bin");
    }

    private void copyModelIfMissing(File modelDir, String fileName) {
        File out = new File(modelDir, fileName);
        if (out.exists() && out.length() > 0) return;
        try (InputStream in = getAssets().open("model/" + fileName);
             OutputStream os = new FileOutputStream(out, false)) {
            byte[] buf = new byte[64 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) {
                os.write(buf, 0, n);
            }
            os.flush();
        } catch (Exception e) {
            Log.w(TAG, "optional model not in assets: " + fileName + " err=" + e.getMessage());
            if (out.exists() && out.length() <= 0) {
                //noinspection ResultOfMethodCallIgnored
                out.delete();
            }
        }
    }

    private String modelPathForCurrentMode() {
        // Both spatial modes load the same combined magic_sr_gpu_params.bin.
        String[] names = new String[] { "magic_sr_gpu_params.bin" };
        File modelDir = new File(getFilesDir(), "models");
        StringBuilder tried = new StringBuilder();
        for (String name : names) {
            File model = new File(modelDir, name);
            if (tried.length() > 0) tried.append(", ");
            tried.append(model.getAbsolutePath());
            if (model.exists() && model.length() > 0) {
                return model.getAbsolutePath();
            }
        }
        throw new IllegalStateException("model missing; tried: " + tried);
    }

    private String modeName() {
        return selectedMode == MODE_BALANCED ? "balanced" : "speed";
    }

    private static String formatScale(float scale) {
        return String.format(Locale.US, "%.2f", scale).replaceAll("0+$", "").replaceAll("\\.$", "");
    }

    private static int progressForScale(float scale) {
        float clamped = Math.max(MIN_SCALE, Math.min(MAX_SCALE, scale));
        float unit = (clamped - MIN_SCALE) / (MAX_SCALE - MIN_SCALE);
        return Math.round(unit * SCALE_SEEK_MAX);
    }

    private static float scaleForProgress(int progress) {
        int bounded = Math.max(0, Math.min(SCALE_SEEK_MAX, progress));
        float unit = (float) bounded / (float) SCALE_SEEK_MAX;
        return MIN_SCALE + unit * (MAX_SCALE - MIN_SCALE);
    }

    private void stopWithError(String message) {
        stopCamera();
        synchronized (engineLock) {
            SuperResolutionLib.uninitSuperResolution();
            srHandle = 0;
            engineDirty = true;
            engineInitWidth = -1;
            engineInitHeight = -1;
            engineInitScale = -1.0f;
            engineInitMode = -1;
            currentModelPath = "";
        }
        Log.e(TAG, "stopWithError: " + message);
        if (statusText != null) {
            statusText.setText("error: " + message);
        }
    }

    private static Bitmap rgbaToBitmap(byte[] rgba, int width, int height) {
        int[] pixels = new int[width * height];
        int p = 0;
        for (int i = 0; i < pixels.length; i++) {
            int r = rgba[p] & 0xff;
            int g = rgba[p + 1] & 0xff;
            int b = rgba[p + 2] & 0xff;
            int a = rgba[p + 3] & 0xff;
            pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
            p += 4;
        }
        return Bitmap.createBitmap(pixels, width, height, Bitmap.Config.ARGB_8888);
    }

    private static Bitmap rotateBitmap(Bitmap bitmap, int rotationDegrees) {
        if (bitmap == null || rotationDegrees % 360 == 0) return bitmap;
        Matrix matrix = new Matrix();
        matrix.postRotate(rotationDegrees);
        return Bitmap.createBitmap(bitmap, 0, 0, bitmap.getWidth(), bitmap.getHeight(), matrix, true);
    }

    private static void yuv420ToRgba(ImageProxy image, byte[] rgbaOut) {
        ImageProxy.PlaneProxy yPlane = image.getPlanes()[0];
        ImageProxy.PlaneProxy uPlane = image.getPlanes()[1];
        ImageProxy.PlaneProxy vPlane = image.getPlanes()[2];
        int width = image.getWidth();
        int height = image.getHeight();
        byte[] yData = new byte[yPlane.getBuffer().remaining()];
        byte[] uData = new byte[uPlane.getBuffer().remaining()];
        byte[] vData = new byte[vPlane.getBuffer().remaining()];
        yPlane.getBuffer().get(yData);
        uPlane.getBuffer().get(uData);
        vPlane.getBuffer().get(vData);
        int yRowStride = yPlane.getRowStride();
        int uRowStride = uPlane.getRowStride();
        int vRowStride = vPlane.getRowStride();
        int uPixelStride = uPlane.getPixelStride();
        int vPixelStride = vPlane.getPixelStride();
        int out = 0;
        for (int j = 0; j < height; j++) {
            int yRow = j * yRowStride;
            int uvRow = (j / 2);
            for (int i = 0; i < width; i++) {
                int y = yData[yRow + i] & 0xff;
                int uvCol = (i / 2);
                int u = uData[uvRow * uRowStride + uvCol * uPixelStride] & 0xff;
                int v = vData[uvRow * vRowStride + uvCol * vPixelStride] & 0xff;
                int c = y - 16;
                int d = u - 128;
                int e = v - 128;
                int r = (298 * c + 409 * e + 128) >> 8;
                int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
                int b = (298 * c + 516 * d + 128) >> 8;
                rgbaOut[out++] = (byte) clamp255(r);
                rgbaOut[out++] = (byte) clamp255(g);
                rgbaOut[out++] = (byte) clamp255(b);
                rgbaOut[out++] = (byte) 255;
            }
        }
    }

    private static int clamp255(int value) {
        if (value < 0) return 0;
        return Math.min(value, 255);
    }
}
