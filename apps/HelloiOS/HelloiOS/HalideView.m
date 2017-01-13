//
//  HalideView.m
//  Halide test
//
//  Created by Andrew Adams on 7/23/14.
//  Copyright (c) 2014 Andrew Adams. All rights reserved.
//


#import "HalideView.h"

#include "HalideBuffer.h"
#include "HalideRuntime.h"
#include "HalideRuntimeMetal.h"
#include "reaction_diffusion_2_init.h"
#include "reaction_diffusion_2_render.h"
#include "reaction_diffusion_2_update.h"
#if HAS_METAL_SDK
#include "reaction_diffusion_2_metal_init.h"
#include "reaction_diffusion_2_metal_render.h"
#include "reaction_diffusion_2_metal_update.h"
#endif

using Halide::Runtime::Buffer;

@implementation HalideView
{
@private
#if HAS_METAL_SDK
    __weak CAMetalLayer *_metalLayer;
#endif  // HAS_METAL_SDK
    
    Buffer<float> buf1;
    Buffer<float> buf2;
    Buffer<uint8_t> pixel_buf;
    
    int32_t iteration;
    
    double lastFrameTime;
    double frameElapsedEstimate;
}


#if HAS_METAL_SDK
+ (Class)layerClass
{
    return [CAMetalLayer class];
}
#endif  // HAS_METAL_SDK

- (void)initCommon
{
    self.opaque          = YES;
    self.backgroundColor = nil;

#if HAS_METAL_SDK
    _metalLayer = (CAMetalLayer *)self.layer;
    _metalLayer.delegate = self;

    _device = MTLCreateSystemDefaultDevice();
    _commandQueue = [_device newCommandQueue];
    
    _metalLayer.device      = _device;
    _metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    
    _metalLayer.framebufferOnly = NO;
#endif  // HAS_METAL_SDK

    iteration = 0;
    lastFrameTime = -1;
    frameElapsedEstimate = -1;
}

- (void)initBufsWithWidth: (int)w height: (int)h
{
    NSLog(@"InitBufs: %d %d", w, h);

    // Make a pair of buffers to represent the current state
    if (self.use_metal) {
        buf1 = Buffer<float>::make_interleaved(w, h, 3);
        buf2 = Buffer<float>::make_interleaved(w, h, 3);
    } else {
        buf1 = Buffer<float>(w, h, 3);
        buf2 = Buffer<float>(w, h, 3);
    }

    // We really only need to pad this for the use_metal case,
    // but it doesn't really hurt to always do it.
    const int c = 4;
    const int pad_pixels = (64 / sizeof(int32_t));
    const int row_stride = (w + pad_pixels - 1) & ~(pad_pixels - 1);
    const halide_dimension_t pixelBufShape[] = {
        {0, w, c},
        {0, h, c * row_stride},
        {0, c, 1}
    };

    // This allows us to make a Buffer with an arbitrary shape
    // and memory managed by Buffer itself
    pixel_buf = Buffer<uint8_t>(nullptr, 3, pixelBufShape);
    pixel_buf.allocate();
}

- (void)didMoveToWindow
{
    self.contentScaleFactor = self.window.screen.nativeScale;
}

- (id)initWithFrame:(CGRect)frame
{
    self = [super initWithFrame:frame];
    if(self)
    {
        [self initCommon];
    }
    return self;
}

- (instancetype)initWithCoder:(NSCoder *)coder
{
    self = [super initWithCoder:coder];
    
    if(self)
    {
        [self initCommon];
    }
    return self;
}

- (void)setContentScaleFactor:(CGFloat)contentScaleFactor
{
    [super setContentScaleFactor:contentScaleFactor];
}

- (void)updateLogWith: (double) elapsedTime
{
#if HAS_METAL_SDK
    const char* mode = self.use_metal ? "Metal" : "CPU";
    const char* other = !self.use_metal ? "Metal" : "CPU";
#else
    const char* mode = "CPU";
    const char* other = "CPU";
#endif
    char log_text[2048];
    snprintf(log_text, sizeof(log_text),
         "Halide routine takes %0.3f ms (%s) [Double-tap for %s]\n", elapsedTime * 1000, mode, other);
    [self.outputLog setText: [NSString stringWithUTF8String:log_text]];
}

