#import "ViewController.h"

#import <AVFoundation/AVFoundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#import <math.h>
#import <stdlib.h>
#import <string.h>

#include "mc_interface.h"
#include <stdint.h>

static const int32_t kInputWidth = 1920;
static const int32_t kInputHeight = 1080;
static const NSTimeInterval kScaleApplyDelay = 0.08;

@interface ViewController () <AVCaptureVideoDataOutputSampleBufferDelegate>
@property(nonatomic, strong) UIImageView *imageView;
@property(nonatomic, strong) UILabel *statusLabel;
@property(nonatomic, strong) UISegmentedControl *modeControl;
@property(nonatomic, strong) UISlider *scaleSlider;
@property(nonatomic, strong) UILabel *scaleLabel;
@property(nonatomic, strong) UIView *scaleTickStrip;
@property(nonatomic, strong) NSArray<UIView *> *scaleTickMarks;
@property(nonatomic, strong) NSArray<UILabel *> *scaleTickLabels;
@property(nonatomic) BOOL scaleDragging;
@property(nonatomic, strong) AVCaptureSession *session;
@property(nonatomic, strong) dispatch_queue_t captureQueue;
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic) alg_mode_e selectedMode;
@property(nonatomic) float selectedScale;
@property(nonatomic, strong) id<MTLTexture> inputTexture;
@property(nonatomic, strong) id<MTLTexture> outputTexture;
@property(nonatomic) void *srHandle;
@property(nonatomic) float engineScale;
@property(nonatomic) alg_mode_e engineMode;
@property(nonatomic, strong) NSMutableData *fullRgbaData;
@property(nonatomic, strong) NSMutableData *cropRgbaData;
@property(nonatomic, strong) NSMutableData *outputRgbaData;
@property(nonatomic) BOOL processingFrame;
@property(nonatomic) size_t fullFrameWidth;
@property(nonatomic) size_t fullFrameHeight;
@property(nonatomic) size_t inputTexWidth;
@property(nonatomic) size_t inputTexHeight;
@property(nonatomic) size_t outputTexWidth;
@property(nonatomic) size_t outputTexHeight;
@property(nonatomic) BOOL loggedFirstProcess;
@end

@implementation ViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    self.captureQueue = dispatch_queue_create("magic.magnifier.capture", DISPATCH_QUEUE_SERIAL);
    self.device = MTLCreateSystemDefaultDevice();
    self.selectedMode = SPATIAL_SPEED_MODE;
    self.selectedScale = 2.0f;
    self.srHandle = NULL;
    self.fullFrameWidth = 0;
    self.fullFrameHeight = 0;
    self.inputTexWidth = 0;
    self.inputTexHeight = 0;
    self.outputTexWidth = 0;
    self.outputTexHeight = 0;
    self.engineScale = -1.0f;
    self.engineMode = (alg_mode_e)MAX_ALG_MODE;
    self.loggedFirstProcess = NO;
    NSLog(@"[MagicMagnifierSR] loaded mc_nscaler_version=%s", mc_nscaler_version());
    [self buildUi];

    @try {
        [self ensureModelsInSandbox];
    } @catch (NSException *exception) {
        [self failAndStop:[NSString stringWithFormat:@"error: %@", exception.reason ?: @"model setup failed"]];
        return;
    }
    [self requestCameraAndStart];
}

- (void)dealloc {
    [self.session stopRunning];
    if (self.captureQueue) {
        dispatch_sync(self.captureQueue, ^{
            [self restartEngineUnsafe];
        });
    } else {
        [self restartEngineUnsafe];
    }
}

