// Test-only offscreen bootstrap. Production code is included so no test API is added to the app.
#include "../runtime/nvn/metal.m"
#include <assert.h>

static void check_texture_dump(void) {
    FILE *out = tmpfile();
    assert(out);
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fileno(out), STDERR_FILENO) >= 0);
    dump_largest_textures("test");
    fflush(stderr);
    assert(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    rewind(out);
    char line[512];
    unsigned n = 0;
    bool seen[17] = {0};
    uint64_t previous = UINT64_MAX;
    while (fgets(line, sizeof line, out)) {
        double mib;
        if (sscanf(line, "[texdump] %lf MB", &mib) != 1) continue;
        char *label = strstr(line, "test-dump-");
        unsigned i;
        assert(label && sscanf(label, "test-dump-%u", &i) == 1 && i < (unsigned)g_reg_n && !seen[i]);
        seen[i] = true;
        TexBackend *b = (TexBackend *)g_reg[i]->backend;
        uint64_t size = b->tex.allocatedSize;
        assert(size <= previous && fabs(mib - size / 1048576.0) <= 0.051);
        previous = size;
        n++;
    }
    fclose(out);
    assert(n == (unsigned)MIN(g_reg_n, 16));
    if (g_reg_n == 17) assert(seen[16] && !seen[15]);
}

void metal_test_texture_dump(void) {
    assert(g_reg_n == 0);
    NvnTexture textures[17] = {0};
    TexBackend backends[17] = {0};
    id<MTLTexture> metal_textures[17] = {nil};
    check_texture_dump();
    for (unsigned i = 0; i < 17; i++) {
        unsigned width = i < 15 ? 128 + i * 64 : i == 15 ? 64 : 2048;
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:width height:width mipmapped:NO];
        metal_textures[i] = [dev newTextureWithDescriptor:d];
        backends[i].tex = metal_textures[i];
        assert(backends[i].tex);
        textures[i].width = textures[i].height = width;
        textures[i].backend = &backends[i];
        snprintf(textures[i].label, sizeof textures[i].label, "test-dump-%u", i);
        reg_add(&textures[i], &backends[i]);
        if (i >= 14) check_texture_dump();
    }
    for (unsigned i = 0; i < 17; i++) reg_remove(&backends[i]);
    assert(g_reg_n == 0);
}

void metal_test_bootstrap(void) {
    // runtime/main.o's constructor has already reserved the guest window, even with our alternate entry point.
    uint64_t address = HEAP_BASE;  // the alias reservation can move on newer macOS releases
    dev = MTLCreateSystemDefaultDevice();
    if (!dev) return;
    queue = [dev newCommandQueue];
    MTLSamplerDescriptor *sampler = [MTLSamplerDescriptor new];
    sampler.minFilter = sampler.magFilter = MTLSamplerMinMagFilterLinear;
    present_sampler = [dev newSamplerStateWithDescriptor:sampler];
    gwin_done = true;
    // Indirect commands use the same guest-address checks as the game, so keep this tiny mapping in that range.
    g_guest_buffer = [dev newBufferWithBytesNoCopy:(void *)(uintptr_t)address length:16384
                                         options:MTLResourceStorageModeShared deallocator:nil];
    if (!g_guest_buffer) { fprintf(stderr, "test guest Metal buffer failed\n"); abort(); }
    g_guest_buffer_base = g_gwin_base = address;
    g_gwin_max = g_guest_buffer.length;
    renderer_init(dev);
}

// Advance only the upload-cache frame for ownership regressions; no window or fake renderer is needed.
void metal_test_next_texture_frame(void) { g_frame_counter++; }