- (void)touchesBegan:(NSSet *)touches withEvent:(UIEvent *)event {
    UITouch* touch = [touches anyObject];
    self.touch_position = [touch locationInView:self];
    self.touch_active = [self pointInside:self.touch_position withEvent:event];
#if HAS_METAL_SDK
    NSUInteger numTaps = [touch tapCount];
    if (numTaps > 1) {
        self.use_metal = !self.use_metal;
    }
    NSLog(@"TBTaps: %d, self.use_metal %d", (int)numTaps, (int)self.use_metal);
#endif
}

- (void)touchesMoved:(NSSet *)touches withEvent:(UIEvent *)event {
    self.touch_position = [touches.anyObject locationInView:self];
    self.touch_active = [self pointInside:self.touch_position withEvent:event];
}

- (void)touchesEnded:(NSSet *)touches withEvent:(UIEvent *)event {
    self.touch_active = false;
}

- (void)touchesCancelled:(NSSet *)touches withEvent:(UIEvent *)event {
    self.touch_active = false;
}

#if HAS_METAL_SDK
- (void)initiateRenderMetal {
    // NSLog(@"initiateRenderMetal");
    assert(self.use_metal);

    // Create autorelease pool per frame to avoid possible deadlock situations
    // because there are 3 CAMetalDrawables sitting in an autorelease pool.
    @autoreleasepool
    {
        id <CAMetalDrawable> drawable = [_metalLayer nextDrawable];
        id <MTLTexture> texture = drawable.texture;
 
        // handle display changes here
        if (texture.width != buf1.dim(0).extent() ||
            texture.height != buf1.dim(1).extent() ||
            buf1.dim(0).stride() != 3) {
 
            // set the metal layer to the drawable size in case orientation or size changes
            CGSize drawableSize = self.bounds.size;
            // Metal schedule requires that the image be exact multiples of 8 in x & y
            drawableSize.width  = ((long)drawableSize.width + 7) & ~7;
            drawableSize.height  = ((long)drawableSize.height + 7) & ~7;
            _metalLayer.drawableSize = drawableSize;
            
            [self initBufsWithWidth:drawableSize.width height:drawableSize.height];
            reaction_diffusion_2_metal_init((__bridge void *)self, buf1);
            
            iteration = 0;
            lastFrameTime = -1;
            frameElapsedEstimate = -1;
        }
        
        // Grab the current touch position (or leave it far off-screen if there isn't one)
        int tx = -100, ty = -100;
        if (self.touch_active) {
            tx = (int)self.touch_position.x;
            ty = (int)self.touch_position.y;
        }
            
        reaction_diffusion_2_metal_update((__bridge void *)self, buf1, tx, ty, iteration++, buf2);
        reaction_diffusion_2_metal_render((__bridge void *)self, buf2, pixel_buf);

        std::swap(buf1, buf2);

        id <MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
        id <MTLBlitCommandEncoder> blitEncoder = [commandBuffer blitCommandEncoder];

        MTLSize image_size;
        image_size.width = pixel_buf.dim(0).extent();
        image_size.height = pixel_buf.dim(1).extent();
        image_size.depth = 1;
        MTLOrigin origin = { 0, 0, 0 };

        id <MTLBuffer> buffer = (__bridge id <MTLBuffer>)(void *)halide_metal_get_buffer((void *)&self, pixel_buf);
        [blitEncoder 
            copyFromBuffer:buffer 
            sourceOffset: 0
            sourceBytesPerRow: pixel_buf.dim(1).stride() * pixel_buf.type().bits / 8
            sourceBytesPerImage: pixel_buf.size_in_bytes()
            sourceSize: image_size 
            toTexture: texture
            destinationSlice: 0 
            destinationLevel: 0 
            destinationOrigin: origin];
        [blitEncoder endEncoding];
        [commandBuffer addCompletedHandler: ^(id MTLCommandBuffer) {
            dispatch_async(dispatch_get_main_queue(), ^(void) {
                [self displayRenderMetal:drawable];
            });
        }];
        [commandBuffer commit];
        [_commandQueue insertDebugCaptureBoundary];
    }
}