- (void)buildUi {
    self.imageView = [[UIImageView alloc] initWithFrame:self.view.bounds];
    self.imageView.contentMode = UIViewContentModeScaleAspectFit;
    self.imageView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.imageView.backgroundColor = UIColor.blackColor;
    [self.view addSubview:self.imageView];

    UIView *panel = [[UIView alloc] initWithFrame:CGRectZero];
    panel.backgroundColor = [[UIColor blackColor] colorWithAlphaComponent:0.6];
    panel.layer.cornerRadius = 10.0;
    panel.clipsToBounds = YES;
    panel.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:panel];

    UIStackView *stack = [[UIStackView alloc] initWithFrame:CGRectZero];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 10;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [panel addSubview:stack];

    self.modeControl = [[UISegmentedControl alloc] initWithItems:@[@"speed", @"balanced"]];
    self.modeControl.selectedSegmentIndex = 0;
    [self.modeControl addTarget:self action:@selector(modeChanged:) forControlEvents:UIControlEventValueChanged];

    UILabel *versionLabel = [[UILabel alloc] initWithFrame:CGRectZero];
    versionLabel.text = [NSString stringWithFormat:@"MagicSR %s", mc_nscaler_version()];
    versionLabel.textColor = [UIColor.whiteColor colorWithAlphaComponent:0.85];
    versionLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightBold];
    versionLabel.textAlignment = NSTextAlignmentRight;

    UIStackView *topRow = [[UIStackView alloc] initWithFrame:CGRectZero];
    topRow.axis = UILayoutConstraintAxisHorizontal;
    topRow.alignment = UIStackViewAlignmentCenter;
    topRow.spacing = 8;
    [topRow addArrangedSubview:self.modeControl];
    [topRow addArrangedSubview:versionLabel];
    [stack addArrangedSubview:topRow];

    UIStackView *scaleRow = [[UIStackView alloc] initWithFrame:CGRectZero];
    scaleRow.axis = UILayoutConstraintAxisHorizontal;
    scaleRow.spacing = 8;
    scaleRow.alignment = UIStackViewAlignmentTop;
    self.scaleLabel = [[UILabel alloc] initWithFrame:CGRectZero];
    self.scaleLabel.textColor = UIColor.whiteColor;
    self.scaleLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
    [scaleRow addArrangedSubview:self.scaleLabel];

    UIStackView *seekCol = [[UIStackView alloc] initWithFrame:CGRectZero];
    seekCol.axis = UILayoutConstraintAxisVertical;
    seekCol.spacing = 2;
    seekCol.alignment = UIStackViewAlignmentFill;

    self.scaleSlider = [[UISlider alloc] initWithFrame:CGRectZero];
    self.scaleSlider.minimumValue = 1.0f;
    self.scaleSlider.maximumValue = 8.0f;
    self.scaleSlider.value = self.selectedScale;
    self.scaleSlider.continuous = YES;
    [self.scaleSlider addTarget:self action:@selector(scaleTouchDown:) forControlEvents:UIControlEventTouchDown];
    [self.scaleSlider addTarget:self action:@selector(scaleDrag:) forControlEvents:UIControlEventTouchDragInside | UIControlEventTouchDragOutside];
    [self.scaleSlider addTarget:self action:@selector(scaleChanged:) forControlEvents:UIControlEventValueChanged];
    [self.scaleSlider addTarget:self action:@selector(scaleTouchUp:) forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside | UIControlEventTouchCancel];
    [seekCol addArrangedSubview:self.scaleSlider];

    self.scaleTickStrip = [[UIView alloc] initWithFrame:CGRectZero];
    [self.scaleTickStrip.heightAnchor constraintEqualToConstant:24].active = YES;
    NSMutableArray<UIView *> *tickMarks = [NSMutableArray arrayWithCapacity:8];
    NSMutableArray<UILabel *> *tickLabels = [NSMutableArray arrayWithCapacity:8];
    for (NSInteger tick = 1; tick <= 8; tick++) {
        UIView *mark = [[UIView alloc] initWithFrame:CGRectZero];
        mark.backgroundColor = [[UIColor whiteColor] colorWithAlphaComponent:0.75];
        mark.tag = tick;
        [self.scaleTickStrip addSubview:mark];
        [tickMarks addObject:mark];

        UILabel *tickLabel = [[UILabel alloc] initWithFrame:CGRectZero];
        tickLabel.text = [NSString stringWithFormat:@"%ld", (long)tick];
        tickLabel.textColor = [[UIColor whiteColor] colorWithAlphaComponent:0.85];
        tickLabel.font = [UIFont monospacedDigitSystemFontOfSize:11 weight:UIFontWeightRegular];
        tickLabel.textAlignment = NSTextAlignmentCenter;
        tickLabel.tag = tick;
        tickLabel.userInteractionEnabled = YES;
        UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(scaleTickTapped:)];
        [tickLabel addGestureRecognizer:tap];
        [self.scaleTickStrip addSubview:tickLabel];
        [tickLabels addObject:tickLabel];
    }
    self.scaleTickMarks = tickMarks;
    self.scaleTickLabels = tickLabels;
    UITapGestureRecognizer *stripTap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(scaleTickStripTapped:)];
    [self.scaleTickStrip addGestureRecognizer:stripTap];
    [seekCol addArrangedSubview:self.scaleTickStrip];
    [scaleRow addArrangedSubview:seekCol];
    [stack addArrangedSubview:scaleRow];

    self.statusLabel = [[UILabel alloc] initWithFrame:CGRectZero];
    self.statusLabel.textColor = UIColor.whiteColor;
    self.statusLabel.font = [UIFont monospacedDigitSystemFontOfSize:12 weight:UIFontWeightRegular];
    self.statusLabel.numberOfLines = 2;
    self.statusLabel.text = @"initializing";
    [stack addArrangedSubview:self.statusLabel];

    [NSLayoutConstraint activateConstraints:@[
        [panel.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor constant:12],
        [panel.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-12],
        [panel.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor constant:8],
        [stack.leadingAnchor constraintEqualToAnchor:panel.leadingAnchor constant:12],
        [stack.trailingAnchor constraintEqualToAnchor:panel.trailingAnchor constant:-12],
        [stack.topAnchor constraintEqualToAnchor:panel.topAnchor constant:12],
        [stack.bottomAnchor constraintEqualToAnchor:panel.bottomAnchor constant:-12],
    ]];
    [self updateScaleLabel];
    [self.view setNeedsLayout];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    [self layoutScaleTickLabels];
}

