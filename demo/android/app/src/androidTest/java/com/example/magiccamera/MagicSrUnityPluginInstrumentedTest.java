package com.example.magiccamera;

import static org.junit.Assert.assertTrue;
import static org.junit.Assume.assumeTrue;

import android.Manifest;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.Bundle;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import androidx.test.platform.app.InstrumentationRegistry;

import com.example.superresolution.natives.SuperResolutionLib;

import org.junit.Test;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Comparator;
import java.util.Locale;

public class MagicSrUnityPluginInstrumentedTest {
    private static final String TAG = "MagicSRUnityPluginITest";
    private static final String CPU_MODEL = "magic_speedx2_cpu_params.bin";
    private static final String GLES_SPEED_MODEL = "magic_gles_speed_gpu_params.bin";
    private static final String GLES_BALANCED_MODEL = "magic_gles_balanced_gpu_params.bin";
    private static final String VULKAN_MODEL = "magic_veryfast_gpu_params.bin";
    private static final String SPEED_GPU_MODEL = "magic_speed_gpu_params.bin";
    private static final String SPEED_GPU_MODEL = "magic_balanced_gpu_params.bin";
    private static final int MODE_SPEED = 0;
    private static final int MODE_BALANCED = 1;
    private static final int BACKEND_OPENGLES = 5;
    private static final int BACKEND_VULKAN = 6;
    private static final int SCALE = 2;
    private static final float DEFAULT_BATCH_SCALE = 1.5f;

    @Test
    public void unityPluginSmokeTestPassesInsideCameraAppPackage() {
        adoptStoragePermissions();
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String modelPath = resolveModelPath(context);
        String result = SuperResolutionLib.runUnityPluginSmokeTest(modelPath);
        Log.i(TAG, "modelPath=" + modelPath + "\n" + result);
        assertTrue("Unity plugin smoke test failed:\nmodelPath=" + modelPath + "\n" + result,
                result.contains("result=PASS"));
    }

    @Test
    public void unityPluginOpenGlesRgbCameraPathPassesOnDevice() {
        adoptStoragePermissions();
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String glesModel = resolveModelPath(context, GLES_SPEED_MODEL);
        String fastModel = resolveModelPath(context, GLES_BALANCED_MODEL);
        int mode = canRead(new File(glesModel)) ? MODE_SPEED : MODE_BALANCED;
        String modelName = mode == MODE_SPEED ? GLES_SPEED_MODEL : GLES_BALANCED_MODEL;
        assumeTrue("OpenGLES model is required: gles=" + glesModel + " fast=" + fastModel,
                canRead(new File(mode == MODE_SPEED ? glesModel : fastModel)));
        runUnityPluginRgbBackendCase(BACKEND_OPENGLES, new String[] { modelName }, "opengles", mode);
    }

    @Test
    public void unityPluginVulkanRgbCameraPathPassesOnDevice() {
        runUnityPluginRgbBackendCase(BACKEND_VULKAN, new String[] { SPEED_GPU_MODEL }, "vulkan");
    }

    @Test
    public void magicOpenGlesRgbCameraPathPassesOnDevice() {
        adoptStoragePermissions();
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String glesModel = resolveModelPath(context, GLES_SPEED_MODEL);
        String fastModel = resolveModelPath(context, GLES_BALANCED_MODEL);
        int mode = canRead(new File(glesModel)) ? MODE_SPEED : MODE_BALANCED;
        String modelPath = mode == MODE_SPEED ? glesModel : fastModel;
        final int width = 64;
        final int height = 64;
        final int outW = width * SCALE;
        final int outH = height * SCALE;
        byte[] input = createRgbaInput(width, height);
        byte[] output = new byte[outW * outH * 4];

        long handle = SuperResolutionLib.initSuperResolution(
                width, height, SCALE, mode, 1, BACKEND_OPENGLES, modelPath);
        int ret = handle == 0 ? -1 : SuperResolutionLib.processImage(
                input, output, 4, width, height, outW, outH);
        SuperResolutionLib.uninitSuperResolution();
        int nonZero = countNonZero(output);
        String report = "engine=magic backend=opengles"
                + " glesModel=" + glesModel + " glesReadable=" + canRead(new File(glesModel))
                + " fastModel=" + fastModel + " fastReadable=" + canRead(new File(fastModel))
                + " mode=" + mode
                + " selectedModel=" + modelPath
                + " handle=" + handle
                + " ret=" + ret
                + " nonZero=" + nonZero;
        Log.i(TAG, report);
        assertTrue("Magic OpenGLES backend failed:\n" + report, handle != 0 && ret == 0 && nonZero > 0);
    }

