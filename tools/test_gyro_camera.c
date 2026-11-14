// Authored synthetic controller reports; no SDL, controller, window, or game files.
#include "../runtime/hle/gyro_camera.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr,"FAIL %s:%d: %s\n",__func__,__LINE__,#c); exit(1); } } while (0)
#define BASE 1000000000ull
#define MS(n) ((uint64_t)(n) * 1000000ull)
#define PI 3.14159265358979323846
static const float upright[3] = {0, 9.80665f, 0}, flat[3] = {0, 0, 9.80665f};
static const GyroCameraConfig normal = {1, false, false};
static double gain(double rate, double sensitivity) { return .2 + .8 * fmin(1, rate * sensitivity / PI); }
static void sample(GyroCamera *c, uint64_t t, float x, float y, float z, const float *accel, bool active) {
    const float gyro[3] = {x,y,z}; gyro_camera_sample(c,t,gyro,accel,active);
}
static void read_stick(GyroCamera *c, uint64_t t, bool active, const GyroCameraConfig *config, float out[2]) {
    gyro_camera_update(c,t,active,config,out,out+1);
    CHECK(isfinite(out[0]) && isfinite(out[1]));
    CHECK(hypot(out[0],out[1]) <= 1.000001);
}
static void zero(const float out[2]) { CHECK(out[0] == 0 && out[1] == 0); }
static void near(double a, double b) { CHECK(fabs(a-b) < .000002); }