- (void)layoutScaleTickLabels {
    if (self.scaleTickStrip.bounds.size.width <= 1.0 || self.scaleTickLabels.count == 0) return;
    CGFloat trackW = CGRectGetWidth(self.scaleSlider.bounds);
    // Approximate UISlider track inset so ticks align with thumb travel.
    CGFloat inset = 14.0;
    CGFloat usable = MAX(1.0, trackW - inset * 2.0);
    CGFloat stripW = CGRectGetWidth(self.scaleTickStrip.bounds);
    CGFloat originX = (stripW - trackW) * 0.5 + inset;
    CGFloat labelW = 18.0;
    CGFloat markH = 6.0;
    CGFloat stripH = CGRectGetHeight(self.scaleTickStrip.bounds);
    NSInteger minTick = 1;
    NSInteger maxTick = 8;
    NSInteger span = maxTick - minTick;
    for (NSUInteger i = 0; i < self.scaleTickLabels.count; i++) {
        UILabel *label = self.scaleTickLabels[i];
        UIView *mark = (i < self.scaleTickMarks.count) ? self.scaleTickMarks[i] : nil;
        NSInteger tick = label.tag;
        CGFloat t = span > 0 ? (CGFloat)(tick - minTick) / (CGFloat)span : 0.0;
        CGFloat centerX = originX + t * usable;
        if (mark) {
            mark.frame = CGRectMake(centerX - 0.5, 0, 1.0, markH);
        }
        label.frame = CGRectMake(centerX - labelW * 0.5, markH, labelW, MAX(1.0, stripH - markH));
    }
}

- (void)modeChanged:(UISegmentedControl *)sender {
    self.selectedMode = sender.selectedSegmentIndex == 1 ? SPATIAL_BALANCED_MODE : SPATIAL_SPEED_MODE;
    [self restartEngine];
}

- (void)scaleTouchDown:(UISlider *)sender {
    self.scaleDragging = NO;
}

- (void)scaleDrag:(UISlider *)sender {
    self.scaleDragging = YES;
}

- (void)scaleChanged:(UISlider *)sender {
    self.selectedScale = MAX(1.0f, MIN(8.0f, sender.value));
    [self updateScaleLabel];
    if (self.scaleDragging) {
        [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(restartEngine) object:nil];
        [self performSelector:@selector(restartEngine) withObject:nil afterDelay:kScaleApplyDelay];
    }
}