static void check_smaa_metrics(id<MTLTexture> input) {
    // A size-specialized reference must produce the same pixels as runtime metrics.
    NSString *metrics = [NSString stringWithFormat:@"#define SMAA_RT_METRICS float4(%.9g, %.9g, %.1f, %.1f)",
                         1.0 / input.width, 1.0 / input.height, (double)input.width, (double)input.height];
    NSString *reference = [smaa_msl() stringByReplacingOccurrencesOfString:@"#define SMAA_RT_METRICS metrics" withString:metrics];
    NSError *error = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:reference options:nil error:&error];
    assert(lib && !error);
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"smaa_vs"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> reference_pipelines[3];
    const char *names[] = {"smaa_edge", "smaa_weight", "smaa_blend"};
    for (unsigned i = 0; i < 3; i++) {
        pd.fragmentFunction = [lib newFunctionWithName:@(names[i])];
        reference_pipelines[i] = [dev newRenderPipelineStateWithDescriptor:pd error:&error];
        assert(reference_pipelines[i] && !error);
    }
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    PresentEffects *effects = [PresentEffects new];
    id<MTLTexture> actual = smaa_encode(cb, input, effects);
    id<MTLTexture> edges = smaa_target(nil, input.width, input.height, MTLPixelFormatRGBA8Unorm);
    id<MTLTexture> weights = smaa_target(nil, input.width, input.height, MTLPixelFormatRGBA8Unorm);
    id<MTLTexture> expected = smaa_target(nil, input.width, input.height, MTLPixelFormatRGBA8Unorm);
    // smaa_encode has uploaded the immutable area/search tables; bind the same tables again.
    extern const unsigned char *smaa_area_tex(void), *smaa_search_tex(void);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG8Unorm width:160 height:560 mipmapped:NO];
    id<MTLTexture> area = [dev newTextureWithDescriptor:d];
    [area replaceRegion:MTLRegionMake2D(0, 0, 160, 560) mipmapLevel:0 withBytes:smaa_area_tex() bytesPerRow:320];
    d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:64 height:16 mipmapped:NO];
    id<MTLTexture> search = [dev newTextureWithDescriptor:d];
    [search replaceRegion:MTLRegionMake2D(0, 0, 64, 16) mipmapLevel:0 withBytes:smaa_search_tex() bytesPerRow:64];
    smaa_pass(cb, reference_pipelines[0], edges, true, input, nil, nil);
    smaa_pass(cb, reference_pipelines[1], weights, true, edges, area, search);
    smaa_pass(cb, reference_pipelines[2], expected, false, input, weights, nil);
    NSUInteger row = (input.width * 4 + 255) & ~255ul, length = row * input.height;
    id<MTLBuffer> a = [dev newBufferWithLength:length options:MTLResourceStorageModeShared];
    id<MTLBuffer> b = [dev newBufferWithLength:length options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    for (unsigned i = 0; i < 2; i++)
        [blit copyFromTexture:i ? expected : actual sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                  sourceSize:MTLSizeMake(input.width, input.height, 1) toBuffer:i ? b : a destinationOffset:0
                  destinationBytesPerRow:row destinationBytesPerImage:length];
    [blit endEncoding]; [cb commit]; [cb waitUntilCompleted]; assert(!cb.error);
    for (NSUInteger y = 0; y < input.height; y++) for (NSUInteger x = 0; x < input.width * 4; x++)
        assert(abs(((uint8_t *)a.contents)[y * row + x] - ((uint8_t *)b.contents)[y * row + x]) <= 1);
}

void metal_test_frame_generation_filters(void) {
    int old_aa=g_settings.antialias,old_upscale=g_settings.upscaler;
    g_settings.antialias=AA_SMAA;g_settings.upscaler=UPSCALE_METALFX;
    id<MTLTexture> inputs[2];
    for(unsigned i=0;i<2;i++) {
        unsigned w=i?128:256,h=i?96:192;
        MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
        d.usage=MTLTextureUsageShaderRead;inputs[i]=[dev newTextureWithDescriptor:d];
        uint8_t *pixels=calloc(w*h,4);
        for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) {
            uint8_t *p=pixels+4*(y*w+x);p[0]=p[1]=p[2]=(x+2*y)%w<w/2?32:224;p[3]=255;
        }
        [inputs[i] replaceRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0 withBytes:pixels bytesPerRow:w*4];free(pixels);
    }
    id<MTLRenderPipelineState> pipelines[2]={nil,nil};id<MTLFXSpatialScaler> scalers[2]={nil,nil};
    id<MTLTexture> outputs[2]={nil,nil};
    for(unsigned frame=0;frame<3;frame++) {
        id<MTLCommandBuffer> cb=[queue commandBuffer];
        for(unsigned i=0;i<2;i++) {
            id<MTLTexture> output=present_postprocess(cb,inputs[i],320,240,i!=0);
            PresentEffects *effects=present_effects(i!=0);
            assert(effects->pe && effects->fx && output.width==320 && output.height==240);
            if(frame)assert(effects->pe==pipelines[i] && effects->fx==scalers[i] && output==outputs[i]);
            pipelines[i]=effects->pe;scalers[i]=effects->fx;outputs[i]=output;
        }
        assert(outputs[0]!=outputs[1]);[cb commit];[cb waitUntilCompleted];assert(!cb.error);
    }
    for (unsigned size=0;size<3;size++) {
        id<MTLCommandBuffer> cb=[queue commandBuffer];
        id<MTLTexture> input=inputs[size==1];
        assert(present_postprocess(cb,input,320,240,false));
        assert(present_effects(false)->pe==pipelines[0]);
        [cb commit];[cb waitUntilCompleted];assert(!cb.error);
    }
    PresentEffects *effects=present_effects(false);
    id<MTLFXSpatialScaler> scaler=effects->fx;
    effects->fx_out=nil;  // the cached state left by a failed output allocation
    effects->fx_retry_after=CACurrentMediaTime()+60;
    assert(!metalfx_upscale(nil,inputs[0],320,240,effects));
    assert(effects->fx==scaler && !effects->fx_out);
    effects->fx_retry_after=0;
    assert(metalfx_upscale(nil,inputs[0],320,240,effects));
    assert(effects->fx==scaler);
    for (unsigned i=0;i<2;i++)check_smaa_metrics(inputs[i]);
    g_settings.antialias=old_aa;g_settings.upscaler=old_upscale;
}

