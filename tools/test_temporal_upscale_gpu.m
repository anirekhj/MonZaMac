#include <stdbool.h>
bool settings_temporal_upscale(void);
#include "../runtime/nvn/temporal_upscale.m"
#import <Foundation/Foundation.h>
#import <simd/simd.h>
#include <assert.h>
#include <math.h>
#include <stdarg.h>
static bool selected=true,menu_open;
Settings g_settings;
bool menu_is_open(void){return menu_open;}
bool settings_temporal_upscale(void){return selected;}
void devlog(const char *subject,const char *fmt,...) {(void)subject;(void)fmt;}
static id<MTLTexture> input(id<MTLDevice> dev,MTLPixelFormat f) {
 MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:1920 height:1080 mipmapped:NO];
 d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;return [dev newTextureWithDescriptor:d];
}
static id<MTLTexture> sized_input(id<MTLDevice> dev,MTLPixelFormat f,unsigned w,unsigned h) {
 MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
 d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;return [dev newTextureWithDescriptor:d];
}
static void clear_input(id<MTLCommandBuffer> cb,id<MTLTexture> t,double red) {
 MTLRenderPassDescriptor *p=[MTLRenderPassDescriptor renderPassDescriptor];
 if(t.pixelFormat==MTLPixelFormatDepth32Float){p.depthAttachment.texture=t;p.depthAttachment.clearDepth=.5;p.depthAttachment.loadAction=MTLLoadActionClear;p.depthAttachment.storeAction=MTLStoreActionStore;}
 else{p.colorAttachments[0].texture=t;p.colorAttachments[0].clearColor=MTLClearColorMake(red,.2,.1,1);p.colorAttachments[0].loadAction=MTLLoadActionClear;p.colorAttachments[0].storeAction=MTLStoreActionStore;}
 [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
}
static NSData *pixel(id<MTLCommandQueue> q,id<MTLTexture> t,unsigned x,unsigned y) {
 id<MTLBuffer> data=[q.device newBufferWithLength:256 options:MTLResourceStorageModeShared];
 id<MTLCommandBuffer> cb=[q commandBuffer];id<MTLBlitCommandEncoder> e=[cb blitCommandEncoder];
 [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x,y,0) sourceSize:MTLSizeMake(1,1,1)
           toBuffer:data destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256];
 [e endEncoding];[cb commit];[cb waitUntilCompleted];assert(!cb.error);return [NSData dataWithBytes:data.contents length:8];
}
static id<MTLTexture> pan_input(id<MTLDevice> dev,unsigned w,unsigned h,unsigned frame) {
 MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
 d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead;id<MTLTexture> t=[dev newTextureWithDescriptor:d];
 uint8_t *bytes=malloc(w*h*4);
 for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++){
  float uv=(x+.5f)/w+frame/64.f;unsigned at=(y*w+x)*4;
  bytes[at]=(uint8_t)lroundf((.1f+.7f*uv)*255);bytes[at+1]=51;bytes[at+2]=((int)floorf(uv*16)+(y*16/h))&1?220:25;bytes[at+3]=255;
 }
 [t replaceRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0 withBytes:bytes bytesPerRow:w*4];free(bytes);return t;
}
static float linear(float x){return x<=.04045f?x/12.92f:powf((x+.055f)/1.055f,2.4f);}
static unsigned dynamic_test(id<MTLDevice> dev,id<MTLCommandQueue> q,FgCamera *cam,int fg_mode) {
 const unsigned sizes[][2]={{512,288},{448,252},{384,216},{512,288},{448,252},{512,288}};
 g_settings.res_mode=RES_DYNAMIC;unsetenv("LARECOMP_TEMPORAL_DYNAMIC_INPUT");ts_reset();fg_reset();ts_output_extent(768,432);
 TsState *generation=nil;TsPrograms *code=nil;
 for(unsigned i=0;i<sizeof sizes/sizeof sizes[0];i++){
  unsigned w=sizes[i][0],h=sizes[i][1];id<MTLTexture> s=pan_input(dev,w,h,i),z=sized_input(dev,MTLPixelFormatR32Float,w,h);
  float n=.1f,f=100,ys=1.f/tanf(M_PI/6),xs=ys/cam->aspect;
  matrix_float4x4 projection={.columns={{xs,0,0,0},{0,ys,0,0},{0,0,f/(n-f),-1},{0,0,n*f/(n-f),0}}};
  matrix_float4x4 view=matrix_identity_float4x4;view.columns[3].x=-(float)i/(16*xs);
  matrix_float4x4 vp=simd_mul(projection,view),inverse=simd_inverse(vp);FgCamera moving=*cam;
  memcpy(moving.view_projection,&vp,sizeof vp);memcpy(moving.inverse_view_projection,&inverse,sizeof inverse);
  id<MTLCommandBuffer> cb=[q commandBuffer];clear_input(cb,z,(-2*f/(n-f)+n*f/(n-f))/2);ts_geometry_extent(w,h,100+i);
  FgRect r={0,0,w,h};TemporalOutput out={0};assert(ts_encode(cb,s,s,z,nil,&moving,&r,768,432,100+i,&out));
  if(!i){generation=state;code=state->programs;assert(state->dynamic && state->iw==768 && state->ih==432);}
  assert(state==generation && state->programs==code);assert(out.prepared.reset==(i==0));
  assert(out.prepared.depth.width==768 && out.prepared.motion.height==432);
  assert(state->effect.inputContentWidth==w && state->effect.inputContentHeight==h);
  id<MTLTexture> generated=fg_mode?fg_encode_prepared(cb,out.scene,out.composited,&out.prepared,&moving,fg_mode,30.+i/(fg_mode==1?30.:60.)):nil;
  if(fg_mode){if(i<2)assert(!generated);else assert(generated && generated.width==768 && generated.height==432);}
  [cb commit];[cb waitUntilCompleted];if(cb.error)fprintf(stderr,"%s\n",cb.error.description.UTF8String);assert(!cb.error);
  NSData *data=pixel(q,out.scene,384,216);const __fp16 *rgb=data.bytes;
  assert(fabsf((float)rgb[0]-linear(.1f+.7f*(.5f+i/64.f)))<.025f && fabsf((float)rgb[1]-.0331f)<.02f);
  data=pixel(q,out.prepared.motion,w/2,h/2);const __fp16 *motion=data.bytes;
  assert(fabsf((float)motion[0]-(i?w/64.f:0))<.05f && fabsf((float)motion[1])<.05f);
  if(generated && i>=3){
   float errors[3]={0};
   for(unsigned x=192;x<=576;x+=96){data=pixel(q,generated,x,216);const __fp16 *value=data.bytes;
    for(unsigned k=0;k<3;k++)errors[k]+=fabsf((float)value[0]-linear(.1f+.7f*((x+.5f)/768+(i-1+k*.5f)/64.f)));
   }
   assert(errors[1]<=errors[0]+.006f && errors[1]<=errors[2]+.006f);
  }
 }
 // Force the fixed-size fallback without losing the device-level preparation pipelines.
 setenv("LARECOMP_TEMPORAL_DYNAMIC_INPUT","0",1);id<MTLCommandBuffer> cb=[q commandBuffer];
 id<MTLTexture> s=sized_input(dev,MTLPixelFormatRGBA8Unorm,512,288),z=sized_input(dev,MTLPixelFormatR32Float,512,288);clear_input(cb,s,.3);clear_input(cb,z,.5);
 FgRect r={0,0,512,288};TemporalOutput out={0};assert(ts_encode(cb,s,s,z,nil,cam,&r,768,432,106,&out));
 assert(state!=generation && !state->dynamic && state->iw==512 && out.prepared.reset && state->programs==code);
 [cb commit];[cb waitUntilCompleted];assert(!cb.error);unsetenv("LARECOMP_TEMPORAL_DYNAMIC_INPUT");g_settings.res_mode=RES_X15;
 return 7;
}
static unsigned reactive_test(id<MTLDevice> dev,id<MTLCommandQueue> q,FgCamera *cam) {
 ts_reset();fg_reset();g_settings.res_mode=RES_DYNAMIC;setenv("LARECOMP_TEMPORAL_REACTIVE","1",1);ts_output_extent(512,288);
 TsPrograms *code=nil;TsFrame *previous=nil;
 for(unsigned i=0;i<3;i++){
  id<MTLTexture> s=sized_input(dev,MTLPixelFormatRGBA8Unorm,256,144),z=sized_input(dev,MTLPixelFormatR32Float,256,144);
  id<MTLCommandBuffer> cb=[q commandBuffer];clear_input(cb,s,i==2?.9:.3);clear_input(cb,z,.5);ts_geometry_extent(256,144,300+i);
  FgRect r={0,0,256,144};assert(ts_encode(cb,s,s,z,nil,cam,&r,512,288,300+i,NULL));
  assert(state->reactive && state->previous_frame!=previous);previous=state->previous_frame;code=state->programs;
  [cb commit];[cb waitUntilCompleted];assert(!cb.error);NSData *data=pixel(q,previous->reactive,128,72);unsigned value=((const uint8_t *)data.bytes)[0];
  if(i<2)assert(value==0);else assert(value>250);
 }
 // Pending comparisons must retain both their previous and current slots across a reset.
 NSMutableArray *pending=[NSMutableArray new],*outputs=[NSMutableArray new];
 for(unsigned i=0;i<5;i++){
  id<MTLTexture> s=sized_input(dev,MTLPixelFormatRGBA8Unorm,256,144),z=sized_input(dev,MTLPixelFormatR32Float,256,144);
  id<MTLCommandBuffer> cb=[q commandBuffer];clear_input(cb,s,.3+i*.05);clear_input(cb,z,.5);ts_geometry_extent(256,144,303+i);
  FgRect r={0,0,256,144};id<MTLTexture> out=ts_encode(cb,s,s,z,nil,cam,&r,512,288,303+i,NULL);
  assert(out && ![outputs containsObject:out]);[pending addObject:cb];[outputs addObject:out];
 }
 ts_reset();for(id<MTLCommandBuffer> cb in pending){[cb commit];[cb waitUntilCompleted];assert(!cb.error);}
 unsetenv("LARECOMP_TEMPORAL_REACTIVE");ts_reset();assert(program_cache.count==1 && program_cache[0]==code);g_settings.res_mode=RES_X15;
 return 9;
}
static unsigned subrect_test(id<MTLDevice> dev,id<MTLCommandQueue> q,FgCamera *cam) {
 ts_reset();FgRect r={32,16,128,72};
 MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:256 height:144 mipmapped:NO];
 d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead;id<MTLTexture> z=[dev newTextureWithDescriptor:d];
 float *depth=malloc(256*144*sizeof(float));uint8_t *rgba=malloc(256*144*4);
 for(unsigned y=0;y<144;y++)for(unsigned x=0;x<256;x++){
  bool inside=x>=r.x && x<r.x+r.width && y>=r.y && y<r.y+r.height;unsigned at=y*256+x;
  depth[at]=inside?.75f:.125f;rgba[at*4]=inside?77:230;rgba[at*4+1]=51;rgba[at*4+2]=26;rgba[at*4+3]=255;
 }
 [z replaceRegion:MTLRegionMake2D(0,0,256,144) mipmapLevel:0 withBytes:depth bytesPerRow:256*sizeof(float)];free(depth);
 d.pixelFormat=MTLPixelFormatRGBA8Unorm;id<MTLTexture> ui=[dev newTextureWithDescriptor:d];
 [ui replaceRegion:MTLRegionMake2D(0,0,256,144) mipmapLevel:0 withBytes:rgba bytesPerRow:256*4];free(rgba);
 id<MTLTexture> s=sized_input(dev,MTLPixelFormatRGBA8Unorm,128,72);id<MTLCommandBuffer> cb=[q commandBuffer];clear_input(cb,s,.3);
 TemporalOutput out={0};assert(ts_encode(cb,s,ui,z,nil,cam,&r,256,144,600,&out));[cb commit];[cb waitUntilCompleted];assert(!cb.error);
 NSData *data=pixel(q,out.prepared.depth,0,0);assert(fabsf(*(const float *)data.bytes-.75f)<1e-6f);
 data=pixel(q,out.composited,8,8);assert(fabsf((float)((const __fp16 *)data.bytes)[0]-.07324f)<.02f);
 return 3;
}
int main(void){@autoreleasepool {
 unsetenv("LARECOMP_TEMPORAL_REACTIVE");unsetenv("LARECOMP_TEMPORAL_DYNAMIC_INPUT");g_settings.res_mode=RES_X15;
 unsigned checks=0;
 for(unsigned flip=0;flip<2;flip++)for(unsigned frame=0;frame<8;frame++){
  float j[2],m[96];ts_jitter(frame,j);matrix_float4x4 identity=matrix_identity_float4x4;
  matrix_float4x4 projection={.columns={{2,0,0,0},{0,3,0,0},{0,0,-1.001f,-1},{0,0,-.1001f,0}}};
  matrix_float4x4 inv=simd_inverse(projection);memcpy(m,&projection,64);memcpy(m+16,&identity,64);memcpy(m+32,&projection,64);
  memcpy(m+48,&inv,64);memcpy(m+64,&identity,64);memcpy(m+80,&inv,64);ts_camera_jitter(m,j,1920,1080,flip);
  matrix_float4x4 vp,vi,p,pi;memcpy(&vp,m,64);memcpy(&p,m+32,64);memcpy(&vi,m+48,64);memcpy(&pi,m+80,64);
  matrix_float4x4 a=simd_mul(vp,vi),b=simd_mul(p,pi);
  for(unsigned c=0;c<4;c++)for(unsigned r=0;r<4;r++){assert(fabsf(a.columns[c][r]-(c==r))<1e-5);assert(fabsf(b.columns[c][r]-(c==r))<1e-5);checks+=2;}
  vector_float4 point={.2f,.1f,-2,1},old=simd_mul(projection,point),shifted=simd_mul(vp,point);
  assert(fabsf((shifted.x/shifted.w-old.x/old.w)*960-j[0])<1e-4);
  assert(fabsf((shifted.y/shifted.w-old.y/old.w)*(flip?540:-540)-j[1])<1e-4);checks+=2;
 }
 setenv("LARECOMP_TEMPORAL_SPIKE","1",1);assert(ts_enabled());ts_output_extent(2880,1620);assert(!ts_geometry_extent(1920,1080,0));
 id<MTLDevice> dev=MTLCreateSystemDefaultDevice();id<MTLCommandQueue> q=[dev newCommandQueue];
 if(!dev || !temporal_supported()){puts("SKIP: supported MetalFX GPU required");return 0;}
 id<MTLTexture> scene=input(dev,MTLPixelFormatRGBA8Unorm),ui=input(dev,MTLPixelFormatRGBA8Unorm);
 FgCamera cam={.near_plane=.1f,.far_plane=100,.vertical_fov=60,.aspect=16.f/9};matrix_float4x4 identity=matrix_identity_float4x4;
 memcpy(cam.view_projection,&identity,64);memcpy(cam.inverse_view_projection,&identity,64);FgRect rect={0,0,1920,1080};
 id<MTLTexture> last_depth=nil;
 for(unsigned type=0;type<2;type++){
  id<MTLTexture> depth=input(dev,type?MTLPixelFormatDepth32Float:MTLPixelFormatR32Float);last_depth=depth;ts_reset();
  id<MTLCommandBuffer> cb=[q commandBuffer];
  for(id<MTLTexture> t in @[scene,ui,depth]){
   MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];
   if(t.pixelFormat==MTLPixelFormatDepth32Float){rp.depthAttachment.texture=t;rp.depthAttachment.clearDepth=.5;rp.depthAttachment.loadAction=MTLLoadActionClear;rp.depthAttachment.storeAction=MTLStoreActionStore;}
   else{rp.colorAttachments[0].texture=t;rp.colorAttachments[0].clearColor=MTLClearColorMake(.3,.2,.1,1);rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionStore;}
   [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
  }
  [cb commit];[cb waitUntilCompleted];assert(!cb.error);
  for(unsigned frame=0;frame<5;frame++){
   ts_geometry_extent(1920,1080,frame==3?7:frame);
   cb=[q commandBuffer];unsigned w=frame==4?2560:2880,h=frame==4?1440:1620;
   id<MTLTexture> out=ts_encode(cb,scene,ui,depth,nil,&cam,&rect,w,h,frame==3?7:frame,NULL);
   assert(out && out.width==w && out.height==h);[cb commit];[cb waitUntilCompleted];assert(!cb.error);checks++;
  }
 }
 assert(ts_geometry_extent(1920,1080,5));
 menu_open=true;assert(!ts_geometry_extent(1920,1080,5));assert(!ts_raster_active());menu_open=false;
 assert(!ts_geometry_extent(480,270,5));checks+=4;
 id<MTLCommandBuffer> bad=[q commandBuffer];FgRect badrect={UINT32_MAX,0,1920,1080};
 assert(!ts_encode(bad,scene,ui,last_depth,nil,&cam,&badrect,2880,1620,9,NULL));assert(!state);checks+=2;
 // Differing physical pixel aspect is valid: camera projection still supplies the right rays.
 FgCamera stretched=cam;stretched.aspect=1.6f;ts_output_extent(2880,1800);
 id<MTLCommandBuffer> tall=[q commandBuffer];TemporalOutput tall_out={0};
 assert(ts_encode(tall,scene,ui,last_depth,nil,&stretched,&rect,2880,1800,10,&tall_out));
 assert(tall_out.prepared.reset && tall_out.prepared.depth.width==1920 && tall_out.scene.width==2880);
 [tall commit];[tall waitUntilCompleted];assert(!tall.error);checks+=2;
 // Six queued generations must retain distinct resources through a reset.
 ts_reset();ts_output_extent(2880,1620);
 NSMutableArray *pending=[NSMutableArray new],*outputs=[NSMutableArray new];
 for(unsigned i=0;i<6;i++){
  id<MTLCommandBuffer> cb=[q commandBuffer];ts_geometry_extent(1920,1080,20+i);
  id<MTLTexture> out=ts_encode(cb,scene,ui,last_depth,nil,&cam,&rect,2880,1620,20+i,NULL);
  assert(out && ![outputs containsObject:out]);[outputs addObject:out];[pending addObject:cb];checks++;
 }
 id<MTLCommandBuffer> extra=[q commandBuffer];assert(!ts_encode(extra,scene,ui,last_depth,nil,&cam,&rect,2880,1620,26,NULL));checks++;
 ts_reset();
 for(id<MTLCommandBuffer> cb in pending){[cb commit];[cb waitUntilCompleted];assert(!cb.error);checks++;}
 selected=false;assert(!ts_raster_active());selected=true;
 assert(!ts_geometry_extent(4096,2160,9));assert(!ts_raster_active());checks+=3;
 checks+=dynamic_test(dev,q,&cam,0);
 if(fg_supported()){checks+=dynamic_test(dev,q,&cam,1);checks+=dynamic_test(dev,q,&cam,2);}
 checks+=reactive_test(dev,q,&cam);
 checks+=subrect_test(dev,q,&cam);
 printf("PASS %u matrix, coordinate, depth, history, dynamic-input, reactive-mask and linked-FG checks\n",checks);return 0;
}}