- (void)scaleTouchUp:(UISlider *)sender {
    if (self.scaleDragging) {
        self.selectedScale = MAX(1.0f, MIN(8.0f, sender.value));
        [self updateScaleLabel];
        [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(restartEngine) object:nil];
        [self performSelector:@selector(restartEngine) withObject:nil afterDelay:kScaleApplyDelay];
    } else {
        [self applyIntegerScale:(int)lroundf(sender.value)];
    }
    self.scaleDragging = NO;
}

- (void)scaleTickTapped:(UITapGestureRecognizer *)gesture {
    if (gesture.state != UIGestureRecognizerStateEnded) return;
    [self applyIntegerScale:(int)gesture.view.tag];
}

- (void)scaleTickStripTapped:(UITapGestureRecognizer *)gesture {
    if (gesture.state != UIGestureRecognizerStateEnded) return;
    CGPoint p = [gesture locationInView:self.scaleTickStrip];
    CGFloat trackW = CGRectGetWidth(self.scaleSlider.bounds);
    CGFloat inset = 14.0;
    CGFloat usable = MAX(1.0, trackW - inset * 2.0);
    CGFloat stripW = CGRectGetWidth(self.scaleTickStrip.bounds);
    CGFloat originX = (stripW - trackW) * 0.5 + inset;
    CGFloat unit = (p.x - originX) / usable;
    unit = MAX(0.0, MIN(1.0, unit));
    float scale = 1.0f + (float)unit * 7.0f;
    [self applyIntegerScale:(int)lroundf(scale)];
}

- (void)applyIntegerScale:(int)integerScale {
    int snapped = MAX(1, MIN(8, integerScale));
    self.selectedScale = (float)snapped;
    self.scaleSlider.value = self.selectedScale;
    [self updateScaleLabel];
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(restartEngine) object:nil];
    [self performSelector:@selector(restartEngine) withObject:nil afterDelay:kScaleApplyDelay];
}

- (void)updateScaleLabel {
    self.scaleLabel.text = [NSString stringWithFormat:@"scale=x%@", [self formatScale:self.selectedScale]];
}

- (NSString *)formatScale:(float)value {
    NSString *text = [NSString stringWithFormat:@"%.2f", value];
    while ([text hasSuffix:@"0"]) text = [text substringToIndex:text.length - 1];
    if ([text hasSuffix:@"."]) text = [text substringToIndex:text.length - 1];
    return text;
}

- (void)requestCameraAndStart {
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL granted) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!granted) {
                [self failAndStop:@"error: camera permission denied"];
                return;
            }
            [self startCapture];
        });
    }];
}

- (void)startCapture {
    if (self.session && self.session.isRunning) return;
    self.session = [[AVCaptureSession alloc] init];
    self.session.sessionPreset = AVCaptureSessionPresetInputPriority;

    AVCaptureDeviceDiscoverySession *discovery =
        [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeBuiltInWideAngleCamera]
                                                               mediaType:AVMediaTypeVideo
                                                                position:AVCaptureDevicePositionBack];
    AVCaptureDevice *camera = discovery.devices.firstObject ?: [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
    if (!camera) {
        [self failAndStop:@"error: no camera device"];
        return;
    }

    NSError *error = nil;
    AVCaptureDeviceInput *input = [AVCaptureDeviceInput deviceInputWithDevice:camera error:&error];
    if (!input || error) {
        [self failAndStop:[NSString stringWithFormat:@"error: camera input failed %@", error.localizedDescription ?: @""]];
        return;
    }
    [self configure1080pFormat:camera];

    AVCaptureVideoDataOutput *output = [[AVCaptureVideoDataOutput alloc] init];
    output.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)};
    output.alwaysDiscardsLateVideoFrames = YES;
    [output setSampleBufferDelegate:self queue:self.captureQueue];

    if ([self.session canAddInput:input]) [self.session addInput:input];
    if ([self.session canAddOutput:output]) [self.session addOutput:output];
    AVCaptureConnection *videoConn = [output connectionWithMediaType:AVMediaTypeVideo];
    if (videoConn.isVideoOrientationSupported) {
        videoConn.videoOrientation = AVCaptureVideoOrientationPortrait;
    }
    [self.session startRunning];
    self.statusLabel.text = @"camera ready";
}