    @Test
    public void modelAvailabilityProbe() {
        adoptStoragePermissions();
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String glesModel = resolveModelPath(context, GLES_SPEED_MODEL);
        String fastModel = resolveModelPath(context, GLES_BALANCED_MODEL);
        String gpuModel = resolveModelPath(context, VULKAN_MODEL);
        String speedModel = resolveModelPath(context, SPEED_GPU_MODEL);
        String balancedModel = resolveModelPath(context, SPEED_GPU_MODEL);
        String cpuModel = resolveModelPath(context, CPU_MODEL);
        String report = "models:"
                + "\n gles=" + glesModel + " readable=" + canRead(new File(glesModel))
                + "\n fastGpu=" + fastModel + " readable=" + canRead(new File(fastModel))
                + "\n gpu=" + gpuModel + " readable=" + canRead(new File(gpuModel))
                + "\n speedGpu=" + speedModel + " readable=" + canRead(new File(speedModel))
                + "\n speedGpu=" + balancedModel + " readable=" + canRead(new File(balancedModel))
                + "\n cpu=" + cpuModel + " readable=" + canRead(new File(cpuModel));
        Log.i(TAG, report);
        assertTrue("At least one Vulkan GPU model should be readable:\n" + report,
                canRead(new File(gpuModel)) || canRead(new File(speedModel)) || canRead(new File(balancedModel)));
    }

    @Test
    public void nativeBackendSelfTestPassesOnDevice() {
        adoptStoragePermissions();
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String modelPath = resolveModelPath(context, VULKAN_MODEL);
        resolveModelPath(context, GLES_SPEED_MODEL);
        resolveModelPath(context, GLES_BALANCED_MODEL);
        resolveModelPath(context, CPU_MODEL);
        String result = SuperResolutionLib.runBackendSelfTest(modelPath);
        Log.i(TAG, "native backend self-test modelPath=" + modelPath + "\n" + result);
        assertTrue("Native backend self-test failed:\nmodelPath=" + modelPath + "\n" + result,
                result.contains("summary failures=0"));
    }