- (void)displayRenderMetal:(id <MTLDrawable>)drawable
{
    [drawable present];
    double frameTime = CACurrentMediaTime();
    
    if (lastFrameTime == -1) {
        lastFrameTime = frameTime;
    } else {
        double t_elapsed = (frameTime - lastFrameTime) + (frameTime - lastFrameTime);
        lastFrameTime = frameTime;
        lastFrameTime = frameTime;
        // Smooth elapsed using an IIR
        if (frameElapsedEstimate == -1) {
            frameElapsedEstimate = t_elapsed;
        } else {
            frameElapsedEstimate = (frameElapsedEstimate * 31 + t_elapsed) / 32.0;
        }
        if ((iteration % 30) == 0) {
            [self updateLogWith: frameElapsedEstimate];
        }
    }
    [self initiateRender];
}

#endif  // HAS_METAL_SDK

- (void)initiateRenderCPU {
    // NSLog(@"initiateRenderCPU");

    // Start a background task
    assert(!self.use_metal);

    CGRect box = self.window.frame;
    int image_width = box.size.width;
    int image_height = box.size.height;

    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        // Make a frame buffer
        
        [self initBufsWithWidth:image_width height:image_height];

        CGDataProviderRef provider =
            CGDataProviderCreateWithData(NULL, pixel_buf.data(), pixel_buf.size_in_bytes(), NULL);

        CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();

        double t_estimate = 0.0;
        
        reaction_diffusion_2_init(buf1);
   
        for (int i = 0; ; i++) {
  
            // Grab the current touch position (or leave it far off-screen if there isn't one)
            int tx = -100, ty = -100;
            if (self.touch_active) {
                tx = (int)self.touch_position.x;
                ty = (int)self.touch_position.y;
            }
            
            double t_before = CACurrentMediaTime();
            reaction_diffusion_2_update(buf1, tx, ty, i, buf2);
            reaction_diffusion_2_render(buf2, pixel_buf);
            double t_after = CACurrentMediaTime();

            pixel_buf.copy_to_host();
            
            double t_elapsed = (t_after - t_before);
            
            // Smooth elapsed using an IIR
            if (i == 0) t_estimate = t_elapsed;
            else t_estimate = (t_estimate * 31 + t_elapsed) / 32.0;
            
            const int bytesPerRow = pixel_buf.dim(1).stride() * pixel_buf.type().bits / 8;
            CGImageRef image_ref =
                CGImageCreate(image_width, image_height, 
                              8,   // bitsPerComponent
                              32,  // bitsPerPixel
                              bytesPerRow,
                              color_space,
                              kCGBitmapByteOrderDefault,
                              provider, NULL, NO,
                              kCGRenderingIntentDefault);
            UIImage *im = [UIImage imageWithCGImage:image_ref];
            CGImageRelease(image_ref);

            std::swap(buf1, buf2);
            
            if (!self.use_metal) {
                if (i % 30 == 0) {
                    dispatch_async(dispatch_get_main_queue(), ^(void) {
                       [self updateLogWith: t_estimate];
                    });
                }
                dispatch_async(dispatch_get_main_queue(), ^(void) {
                    [self setImage:im];
                });
            } else {
                NSLog(@"Exiting CPU Render");
                dispatch_async(dispatch_get_main_queue(), ^(void) {
                    [self initiateRender];
                });
                return;
            }

        }
    });
}

- (void)initiateRender {
    // NSLog(@"initiateRender");
#if HAS_METAL_SDK
    if (self.use_metal) {
        [self initiateRenderMetal];
        return;
    }
#endif

    [self initiateRenderCPU];
}

@end

#if HAS_METAL_SDK

#ifdef __cplusplus
extern "C" {
#endif

int halide_metal_acquire_context(void *user_context, struct halide_metal_device **device_ret,
                                 struct halide_metal_command_queue **queue_ret, bool create) {
    HalideView *view = (__bridge HalideView *)user_context;
    *device_ret = (__bridge struct halide_metal_device *)view.device;
    *queue_ret = (__bridge struct halide_metal_command_queue *)view.commandQueue;
    return 0;
}

int halide_metal_release_context(void *user_context) {
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif  // HAS_METAL_SDK