- (void)configure1080pFormat:(AVCaptureDevice *)camera {
    AVCaptureDeviceFormat *target = nil;
    for (AVCaptureDeviceFormat *fmt in camera.formats) {
        CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(fmt.formatDescription);
        if (d.width == kInputWidth && d.height == kInputHeight) {
            target = fmt;
            break;
        }
    }
    if (!target) return;
    NSError *err = nil;
    if ([camera lockForConfiguration:&err]) {
        camera.activeFormat = target;
        [camera unlockForConfiguration];
    }
}

- (void)captureOutput:(AVCaptureOutput *)output
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
       fromConnection:(AVCaptureConnection *)connection {
    (void)output;
    (void)connection;
    if (self.processingFrame) return;
    self.processingFrame = YES;

    CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!pb) {
        self.processingFrame = NO;
        return;
    }
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    size_t width = CVPixelBufferGetWidth(pb);
    size_t height = CVPixelBufferGetHeight(pb);

    @try {
        [self ensureFullFrameBufferForWidth:width height:height];
        [self fillFullRgbaFromPixelBuffer:pb];

        // Magnifier: crop center (1/scale), then SR that crop back up.
        size_t cropW = [self evenSize:llround((double)width / (double)self.selectedScale) max:width];
        size_t cropH = [self evenSize:llround((double)height / (double)self.selectedScale) max:height];
        size_t cropX = (width - cropW) / 2;
        size_t cropY = (height - cropH) / 2;
        /* Core output size: floor(value * scale + 0.5) in double (magic_backend.h). */
        size_t outW = [self scaledDimension:cropW scale:self.selectedScale];
        size_t outH = [self scaledDimension:cropH scale:self.selectedScale];
        [self ensureInputTextureForWidth:cropW height:cropH];
        [self ensureOutputTextureForWidth:outW height:outH];
        [self uploadCropToInputTextureFromFullX:cropX y:cropY width:cropW height:cropH];
        [self ensureSessionForWidth:cropW height:cropH scale:self.selectedScale mode:self.selectedMode];
        outW = self.outputTexWidth;
        outH = self.outputTexHeight;

        mc_nscaler_input_frame_t inFrame;
        mc_nscaler_output_frame_t outFrame;
        memset(&inFrame, 0, sizeof(inFrame));
        memset(&outFrame, 0, sizeof(outFrame));
        inFrame.handle.pointer = (__bridge void *)self.inputTexture;
        inFrame.format = (uint32_t)MTLPixelFormatRGBA8Unorm;
        inFrame.mip_count = 1;
        inFrame.width = (unsigned int)cropW;
        inFrame.height = (unsigned int)cropH;
        outFrame.handle.pointer = (__bridge void *)self.outputTexture;
        outFrame.format = (uint32_t)MTLPixelFormatRGBA8Unorm;
        outFrame.mip_count = 1;
        outFrame.width = (unsigned int)outW;
        outFrame.height = (unsigned int)outH;

        void *h = self.srHandle;
        int ret = mc_nscaler_enable(&h, &inFrame, &outFrame);
        self.srHandle = h;
        if (ret != 0) {
            @throw [NSException exceptionWithName:@"MCProcessError"
                                           reason:[NSString stringWithFormat:@"mc_nscaler_enable failed ret=%d scale=%.2f mode=%d",
                                                   ret, self.selectedScale, (int)self.selectedMode]
                                         userInfo:nil];
        }
        if (!self.loggedFirstProcess) {
            self.loggedFirstProcess = YES;
            NSLog(@"[MagicMagnifierSR] mc_nscaler_enable first ok version=%s ret=0 in=%zux%zu out=%zux%zu scale=%.3f mode=%d",
                  mc_nscaler_version(), cropW, cropH, outW, outH, self.selectedScale, (int)self.selectedMode);
        }

        size_t outputBytes = outW * outH * 4;
        if (!self.outputRgbaData || self.outputRgbaData.length != outputBytes) {
            self.outputRgbaData = [NSMutableData dataWithLength:outputBytes];
        }
        /* Safe: spatial mc_nscaler_enable returned after GPU waitUntilCompleted. */
        [self.outputTexture getBytes:self.outputRgbaData.mutableBytes
                          bytesPerRow:outW * 4
                           fromRegion:MTLRegionMake2D(0, 0, outW, outH)
                          mipmapLevel:0];
        UIImage *img = [self imageFromRgbaData:self.outputRgbaData width:outW height:outH];
        NSString *status = [NSString stringWithFormat:@"ver=%s mode=%@ scale=x%@ crop=%zux%zu out=%zux%zu",
                            mc_nscaler_version(),
                            self.selectedMode == SPATIAL_BALANCED_MODE ? @"balanced" : @"speed",
                            [self formatScale:self.selectedScale],
                            cropW, cropH, outW, outH];
        dispatch_async(dispatch_get_main_queue(), ^{
            self.imageView.image = img;
            self.statusLabel.text = status;
        });
    } @catch (NSException *ex) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self failAndStop:[NSString stringWithFormat:@"error: %@", ex.reason ?: @"process failed"]];
        });
    }

    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    self.processingFrame = NO;
}