    @Test
    public void androidVulkanImageBatch15xWritesNonBlackOutputs() {
        adoptStoragePermissions();
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Bundle args = InstrumentationRegistry.getArguments();
        String inputDir = args.getString("inputDir", "/sdcard/Download/magic_sr_batch_v540p");
        String outputRoot = args.getString("outputRoot", "/sdcard/Download/magic_sr_batch_outputs_1p5x");
        String modeFilter = args.getString("mode", "");
        float batchScale = parseBatchScale(args.getString("batchScale", ""), DEFAULT_BATCH_SCALE);
        boolean skipBlackScreenValidation = parseBoolean(args.getString("skipBlackScreenValidation", ""), false);
        int maxImages = parsePositiveInt(args.getString("maxImages", ""), 0);
        String outputFormat = normalizeOutputFormat(args.getString("outputFormat", "png"));
        int jpegQuality = parseBoundedInt(args.getString("jpegQuality", ""), 95, 1, 100);
        File inputRoot = new File(inputDir);
        File[] inputs = inputRoot.listFiles(file -> {
            String name = file.getName().toLowerCase(Locale.US);
            return file.isFile() && (name.endsWith(".png") || name.endsWith(".jpg") || name.endsWith(".jpeg"));
        });
        if (inputs != null) {
            Arrays.sort(inputs, Comparator.comparing(File::getName));
            if (maxImages > 0 && inputs.length > maxImages) {
                inputs = Arrays.copyOf(inputs, maxImages);
            }
        }
        assertTrue("No PNG/JPG inputs found in " + inputDir, inputs != null && inputs.length > 0);

        String speedModel = resolveOverrideModelPath(context,
                args.getString("highBalancedModelPath", ""), SPEED_GPU_MODEL);
        String balancedModel = resolveOverrideModelPath(context,
                args.getString("balancedModelPath", ""), SPEED_GPU_MODEL);
        String glesSpeedModel = resolveOverrideModelPath(context,
                args.getString("glesSpeedModelPath", ""), GLES_SPEED_MODEL);
        String glesBalancedModel = resolveOverrideModelPath(context,
                args.getString("glesBalancedModelPath", ""), GLES_BALANCED_MODEL);

        BatchCase[] cases = new BatchCase[] {
                new BatchCase("android_vulkan_speed", MODE_SPEED, speedModel, BACKEND_VULKAN, false),
                new BatchCase("android_vulkan_balanced", MODE_BALANCED, balancedModel, BACKEND_VULKAN, false),
                new BatchCase("android_gles_speed", MODE_SPEED, glesSpeedModel, BACKEND_OPENGLES, false),
                new BatchCase("android_gles_balanced", MODE_BALANCED, glesBalancedModel, BACKEND_OPENGLES, false),
                new BatchCase("sgsr1_vulkan", MODE_SPEED, "", BACKEND_VULKAN, true),
                new BatchCase("sgsr1_gles", MODE_SPEED, "", BACKEND_OPENGLES, true),
        };

        StringBuilder manifest = new StringBuilder();
        manifest.append("inputDir=").append(inputDir).append('\n')
                .append("outputRoot=").append(outputRoot).append('\n')
                .append("scale=").append(batchScale).append('\n')
                .append("skipBlackScreenValidation=").append(skipBlackScreenValidation).append('\n')
                .append("maxImages=").append(maxImages > 0 ? maxImages : "all").append('\n')
                .append("outputFormat=").append(outputFormat).append('\n')
                .append("jpegQuality=").append(jpegQuality).append('\n')
                .append("modeFilter=").append(modeFilter.isEmpty() ? "all" : modeFilter).append('\n')
                .append("inputs=").append(inputs.length).append('\n');

        boolean allOk = true;
        boolean ranAny = false;
        for (BatchCase batchCase : cases) {
            if (!modeFilter.isEmpty() && !batchCase.outputName.equals(modeFilter)) {
                continue;
            }
            ranAny = true;
            assertTrue("Missing model for mode=" + batchCase.outputName + ": " + batchCase.modelPath,
                    batchCase.sgsr || canRead(new File(batchCase.modelPath)));
            manifest.append("CASE mode=").append(batchCase.outputName)
                    .append(" nativeMode=").append(batchCase.mode)
                    .append(" model=").append(batchCase.modelPath.isEmpty() ? "(none)" : batchCase.modelPath)
                    .append('\n');
            BatchResult result = runBatchCase(inputs, new File(outputRoot, batchCase.outputName),
                    batchCase, batchScale, skipBlackScreenValidation, outputFormat, jpegQuality, manifest);
            allOk &= result.ok;
            manifest.append("SUMMARY mode=").append(batchCase.outputName)
                    .append(" saved=").append(result.saved)
                    .append(" failures=").append(result.failures)
                    .append(" blackSource=").append(result.blackSource)
                    .append(" blackOutput=").append(result.blackOutput)
                    .append(" blackFromNonBlack=").append(result.blackFromNonBlack)
                    .append(" cropCheckFailures=").append(result.cropCheckFailures)
                    .append('\n');
            Log.i(TAG, "batch summary mode=" + batchCase.outputName
                    + " saved=" + result.saved
                    + " failures=" + result.failures
                    + " blackSource=" + result.blackSource
                    + " blackOutput=" + result.blackOutput
                    + " blackFromNonBlack=" + result.blackFromNonBlack
                    + " cropCheckFailures=" + result.cropCheckFailures);
        }

        File root = new File(outputRoot);
        //noinspection ResultOfMethodCallIgnored
        root.mkdirs();
        writeText(new File(root, "_manifest.txt"), manifest.toString());
        assertTrue("No batch mode matched mode=" + modeFilter + "\n" + manifest, ranAny);
        assertTrue("Android Vulkan 1.5x batch had failures:\n" + manifest, allOk);
    }

    private static void runUnityPluginRgbBackendCase(int backend, String[] modelNames, String backendName) {
        runUnityPluginRgbBackendCase(backend, modelNames, backendName, MODE_SPEED);
    }