static void axes(void) {
    GyroCamera c; float o[2];
    const float *gravity[] = {upright, flat, NULL};
    for (unsigned i=0;i<3;i++) {
        gyro_camera_reset(&c);
        sample(&c,BASE,0,i==1?0:1,i==1?1:0,gravity[i],true);
        read_stick(&c,BASE,true,&normal,o); near(o[0],-gain(1,1)); near(o[1],0);
        gyro_camera_reset(&c); sample(&c,BASE,1,0,0,gravity[i],true);
        read_stick(&c,BASE,true,&normal,o); near(o[0],0); near(o[1],gain(1,1));
    }
    gyro_camera_reset(&c); sample(&c,BASE,0,0,1,upright,true);
    read_stick(&c,BASE,true,&normal,o); zero(o); // controller roll is not yaw
    const float tilt[3]={6.9343486f,6.9343486f,0};
    gyro_camera_reset(&c); sample(&c,BASE,.70710678f,.70710678f,0,tilt,true);
    read_stick(&c,BASE,true,&normal,o); near(o[0],-gain(1,1)); near(o[1],0);
    gyro_camera_reset(&c); sample(&c,BASE,.70710678f,-.70710678f,0,tilt,true);
    read_stick(&c,BASE,true,&normal,o); near(o[0],0); near(o[1],gain(1,1));
    const float edge[3]={9.80665f,0,0};
    gyro_camera_reset(&c); sample(&c,BASE,1,0,0,edge,true);
    read_stick(&c,BASE,true,&normal,o); near(o[0],-gain(1,1)); near(o[1],0);
}
static void sensitivity(void) {
    const double sensitivities[]={.1,1,2,5,0,100,NAN,INFINITY};
    const double effective[]={.1,1,2,5,.1,5,1,1};
    for (unsigned i=0;i<sizeof sensitivities/sizeof *sensitivities;i++) {
        GyroCamera c; float o[2]; GyroCameraConfig config={sensitivities[i],false,false};
        gyro_camera_reset(&c); sample(&c,BASE,0,(float)(PI/2),0,upright,true);
        read_stick(&c,BASE,true,&config,o); near(o[0],-gain(PI/2,effective[i])); near(o[1],0);
    }
    GyroCamera c; float o[2]; GyroCameraConfig invert={1,true,true};
    gyro_camera_reset(&c); sample(&c,BASE,.5f,.5f,0,upright,true);
    read_stick(&c,BASE,true,&invert,o); CHECK(o[0]>0 && o[1]<0);
    gyro_camera_reset(&c); sample(&c,BASE,(float)PI,(float)PI,0,upright,true);
    read_stick(&c,BASE,true,&normal,o); near(hypot(o[0],o[1]),1); near(o[0],-o[1]);
    gyro_camera_reset(&c); sample(&c,BASE,0,100,0,NULL,true);
    read_stick(&c,BASE,true,&normal,o); near(o[0],-1);
}
static void deadzone(void) {
    GyroCamera c; float o[2];
    gyro_camera_reset(&c); read_stick(&c,BASE,true,&normal,o); zero(o);
    sample(&c,BASE,0,.017f,0,NULL,true); read_stick(&c,BASE,true,&normal,o); zero(o);
    gyro_camera_reset(&c); sample(&c,BASE,0,.019f,0,NULL,true);
    read_stick(&c,BASE,true,&normal,o); CHECK(o[0]<-.2f && o[0]>-.21f);
    gyro_camera_reset(&c); sample(&c,BASE,.013f,.013f,0,NULL,true);
    read_stick(&c,BASE,true,&normal,o); CHECK(hypot(o[0],o[1])>.2); // radial, not two independent floors
}
static void calibration(void) {
    GyroCamera c; float o[2];
    gyro_camera_reset(&c);
    for (unsigned i=0;i<50;i++) sample(&c,BASE+MS(i*10),.014f,.026f,-.008f,upright,false);
    CHECK(!gyro_camera_calibrated(&c));
    sample(&c,BASE+MS(500),.014f,.026f,-.008f,upright,false);
    CHECK(gyro_camera_calibrated(&c));
    read_stick(&c,BASE+MS(500),true,&normal,o); zero(o); // activation cannot replay the inactive sample
    sample(&c,BASE+MS(510),.014f,.026f,-.008f,upright,true);
    read_stick(&c,BASE+MS(510),true,&normal,o); zero(o);
    for(unsigned i=52;i<100;i++) sample(&c,BASE+MS(i*10),.014f,.526f,-.008f,upright,true);
    read_stick(&c,BASE+MS(990),true,&normal,o); near(o[0],-gain(.5,1));
    for(unsigned i=100;i<160;i++) sample(&c,BASE+MS(i*10),0,1,0,upright,false);
    CHECK(gyro_camera_calibrated(&c));
    sample(&c,BASE+MS(1600),.014f,.026f,-.008f,upright,true);
    read_stick(&c,BASE+MS(1600),true,&normal,o); zero(o); // release preserved bias
    gyro_camera_reset(&c); CHECK(!gyro_camera_calibrated(&c));
    sample(&c,BASE+MS(1610),.014f,.026f,-.008f,upright,true);
    read_stick(&c,BASE+MS(1610),true,&normal,o); CHECK(hypot(o[0],o[1])>.2);
}
static void calibration_guards(void) {
    GyroCamera c;
    gyro_camera_reset(&c);
    for(unsigned i=0;i<150;i++) sample(&c,BASE+MS(i*10),0,.5f,0,upright,false);
    CHECK(!gyro_camera_calibrated(&c));
    gyro_camera_reset(&c);
    for(unsigned i=0;i<150;i++) sample(&c,BASE+MS(i*10),0,.025f,0,upright,true);
    CHECK(!gyro_camera_calibrated(&c));
    gyro_camera_reset(&c);
    for(unsigned i=0;i<150;i++) sample(&c,BASE+MS(i*10),0,.025f+(i%2?.01f:-.01f),0,upright,false);
    CHECK(!gyro_camera_calibrated(&c));
    gyro_camera_reset(&c);
    for(unsigned i=0;i<20;i++) sample(&c,BASE+MS(i*10),0,.025f,0,upright,false);
    for(unsigned i=0;i<20;i++) sample(&c,BASE+MS(1000+i*10),0,.025f,0,upright,false);
    CHECK(!gyro_camera_calibrated(&c)); // an outage does not count as quiet time
    const float moving[3]={0,16,0}; gyro_camera_reset(&c);
    for(unsigned i=0;i<100;i++) sample(&c,BASE+MS(i*10),0,.025f,0,moving,false);
    CHECK(!gyro_camera_calibrated(&c));
    for(unsigned hz=100;hz<=250;hz+=150) {
        gyro_camera_reset(&c);
        for(unsigned i=0;i<=hz;i++) sample(&c,BASE+(uint64_t)i*1000000000ull/hz,0,.025f,0,NULL,false);
        CHECK(gyro_camera_calibrated(&c));
        float o[2]; sample(&c,BASE+MS(1010),0,.025f,0,NULL,true);
        read_stick(&c,BASE+MS(1010),true,&normal,o); zero(o);
    }
}
static void timing(void) {
    GyroCamera c; float o[2];
    gyro_camera_reset(&c); sample(&c,BASE,0,1,0,NULL,true);
    read_stick(&c,BASE+MS(149),true,&normal,o); CHECK(o[0]<-.2);
    read_stick(&c,BASE+MS(150),true,&normal,o); zero(o);
    sample(&c,BASE+MS(151),0,.2f,0,NULL,true);
    read_stick(&c,BASE+MS(151),true,&normal,o); near(o[0],-gain(.2,1));
    sample(&c,BASE+MS(151),0,1,0,NULL,true); // duplicate report is not extra motion
    read_stick(&c,BASE+MS(152),true,&normal,o); zero(o);
    sample(&c,BASE+MS(150),0,1,0,NULL,true);
    read_stick(&c,BASE+MS(153),true,&normal,o); zero(o);
    sample(&c,BASE+MS(154),0,.2f,0,NULL,true);
    read_stick(&c,BASE+MS(154),true,&normal,o); near(o[0],-gain(.2,1));
    read_stick(&c,BASE+MS(153),true,&normal,o); zero(o); // backwards read clock
    sample(&c,BASE+MS(155),0,.2f,0,NULL,true);
    read_stick(&c,BASE+MS(155),true,&normal,o); near(o[0],-gain(.2,1));
    sample(&c,BASE+MS(10000),0,1,0,NULL,true);
    read_stick(&c,BASE+MS(156),true,&normal,o); zero(o); // future report; recover next valid clock
    sample(&c,BASE+MS(157),0,.2f,0,NULL,true);
    read_stick(&c,BASE+MS(157),true,&normal,o); near(o[0],-gain(.2,1));
}
static void release(void) {
    GyroCamera c; float o[2];
    gyro_camera_reset(&c); sample(&c,BASE,0,1,0,upright,true);
    read_stick(&c,BASE,true,&normal,o); CHECK(o[0]<0);
    read_stick(&c,BASE+MS(1),false,&normal,o); zero(o);
    for(unsigned i=1;i<=100;i++) sample(&c,BASE+MS(i*10),0,10,0,upright,false);
    read_stick(&c,BASE+MS(1000),true,&normal,o); zero(o);
    sample(&c,BASE+MS(1010),0,0,0,upright,true);
    read_stick(&c,BASE+MS(1010),true,&normal,o); zero(o);
    gyro_camera_reset(&c); read_stick(&c,BASE+MS(1020),true,&normal,o); zero(o);
    sample(&c,BASE+MS(1030),0,1,0,NULL,true);
    read_stick(&c,BASE+MS(1030),true,&normal,o); near(o[0],-gain(1,1));
    gyro_camera_reset(&c); sample(&c,BASE,0,1,0,NULL,true);
    read_stick(&c,BASE+MS(10),false,&normal,o); zero(o);
    read_stick(&c,BASE+MS(20),true,&normal,o); zero(o);
    sample(&c,BASE+MS(15),0,1,0,NULL,true); // late event from before reactivation
    read_stick(&c,BASE+MS(21),true,&normal,o); zero(o);
    sample(&c,BASE+MS(22),0,0,0,NULL,true);
    read_stick(&c,BASE+MS(22),true,&normal,o); zero(o);
}
static void rates(void) {
    const unsigned sensor_rates[]={100,250}, poll_rates[]={30,60,120};
    for(unsigned a=0;a<2;a++) for(unsigned b=0;b<3;b++) {
        unsigned hz=sensor_rates[a],poll=poll_rates[b],next=0;
        GyroCamera c; float o[2]; gyro_camera_reset(&c);
        for(unsigned tick=0;tick<=poll*2;tick++) {
            uint64_t now=BASE+(uint64_t)tick*1000000000ull/poll;
            while(BASE+(uint64_t)next*1000000000ull/hz<=now) {
                sample(&c,BASE+(uint64_t)next*1000000000ull/hz,0,1,0,upright,true);next++;
            }
            read_stick(&c,now,true,&normal,o); near(o[0],-gain(1,1)); near(o[1],0);
        }
    }
    double at20[2];
    for(unsigned a=0;a<2;a++) {
        GyroCamera c; float o[2]; gyro_camera_reset(&c);
        sample(&c,BASE,0,0,0,NULL,true);
        unsigned dt=1000/sensor_rates[a];
        for(unsigned ms=dt;ms<=20;ms+=dt) {
            sample(&c,BASE+MS(ms),0,1,0,NULL,true);
            // Extra reads do not advance a filter or accumulate motion.
            for(unsigned n=0;n<5;n++) read_stick(&c,BASE+MS(ms),true,&normal,o);
        }
        at20[a]=-o[0]; CHECK(at20[a]>.34 && at20[a]<.38);
    }
    near(at20[0],at20[1]); // same 20 ms response at both sensor rates
}
static void bad_data(void) {
    const float bad[]={NAN,INFINITY,-INFINITY,1000};
    for(unsigned i=0;i<4;i++) for(unsigned axis=0;axis<3;axis++) {
        GyroCamera c; float o[2],v[3]={0,1,0}; gyro_camera_reset(&c);
        sample(&c,BASE,0,1,0,upright,true);v[axis]=bad[i];
        gyro_camera_sample(&c,BASE+MS(10),v,upright,true);
        read_stick(&c,BASE+MS(10),true,&normal,o);zero(o);
    }
    GyroCamera c; float o[2]; const float broken_accel[3]={NAN,0,0};
    gyro_camera_reset(&c); sample(&c,BASE,0,1,0,broken_accel,true);
    read_stick(&c,BASE,true,&normal,o); near(o[0],-gain(1,1));
    gyro_camera_sample(&c,BASE+MS(10),NULL,NULL,true);
    read_stick(&c,BASE+MS(10),true,&normal,o);zero(o);
    gyro_camera_reset(&c); sample(&c,0,0,1,0,NULL,true);
    read_stick(&c,BASE,true,&normal,o);zero(o);
    // Gravity loss falls back to local Y; stale acceleration is not immortal.
    sample(&c,BASE,0,1,0,flat,true);
    for(unsigned i=1;i<=16;i++) sample(&c,BASE+MS(i*10),0,1,0,NULL,true);
    read_stick(&c,BASE+MS(160),true,&normal,o);near(o[0],-gain(1,1));
}
int main(int argc,char **argv) {
    const struct {const char *name;void (*run)(void);} tests[]={
        {"axes",axes},{"sensitivity",sensitivity},{"deadzone",deadzone},{"calibration",calibration},
        {"calibration-guards",calibration_guards},{"timing",timing},{"release",release},{"rates",rates},{"bad-data",bad_data}};
    unsigned ran=0;
    for(unsigned i=0;i<sizeof tests/sizeof *tests;i++) if(argc==1 || !strcmp(argv[1],tests[i].name)) {tests[i].run();ran++;}
    CHECK(ran>0);printf("PASS %u gyro scenarios (%u checks)\n",ran,checks);return 0;
}