- (void)ensureSessionForWidth:(size_t)width height:(size_t)height scale:(float)scale mode:(alg_mode_e)mode {
    if (self.srHandle &&
        self.inputTexWidth == width &&
        self.inputTexHeight == height &&
        self.engineMode == mode &&
        fabsf(self.engineScale - scale) < 0.0001f) {
        return;
    }

    if (self.srHandle) {
        int ur = mc_nscaler_disable(self.srHandle);
        if (ur != 0) {
            NSLog(@"[MagicMagnifierSR] mc_nscaler_disable failed ret=%d", ur);
        }
        self.srHandle = NULL;
    }

    NSString *modelPath = [self modelPathForCurrentMode];
    ctrl_param_t ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.input_type = INPUT_TEXTURE_RGB8Unorm;
    ctrl.scaler_factor = scale;
    ctrl.alg_mode = mode;
    ctrl.log_level = MAGIC_LOG_INFO;
    ctrl.backend = MAGIC_BACKEND_METAL;
    ctrl.spatial_sharpen_level = 0;
    ctrl.gpu_context.device = (__bridge void *)self.device;
    strncpy(ctrl.model_path, modelPath.UTF8String, sizeof(ctrl.model_path) - 1);

    void *h = NULL;
    int rc = mc_nscaler_control(&h, MC_NSCALER_CMD_SET_PARAM, &ctrl, NULL);
    self.srHandle = h;
    if (rc != 0 || !self.srHandle) {
        @throw [NSException exceptionWithName:@"MCInitError"
                                       reason:[NSString stringWithFormat:@"mc_nscaler_control failed rc=%d scale=%.2f mode=%d model=%@",
                                               rc, scale, (int)mode, modelPath]
                                     userInfo:nil];
    }
    size_t coreOutW = [self scaledDimension:width scale:scale];
    size_t coreOutH = [self scaledDimension:height scale:scale];
    if (coreOutW != self.outputTexWidth || coreOutH != self.outputTexHeight) {
        [self ensureOutputTextureForWidth:coreOutW height:coreOutH];
    }
    self.engineScale = scale;
    self.engineMode = mode;
    self.loggedFirstProcess = NO;
    NSLog(@"[MagicMagnifierSR] mc_nscaler_control ok version=%s scale=%.3f mode=%d %zux%zu -> %zux%zu model=%@",
          mc_nscaler_version(), scale, (int)mode, width, height, coreOutW, coreOutH, modelPath);
}

- (size_t)scaledDimension:(size_t)value scale:(float)scaler {
    double scaled = (double)value * (double)scaler;
    if (scaled < 1.0) return 1;
    if (scaled > (double)UINT32_MAX) return (size_t)UINT32_MAX;
    return (size_t)floor(scaled + 0.5);
}