    private static void runUnityPluginRgbBackendCase(int backend, String[] modelNames, String backendName, int mode) {
        adoptStoragePermissions();
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String modelPath = resolveFirstReadableModelPath(context, modelNames);
        final int width = 64;
        final int height = 64;
        final int outW = width * SCALE;
        final int outH = height * SCALE;
        byte[] input = createRgbaInput(width, height);
        byte[] output = new byte[outW * outH * 4];

        long handle = SuperResolutionLib.initUnityPluginSuperResolution(
                width, height, SCALE, mode, backend, modelPath);
        int ret = handle == 0 ? -1 : SuperResolutionLib.processImageUnityPlugin(
                input, output, 4, width, height, outW, outH);
        String debug = SuperResolutionLib.getLastUnityPluginDebugInfo();
        SuperResolutionLib.uninitUnityPluginSuperResolution();

        int nonZero = countNonZero(output);
        String report = "backend=" + backendName
                + " modelPath=" + modelPath
                + " mode=" + mode
                + " handle=" + handle
                + " ret=" + ret
                + " nonZero=" + nonZero
                + " debug=" + debug;
        Log.i(TAG, report);
        assertTrue("Unity plugin RGB backend failed:\n" + report, handle != 0 && ret == 0 && nonZero > 0);
    }