// A native page must not need a Metal present or touch the overlay's ImGui context.
void metal_test_preparation_page(void) {
    setenv("LARECOMP_HIDDEN", "1", 1);
    ui_window_init();
    NSWindow *native = (__bridge NSWindow *)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
    assert(native);
    NSUInteger baseline = native.contentView.subviews.count;
    NSRect original_bounds = native.contentView.bounds;
    NSView *game_view = native.contentView.subviews.firstObject;
    NSRect original_game_frame = game_view.frame;
    assert(!ui_preparation_update());
    backend_shader_prepare_progress(0, 4);
    assert(ui_preparation_update());
    assert(native.contentView.subviews.count == baseline + 1);
    PreparationPage *page = (PreparationPage *)native.contentView.subviews.lastObject;
    NSProgressIndicator *bar = page->bar;
    NSTextField *count = page->count;
    assert(bar.doubleValue == 0);
    backend_shader_prepare_progress(2, 4);
    assert(ui_preparation_update());
    assert(bar.doubleValue == 0.5 && [count.stringValue containsString:@"2 / 4"]);
    [native.contentView.superview layoutSubtreeIfNeeded];
    assert(NSEqualRects(native.contentView.bounds, original_bounds));
    assert(NSEqualRects(game_view.frame, original_game_frame));
    assert(NSEqualRects(page.frame, native.contentView.bounds));
    assert(page->detail.frame.size.width <= 560 && bar.frame.size.width > 100);
    // Fullscreen/resize uses larger native bounds; the overlay must follow them
    // without making the SDL Metal view or content view adopt its text width.
    [native setContentSize:NSMakeSize(1800, 1000)];
    [native.contentView.superview layoutSubtreeIfNeeded];
    NSRect resized = native.contentView.bounds;
    NSRect resized_game = game_view.frame;
    assert(resized.size.width >= 1700);
    assert(NSEqualSizes(resized_game.size, resized.size));
    assert(ui_preparation_update());
    [native.contentView.superview layoutSubtreeIfNeeded];
    assert(NSEqualRects(page.frame, resized));
    assert(NSEqualRects(native.contentView.bounds, resized));
    assert(NSEqualRects(game_view.frame, resized_game));
    assert(fabs(NSMidX(page->bar.frame) - NSMidX(page.bounds)) < 1);
    backend_shader_prepare_progress(9, 4);
    assert(ui_preparation_update() && bar.doubleValue == 1);
    backend_shader_prepare_progress(0, 0);
    assert(!ui_preparation_update() && native.contentView.subviews.count == baseline);
    assert(NSEqualRects(native.contentView.bounds, resized));
    assert(NSEqualRects(game_view.frame, resized_game));
    // A second preparation (save switch/reboot) owns a fresh page.
    backend_shader_prepare_progress(0, 1);
    assert(ui_preparation_update());
    backend_shader_prepare_progress(0, 0);
    assert(!ui_preparation_update() && native.contentView.subviews.count == baseline);
    SDL_DestroyWindow(window); window = NULL;
}