- (size_t)evenSize:(long long)desired max:(size_t)max {
    size_t v = (size_t)MAX(64LL, MIN(desired, (long long)max));
    if (v > max) v = max;
    return v & ~(size_t)1;
}

- (void)ensureFullFrameBufferForWidth:(size_t)width height:(size_t)height {
    size_t bytes = width * height * 4;
    if (self.fullRgbaData && self.fullFrameWidth == width && self.fullFrameHeight == height &&
        self.fullRgbaData.length == bytes) {
        return;
    }
    self.fullRgbaData = [NSMutableData dataWithLength:bytes];
    self.fullFrameWidth = width;
    self.fullFrameHeight = height;
}

- (void)ensureInputTextureForWidth:(size_t)width height:(size_t)height {
    if (self.inputTexture && self.inputTexWidth == width && self.inputTexHeight == height) {
        return;
    }
    MTLTextureDescriptor *inDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                      width:width
                                                                                     height:height
                                                                                  mipmapped:NO];
    inDesc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    self.inputTexture = [self.device newTextureWithDescriptor:inDesc];
    if (!self.inputTexture) {
        @throw [NSException exceptionWithName:@"TextureCreateError" reason:@"failed to create input texture" userInfo:nil];
    }
    self.inputTexWidth = width;
    self.inputTexHeight = height;
}

- (void)ensureOutputTextureForWidth:(size_t)width height:(size_t)height {
    if (self.outputTexture && self.outputTexWidth == width && self.outputTexHeight == height) {
        return;
    }
    MTLTextureDescriptor *outDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                       width:width
                                                                                      height:height
                                                                                   mipmapped:NO];
    outDesc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    self.outputTexture = [self.device newTextureWithDescriptor:outDesc];
    if (!self.outputTexture) {
        @throw [NSException exceptionWithName:@"TextureCreateError" reason:@"failed to create output texture" userInfo:nil];
    }
    self.outputTexWidth = width;
    self.outputTexHeight = height;
}

- (void)fillFullRgbaFromPixelBuffer:(CVPixelBufferRef)pb {
    uint8_t *src = (uint8_t *)CVPixelBufferGetBaseAddress(pb);
    size_t width = CVPixelBufferGetWidth(pb);
    size_t height = CVPixelBufferGetHeight(pb);
    size_t stride = CVPixelBufferGetBytesPerRow(pb);
    uint8_t *dst = (uint8_t *)self.fullRgbaData.mutableBytes;
    for (size_t y = 0; y < height; y++) {
        const uint8_t *row = src + y * stride;
        uint8_t *out = dst + y * width * 4;
        for (size_t x = 0; x < width; x++) {
            const uint8_t b = row[x * 4 + 0];
            const uint8_t g = row[x * 4 + 1];
            const uint8_t r = row[x * 4 + 2];
            const uint8_t a = row[x * 4 + 3];
            out[x * 4 + 0] = r;
            out[x * 4 + 1] = g;
            out[x * 4 + 2] = b;
            out[x * 4 + 3] = a;
        }
    }
}

- (void)uploadCropToInputTextureFromFullX:(size_t)cropX y:(size_t)cropY width:(size_t)cropW height:(size_t)cropH {
    const uint8_t *src = (const uint8_t *)self.fullRgbaData.bytes;
    size_t fullW = self.fullFrameWidth;
    size_t bytes = cropW * cropH * 4;
    if (!self.cropRgbaData || self.cropRgbaData.length != bytes) {
        self.cropRgbaData = [NSMutableData dataWithLength:bytes];
    }
    uint8_t *dst = (uint8_t *)self.cropRgbaData.mutableBytes;
    for (size_t y = 0; y < cropH; y++) {
        memcpy(dst + y * cropW * 4,
               src + ((cropY + y) * fullW + cropX) * 4,
               cropW * 4);
    }
    [self.inputTexture replaceRegion:MTLRegionMake2D(0, 0, cropW, cropH)
                         mipmapLevel:0
                           withBytes:self.cropRgbaData.bytes
                         bytesPerRow:cropW * 4];
}

