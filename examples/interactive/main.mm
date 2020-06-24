// Old-school, minimalist 'framework' for interactive examples that use `llair`
// and then draw stuff using the Metal API.

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MTKView.h>

// The linked object file(s) must implement these:
extern void llair_example_init(int, const char *[], id<MTLDevice>, MTKView *);
extern void llair_example_exit();
extern void llair_example_resize(CGSize);
extern void llair_example_draw(MTLRenderPassDescriptor *, id<MTLDrawable>);

unsigned width = 960, height = 540;

@interface Delegate : NSObject<NSWindowDelegate, MTKViewDelegate> {
}
@end

@implementation Delegate
// `NSWindowDelegate` overrides:
- (void) windowWillClose: (NSNotification *)notification
{
    [NSApp stop: self];
}

// `MTKViewDelegate` overrides:
- (void) mtkView: (MTKView *)view drawableSizeWillChange: (CGSize)size
{
    llair_example_resize(size);
}

- (void) drawInMTKView: (MTKView *)view
{
    llair_example_draw(view.currentRenderPassDescriptor, view.currentDrawable);
}
@end

int main(int argc, const char * argv[]) {
    @autoreleasepool {
        [NSApplication sharedApplication];

        [NSApp setActivationPolicy: NSApplicationActivationPolicyRegular];
        [NSApp activateIgnoringOtherApps: YES];

        // Create the window:
        auto window_style = NSWindowStyleMaskTitled    |
                            NSWindowStyleMaskClosable  |
                            NSWindowStyleMaskResizable;

        auto window_rect = NSMakeRect(0, 0, width, height);

        auto window = [[NSWindow alloc] initWithContentRect: window_rect
                       styleMask: window_style
                       backing: NSBackingStoreBuffered
                       defer: NO];
        [window setAcceptsMouseMovedEvents: YES];
        [window center];

        // Initialize Metal:
        auto metal_device = MTLCreateSystemDefaultDevice();

        // Create the MTKView:
        auto view = [[MTKView alloc] initWithFrame: window_rect device: metal_device];
        view.framebufferOnly = NO;
        view.depthStencilPixelFormat = MTLPixelFormatDepth32Float;
        [window setContentView: view];

        // Create the Delegate
        auto delegate = [[Delegate alloc] init];
        [window setDelegate: delegate];
        [view setDelegate: delegate];

        [window makeKeyAndOrderFront: nil];

        llair_example_init(argc, argv, metal_device, view);
        llair_example_resize({ (CGFloat)width, (CGFloat)height });
        [NSApp run];
        llair_example_exit();
    }

    return 0;
}