    private static byte[] createRgbaInput(int width, int height) {
        byte[] input = new byte[width * height * 4];
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int i = (y * width + x) * 4;
                input[i] = (byte) ((x * 3 + y * 5) & 0xff);
                input[i + 1] = (byte) ((x * 7) & 0xff);
                input[i + 2] = (byte) ((y * 9) & 0xff);
                input[i + 3] = (byte) 255;
            }
        }
        return input;
    }

    private static BatchResult runBatchCase(File[] inputs, File outputDir, BatchCase batchCase,
                                            float batchScale, boolean skipBlackScreenValidation,
                                            String outputFormat, int jpegQuality,
                                            StringBuilder manifest) {
        cleanOutputDir(outputDir);
        int currentW = -1;
        int currentH = -1;
        long handle = 0;
        BatchResult result = new BatchResult();
        try {
            for (File input : inputs) {
                Bitmap decoded = BitmapFactory.decodeFile(input.getAbsolutePath());
                if (decoded == null) {
                    result.failures++;
                    manifest.append("FAIL mode=").append(batchCase.outputName)
                            .append(" file=").append(input.getName())
                            .append(" reason=decode\n");
                    continue;
                }
                Bitmap bitmap = decoded.getConfig() == Bitmap.Config.ARGB_8888
                        ? decoded
                        : decoded.copy(Bitmap.Config.ARGB_8888, false);
                if (bitmap != decoded) {
                    decoded.recycle();
                }
                int width = bitmap.getWidth();
                int height = bitmap.getHeight();
                int outW = outputWidthForBatch(width, batchCase, batchScale);
                int outH = outputHeightForBatch(height, batchCase, batchScale);
                if (handle == 0 || currentW != width || currentH != height) {
                    if (handle != 0) {
                        uninitBatchCase(batchCase);
                    }
                    handle = initBatchCase(width, height, batchCase, batchScale);
                    currentW = width;
                    currentH = height;
                }
                byte[] rgba = bitmapToRgba(bitmap);
                ImageStats sourceStats = ImageStats.fromRgba(rgba);
                boolean sourceBlack = sourceStats.isBlackScreen();
                if (sourceBlack) {
                    result.blackSource++;
                }
                bitmap.recycle();
                byte[] output = new byte[outW * outH * 4];
                int ret = handle == 0 ? -1 : processBatchCase(batchCase, rgba, output, width, height, outW, outH);
                ImageStats outputStats = ImageStats.fromRgba(output);
                boolean outputBlack = outputStats.isBlackScreen();
                if (outputBlack) {
                    result.blackOutput++;
                }
                boolean blackFromNonBlack = !sourceBlack && outputBlack;
                if (blackFromNonBlack) {
                    result.blackFromNonBlack++;
                }
                CropCheck cropCheck = CropCheck.skipped();
                if (!sourceBlack && !outputBlack) {
                    cropCheck = CropCheck.fromRgba(rgba, width, height, output, outW, outH);
                    if (!cropCheck.ok) {
                        result.cropCheckFailures++;
                    }
                }
                boolean blackScreenOk = skipBlackScreenValidation || !blackFromNonBlack;
                boolean cropCheckOk = skipBlackScreenValidation || cropCheck.ok;
                if (ret == 0 && blackScreenOk
                        && saveRgbaImage(new File(outputDir, outputName(input, outputFormat)),
                        output, outW, outH, outputFormat, jpegQuality)
                        && cropCheckOk) {
                    result.saved++;
                } else {
                    result.failures++;
                    manifest.append("FAIL mode=").append(batchCase.outputName)
                            .append(" file=").append(input.getName())
                            .append(" ret=").append(ret)
                            .append(" sourceBlack=").append(sourceBlack)
                            .append(" outputBlack=").append(outputBlack)
                            .append(" sourceMax=").append(sourceStats.maxRgb)
                            .append(" outputMax=").append(outputStats.maxRgb)
                            .append(" sourceNonBlackRatio=").append(String.format(Locale.US, "%.6f", sourceStats.nonBlackRatio))
                            .append(" outputNonBlackRatio=").append(String.format(Locale.US, "%.6f", outputStats.nonBlackRatio))
                            .append(" cropFullMae=").append(String.format(Locale.US, "%.3f", cropCheck.fullFrameMae))
                            .append(" cropTopLeftMae=").append(String.format(Locale.US, "%.3f", cropCheck.topLeftCropMae))
                            .append(" cropOk=").append(cropCheck.ok)
                            .append('\n');
                }
            }
        } finally {
            if (handle != 0) {
                uninitBatchCase(batchCase);
            }
        }
        result.ok = result.saved == inputs.length && result.failures == 0
                && (skipBlackScreenValidation || (result.blackFromNonBlack == 0 && result.cropCheckFailures == 0));
        return result;
    }

    private static long initBatchCase(int width, int height, BatchCase batchCase, float batchScale) {
        if (batchCase.sgsr) {
            return SuperResolutionLib.initSgsrWithBackend(width, height,
                    Math.max(1, Math.round(batchScale)), 0, batchCase.backend);
        }
        return SuperResolutionLib.initSuperResolution(
                width, height, batchScale, batchCase.mode, 1, batchCase.backend, batchCase.modelPath);
    }

    private static int processBatchCase(BatchCase batchCase, byte[] input, byte[] output,
                                        int width, int height, int outW, int outH) {
        if (batchCase.sgsr) {
            return SuperResolutionLib.processImageSgsr(input, output, 4, width, height, outW, outH);
        }
        return SuperResolutionLib.processImage(input, output, 4, width, height, outW, outH);
    }

    private static void uninitBatchCase(BatchCase batchCase) {
        if (batchCase.sgsr) {
            SuperResolutionLib.uninitSgsr();
        } else {
            SuperResolutionLib.uninitSuperResolution();
        }
    }

    private static int scaledSize(int value, float scale) {
        return Math.max(1, Math.round(value * scale));
    }

    private static int outputWidthForBatch(int width, BatchCase batchCase, float batchScale) {
        return scaledSize(width, outputScaleForBatch(batchCase, batchScale));
    }

    private static int outputHeightForBatch(int height, BatchCase batchCase, float batchScale) {
        return scaledSize(height, outputScaleForBatch(batchCase, batchScale));
    }

    private static float outputScaleForBatch(BatchCase batchCase, float batchScale) {
        return batchScale;
    }

    private static float parseBatchScale(String value, float defaultScale) {
        if (value == null || value.trim().isEmpty()) {
            return defaultScale;
        }
        try {
            float parsed = Float.parseFloat(value.trim());
            return Float.isFinite(parsed) && parsed > 0.0f ? parsed : defaultScale;
        } catch (NumberFormatException e) {
            return defaultScale;
        }
    }

    private static boolean parseBoolean(String value, boolean defaultValue) {
        if (value == null || value.trim().isEmpty()) {
            return defaultValue;
        }
        String normalized = value.trim().toLowerCase(Locale.US);
        if ("1".equals(normalized) || "true".equals(normalized) || "yes".equals(normalized)) {
            return true;
        }
        if ("0".equals(normalized) || "false".equals(normalized) || "no".equals(normalized)) {
            return false;
        }
        return defaultValue;
    }

    private static int parsePositiveInt(String value, int defaultValue) {
        if (value == null || value.trim().isEmpty()) {
            return defaultValue;
        }
        try {
            int parsed = Integer.parseInt(value.trim());
            return parsed > 0 ? parsed : defaultValue;
        } catch (NumberFormatException e) {
            return defaultValue;
        }
    }

    private static int parseBoundedInt(String value, int defaultValue, int minValue, int maxValue) {
        if (value == null || value.trim().isEmpty()) {
            return defaultValue;
        }
        try {
            int parsed = Integer.parseInt(value.trim());
            if (parsed < minValue || parsed > maxValue) return defaultValue;
            return parsed;
        } catch (NumberFormatException e) {
            return defaultValue;
        }
    }

    private static String normalizeOutputFormat(String value) {
        if (value == null) return "png";
        String normalized = value.trim().toLowerCase(Locale.US);
        return ("jpg".equals(normalized) || "jpeg".equals(normalized)) ? "jpg" : "png";
    }

    private static byte[] bitmapToRgba(Bitmap bitmap) {
        int width = bitmap.getWidth();
        int height = bitmap.getHeight();
        int[] pixels = new int[width * height];
        bitmap.getPixels(pixels, 0, width, 0, 0, width, height);
        byte[] out = new byte[width * height * 4];
        for (int i = 0; i < pixels.length; i++) {
            int p = pixels[i];
            int j = i * 4;
            out[j] = (byte) ((p >> 16) & 0xff);
            out[j + 1] = (byte) ((p >> 8) & 0xff);
            out[j + 2] = (byte) (p & 0xff);
            out[j + 3] = (byte) ((p >>> 24) & 0xff);
        }
        return out;
    }

    private static boolean saveRgbaImage(File outFile, byte[] rgba, int width, int height,
                                         String outputFormat, int jpegQuality) {
        File parent = outFile.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) {
            return false;
        }
        int[] pixels = new int[width * height];
        for (int i = 0; i < pixels.length; i++) {
            int j = i * 4;
            int r = rgba[j] & 0xff;
            int g = rgba[j + 1] & 0xff;
            int b = rgba[j + 2] & 0xff;
            int a = rgba[j + 3] & 0xff;
            pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
        }
        Bitmap outBitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        outBitmap.setPixels(pixels, 0, width, 0, 0, width, height);
        try (FileOutputStream fos = new FileOutputStream(outFile, false)) {
            if ("jpg".equals(outputFormat)) {
                return outBitmap.compress(Bitmap.CompressFormat.JPEG, jpegQuality, fos);
            }
            return outBitmap.compress(Bitmap.CompressFormat.PNG, 100, fos);
        } catch (Exception e) {
            Log.w(TAG, "save image failed: " + outFile, e);
            return false;
        } finally {
            outBitmap.recycle();
        }
    }

    private static String outputName(File input, String outputFormat) {
        String name = input.getName();
        int dot = name.lastIndexOf('.');
        return (dot > 0 ? name.substring(0, dot) : name) + ("jpg".equals(outputFormat) ? ".jpg" : ".png");
    }

    private static void cleanOutputDir(File dir) {
        deleteRecursively(dir);
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
    }

    private static void deleteRecursively(File file) {
        if (file == null || !file.exists()) return;
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) {
                for (File child : children) {
                    deleteRecursively(child);
                }
            }
        }
        //noinspection ResultOfMethodCallIgnored
        file.delete();
    }

    private static void writeText(File file, String text) {
        try (FileOutputStream fos = new FileOutputStream(file, false)) {
            fos.write(text.getBytes(StandardCharsets.UTF_8));
        } catch (Exception e) {
            Log.w(TAG, "write text failed: " + file, e);
        }
    }

    private static int countNonZero(byte[] data) {
        int nonZero = 0;
        for (byte b : data) {
            if (b != 0) {
                nonZero++;
            }
        }
        return nonZero;
    }

    private static void adoptStoragePermissions() {
        try {
            InstrumentationRegistry.getInstrumentation().getUiAutomation().adoptShellPermissionIdentity(
                    Manifest.permission.MANAGE_EXTERNAL_STORAGE,
                    Manifest.permission.READ_EXTERNAL_STORAGE,
                    "android.permission.READ_MEDIA_IMAGES");
        } catch (Exception e) {
            Log.w(TAG, "adopt storage permissions failed: " + e.getMessage());
        }
    }

    private static String resolveModelPath(Context context) {
        return resolveModelPath(context, CPU_MODEL);
    }

    private static String resolveModelPath(Context context, String modelName) {
        File externalModels = context.getExternalFilesDir("models");
        if (externalModels != null) {
            File model = new File(externalModels, modelName);
            if (canRead(model)) {
                return model.getAbsolutePath();
            }
        }

        File internalModels = new File(context.getFilesDir(), "models");
        File internalModel = new File(internalModels, modelName);
        if (canRead(internalModel)) {
            return internalModel.getAbsolutePath();
        }

        File docs = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS);
        File docsModel = new File(docs, modelName);
        if (externalModels != null) {
            File mirrored = new File(externalModels, modelName);
            shellCopyModelToAppExternal(docsModel, mirrored);
            if (canRead(mirrored)) {
                return mirrored.getAbsolutePath();
            }
        }
        if (externalModels != null && canRead(docsModel)) {
            File mirrored = new File(externalModels, modelName);
            if (copyModel(docsModel, mirrored) && canRead(mirrored)) {
                return mirrored.getAbsolutePath();
            }
        }
        return docsModel.getAbsolutePath();
    }

    private static String resolveOverrideModelPath(Context context, String overridePath, String defaultModelName) {
        if (overridePath == null || overridePath.trim().isEmpty()) {
            return resolveModelPath(context, defaultModelName);
        }
        File source = new File(overridePath.trim());
        if (canRead(source)) {
            return source.getAbsolutePath();
        }
        File externalModels = context.getExternalFilesDir("models");
        if (externalModels != null) {
            File mirrored = new File(externalModels, source.getName());
            shellCopyModelToAppExternal(source, mirrored);
            if (canRead(mirrored)) {
                return mirrored.getAbsolutePath();
            }
            if (copyModel(source, mirrored) && canRead(mirrored)) {
                return mirrored.getAbsolutePath();
            }
        }
        return source.getAbsolutePath();
    }

    private static String resolveFirstReadableModelPath(Context context, String[] modelNames) {
        String fallback = "";
        for (String modelName : modelNames) {
            String path = resolveModelPath(context, modelName);
            if (fallback.isEmpty()) {
                fallback = path;
            }
            if (canRead(new File(path))) {
                return path;
            }
        }
        return fallback;
    }

    private static boolean isNonIntegerBelow2(float scale) {
        return Math.abs(scale - Math.round(scale)) > 0.0001f && scale < 2.0f;
    }

    private static void shellCopyModelToAppExternal(File source, File target) {
        try {
            File parent = target.getParentFile();
            String command = "mkdir -p '" + parent.getAbsolutePath() + "'"
                    + " && cp '" + source.getAbsolutePath() + "' '" + target.getAbsolutePath() + "'"
                    + " && chmod 644 '" + target.getAbsolutePath() + "'";
            ParcelFileDescriptor fd = InstrumentationRegistry.getInstrumentation()
                    .getUiAutomation()
                    .executeShellCommand(command);
            if (fd != null) {
                fd.close();
            }
            Thread.sleep(300);
        } catch (Exception e) {
            Log.w(TAG, "shell model copy failed: " + e.getMessage());
        }
    }

    private static boolean canRead(File file) {
        return file.exists() && file.length() > 0 && file.canRead();
    }

    private static boolean copyModel(File source, File target) {
        try {
            File parent = target.getParentFile();
            if (parent != null && !parent.exists() && !parent.mkdirs()) {
                return false;
            }
            if (target.exists() && target.length() == source.length()) {
                return true;
            }
            try (InputStream in = new FileInputStream(source);
                 OutputStream out = new FileOutputStream(target, false)) {
                byte[] buffer = new byte[64 * 1024];
                int n;
                while ((n = in.read(buffer)) > 0) {
                    out.write(buffer, 0, n);
                }
                out.flush();
            }
            return true;
        } catch (Exception e) {
            Log.w(TAG, "copy model failed: " + e.getMessage());
            return false;
        }
    }

    private static final class BatchCase {
        final String outputName;
        final int mode;
        final String modelPath;
        final int backend;
        final boolean sgsr;

        BatchCase(String outputName, int mode, String modelPath, int backend, boolean sgsr) {
            this.outputName = outputName;
            this.mode = mode;
            this.modelPath = modelPath;
            this.backend = backend;
            this.sgsr = sgsr;
        }
    }

    private static final class BatchResult {
        int saved;
        int failures;
        int blackSource;
        int blackOutput;
        int blackFromNonBlack;
        int cropCheckFailures;
        boolean ok;
    }

    private static final class CropCheck {
        final double fullFrameMae;
        final double topLeftCropMae;
        final boolean ok;

        CropCheck(double fullFrameMae, double topLeftCropMae, boolean ok) {
            this.fullFrameMae = fullFrameMae;
            this.topLeftCropMae = topLeftCropMae;
            this.ok = ok;
        }

        static CropCheck skipped() {
            return new CropCheck(0.0, 0.0, true);
        }

        static CropCheck fromRgba(byte[] source, int sourceW, int sourceH,
                                  byte[] output, int outputW, int outputH) {
            int stepX = Math.max(1, outputW / 64);
            int stepY = Math.max(1, outputH / 64);
            double fullErr = 0.0;
            double cropErr = 0.0;
            int samples = 0;
            double cropMaxX = Math.max(0.0, (sourceW - 1) * 0.5);
            double cropMaxY = Math.max(0.0, (sourceH - 1) * 0.5);
            for (int y = stepY / 2; y < outputH; y += stepY) {
                for (int x = stepX / 2; x < outputW; x += stepX) {
                    double ox = outputW <= 1 ? 0.0 : (double) x / (double) (outputW - 1);
                    double oy = outputH <= 1 ? 0.0 : (double) y / (double) (outputH - 1);
                    double fullX = ox * (sourceW - 1);
                    double fullY = oy * (sourceH - 1);
                    double cropX = ox * cropMaxX;
                    double cropY = oy * cropMaxY;
                    double outLuma = lumaAtNearest(output, outputW, outputH, x, y);
                    fullErr += Math.abs(outLuma - lumaAtBilinear(source, sourceW, sourceH, fullX, fullY));
                    cropErr += Math.abs(outLuma - lumaAtBilinear(source, sourceW, sourceH, cropX, cropY));
                    samples++;
                }
            }
            if (samples == 0) {
                return skipped();
            }
            double fullMae = fullErr / samples;
            double cropMae = cropErr / samples;
            return new CropCheck(fullMae, cropMae, fullMae < cropMae * 0.90);
        }

        private static double lumaAtNearest(byte[] rgba, int width, int height, int x, int y) {
            int cx = Math.max(0, Math.min(width - 1, x));
            int cy = Math.max(0, Math.min(height - 1, y));
            return lumaAt(rgba, width, cx, cy);
        }

        private static double lumaAtBilinear(byte[] rgba, int width, int height, double x, double y) {
            double clampedX = Math.max(0.0, Math.min(width - 1, x));
            double clampedY = Math.max(0.0, Math.min(height - 1, y));
            int x0 = (int) Math.floor(clampedX);
            int y0 = (int) Math.floor(clampedY);
            int x1 = Math.min(width - 1, x0 + 1);
            int y1 = Math.min(height - 1, y0 + 1);
            double wx = clampedX - x0;
            double wy = clampedY - y0;
            double top = lumaAt(rgba, width, x0, y0) * (1.0 - wx) + lumaAt(rgba, width, x1, y0) * wx;
            double bottom = lumaAt(rgba, width, x0, y1) * (1.0 - wx) + lumaAt(rgba, width, x1, y1) * wx;
            return top * (1.0 - wy) + bottom * wy;
        }

        private static double lumaAt(byte[] rgba, int width, int x, int y) {
            int i = (y * width + x) * 4;
            int r = rgba[i] & 0xff;
            int g = rgba[i + 1] & 0xff;
            int b = rgba[i + 2] & 0xff;
            return 0.299 * r + 0.587 * g + 0.114 * b;
        }
    }

    private static final class ImageStats {
        final int minRgb;
        final int maxRgb;
        final double nonBlackRatio;

        ImageStats(int minRgb, int maxRgb, double nonBlackRatio) {
            this.minRgb = minRgb;
            this.maxRgb = maxRgb;
            this.nonBlackRatio = nonBlackRatio;
        }

        static ImageStats fromRgba(byte[] rgba) {
            int min = 255;
            int max = 0;
            int nonBlackPixels = 0;
            int pixels = rgba.length / 4;
            for (int i = 0; i < pixels; i++) {
                int j = i * 4;
                int r = rgba[j] & 0xff;
                int g = rgba[j + 1] & 0xff;
                int b = rgba[j + 2] & 0xff;
                int localMax = Math.max(r, Math.max(g, b));
                int localMin = Math.min(r, Math.min(g, b));
                min = Math.min(min, localMin);
                max = Math.max(max, localMax);
                if (localMax > 2) {
                    nonBlackPixels++;
                }
            }
            double ratio = pixels == 0 ? 0.0 : (double) nonBlackPixels / (double) pixels;
            return new ImageStats(min, max, ratio);
        }

        boolean isBlackScreen() {
            return maxRgb <= 2 || nonBlackRatio < 0.001;
        }
    }
}