- (UIImage *)imageFromRgbaData:(NSData *)rgba width:(size_t)width height:(size_t)height {
    if (!rgba || width == 0 || height == 0) return nil;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    if (!cs) return nil;
    CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, rgba.bytes, rgba.length, NULL);
    if (!provider) {
        CGColorSpaceRelease(cs);
        return nil;
    }
    CGImageRef imageRef = CGImageCreate(width,
                                        height,
                                        8,
                                        32,
                                        width * 4,
                                        cs,
                                        kCGBitmapByteOrderDefault | kCGImageAlphaLast,
                                        provider,
                                        NULL,
                                        false,
                                        kCGRenderingIntentDefault);
    UIImage *img = imageRef ? [UIImage imageWithCGImage:imageRef scale:1.0 orientation:UIImageOrientationUp] : nil;
    if (imageRef) CGImageRelease(imageRef);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(cs);
    return img;
}

- (void)ensureModelsInSandbox {
    NSArray<NSString *> *names = @[ @"magic_sr_gpu_params.bin" ];
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSFileManager *fm = [NSFileManager defaultManager];
    for (NSString *name in names) {
        NSString *dst = [docs stringByAppendingPathComponent:name];
        NSString *src = [NSBundle.mainBundle pathForResource:[name stringByDeletingPathExtension]
                                                      ofType:name.pathExtension];
        if (!src) {
            continue;
        }
        NSError *err = nil;
        if ([fm fileExistsAtPath:dst]) {
            [fm removeItemAtPath:dst error:&err];
            err = nil;
        }
        [fm copyItemAtPath:src toPath:dst error:&err];
        if (err || ![fm fileExistsAtPath:dst]) {
            @throw [NSException exceptionWithName:@"ModelCopyFailed"
                                           reason:[NSString stringWithFormat:@"copy failed %@: %@", name, err.localizedDescription ?: @"unknown"]
                                         userInfo:nil];
        }
    }
}

- (NSString *)modelPathForCurrentMode {
    /* Combined public GPU model: BALANCED=seg0, SPEED=seg(1+sharpen). */
    NSArray<NSString *> *names = @[ @"magic_sr_gpu_params.bin" ];
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    for (NSString *name in names) {
        NSString *path = [docs stringByAppendingPathComponent:name];
        NSDictionary *attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil];
        if (attrs && attrs.fileSize > 0) {
            return path;
        }
        NSString *bundle = [NSBundle.mainBundle pathForResource:[name stringByDeletingPathExtension]
                                                       ofType:name.pathExtension];
        if (bundle.length > 0) {
            return bundle;
        }
    }
    @throw [NSException exceptionWithName:@"ModelMissing"
                               reason:[NSString stringWithFormat:@"no Metal spatial model for mode=%d", (int)self.selectedMode]
                             userInfo:nil];
}

- (void)restartEngine {
    dispatch_async(self.captureQueue, ^{
        [self restartEngineUnsafe];
    });
}

- (void)restartEngineUnsafe {
    if (self.srHandle) {
        int ur = mc_nscaler_disable(self.srHandle);
        if (ur != 0) {
            NSLog(@"[MagicMagnifierSR] mc_nscaler_disable failed ret=%d", ur);
        }
        self.srHandle = NULL;
    }
    self.inputTexture = nil;
    self.outputTexture = nil;
    self.fullRgbaData = nil;
    self.cropRgbaData = nil;
    self.outputRgbaData = nil;
    self.fullFrameWidth = 0;
    self.fullFrameHeight = 0;
    self.inputTexWidth = 0;
    self.inputTexHeight = 0;
    self.outputTexWidth = 0;
    self.outputTexHeight = 0;
    self.engineScale = -1.0f;
    self.engineMode = (alg_mode_e)MAX_ALG_MODE;
    self.loggedFirstProcess = NO;
}

- (void)failAndStop:(NSString *)message {
    [self.session stopRunning];
    dispatch_sync(self.captureQueue, ^{
        [self restartEngineUnsafe];
    });
    NSLog(@"[MagicMagnifierSR] failAndStop: %@", message ?: @"nil");
    self.statusLabel.text = message.length ? message : @"error";
}

@end
