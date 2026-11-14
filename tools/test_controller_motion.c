// Authored inertial sensor fixtures; no controller, game code or platform I/O.
#include "../runtime/hle/controller_motion.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define BASE 1000000000ull
#define MS(x) ((uint64_t)(x)*1000000ull)
#define G 9.80665f
#define PI 3.14159265358979323846

static void near(double a, double b, double tolerance) { assert(fabs(a-b) <= tolerance); }
static void accel(ControllerMotion *m, uint64_t ns, float x, float y, float z) {
    controller_motion_acceleration(m,ns,(float[3]){x,y,z});
}
static void gyro(ControllerMotion *m, uint64_t ns, float x, float y, float z) {
    controller_motion_gyroscope(m,ns,(float[3]){x,y,z});
}
static InputMotionSnapshot read_motion(ControllerMotion *m, uint64_t ns) {
    InputMotionSnapshot s;
    assert(controller_motion_read(m,ns,&s));
    return s;
}
static void orthonormal(const float r[9]) {
    for (int row=0;row<3;row++) for(int other=0;other<3;other++) {
        double dot=0;
        for(int col=0;col<3;col++) dot+=r[row*3+col]*r[other*3+col];
        near(dot,row==other?1:0,2e-6);
    }
    double determinant=r[0]*(r[4]*r[8]-r[5]*r[7])-r[1]*(r[3]*r[8]-r[5]*r[6])+r[2]*(r[3]*r[7]-r[4]*r[6]);
    near(determinant,1,2e-6);
}
static void gravity_up(const InputMotionSnapshot *s) {
    double world[3]={0};
    for(int row=0;row<3;row++) for(int col=0;col<3;col++) world[row]+=s->direction[row*3+col]*s->acceleration[col];
    near(world[0],0,2e-5); near(world[1],G,2e-5); near(world[2],0,2e-5);
    orthonormal(s->direction);
}
static void tilt(void) {
    const float poses[][3]={{0,G,0},{0,-G,0},{G,0,0},{0,0,G},{0,0,-G},{G*.6f,G*.8f,0}};
    for(unsigned i=0;i<sizeof poses/sizeof *poses;i++) {
        ControllerMotion m;controller_motion_reset(&m);
        controller_motion_acceleration(&m,BASE,poses[i]);
        InputMotionSnapshot s=read_motion(&m,BASE);
        assert(s.acceleration_valid&&!s.gyro_valid&&!s.at_rest);
        gravity_up(&s);
        if(i==0)near(s.direction[4],1,1e-6);
        if(i==1)near(s.direction[4],-1,1e-6);
    }
}
static void spin(void) {
    const unsigned rates[]={100,250};
    for(unsigned r=0;r<2;r++) {
        ControllerMotion m;controller_motion_reset(&m);
        gyro(&m,BASE,0,0,(float)PI);
        for(unsigned i=1;i<=rates[r];i++) gyro(&m,BASE+1000000000ull*i/rates[r],0,0,(float)PI);
        InputMotionSnapshot s=read_motion(&m,BASE+1000000000ull);
        assert(s.gyro_valid&&!s.acceleration_valid);
        near(s.angle[2],PI,1e-6);near(s.direction[0],-1,1e-6);near(s.direction[4],-1,1e-6);
        near(s.direction[8],1,1e-6);orthonormal(s.direction);
        assert(s.sampling_number==rates[r]+1);
    }
}
static void paired(void) {
    const unsigned rates[]={100,250};
    for(unsigned r=0;r<2;r++) for(int order=0;order<2;order++) {
        ControllerMotion m;controller_motion_reset(&m);
        accel(&m,BASE,0,G,0);gyro(&m,BASE,0,0,(float)PI);
        for(unsigned i=1;i<=rates[r];i++) {
            uint64_t ns=BASE+1000000000ull*i/rates[r];double angle=PI*i/rates[r];
            if(order)gyro(&m,ns,0,0,(float)PI);
            accel(&m,ns,G*sin(angle),G*cos(angle),0);
            if(!order)gyro(&m,ns,0,0,(float)PI);
        }
        InputMotionSnapshot s=read_motion(&m,BASE+1000000000ull);
        gravity_up(&s);near(s.direction[0],-1,2e-6);near(s.direction[4],-1,2e-6);
        assert(s.sampling_number==rates[r]+1);
        assert(s.delta_time_ns==1000000000ull/rates[r]);
    }
}
static void polling(void) {
    ControllerMotion m;controller_motion_reset(&m);
    accel(&m,BASE,0,G,0);gyro(&m,BASE,0,0,0);
    InputMotionSnapshot first=read_motion(&m,BASE);
    assert(first.at_rest);
    for(int i=0;i<100;i++) {
        InputMotionSnapshot s=read_motion(&m,BASE+MS(1));
        assert(!memcmp(&first,&s,sizeof s));
    }
    ControllerMotion before=m;
    InputMotionSnapshot s;
    assert(!controller_motion_read(&m,BASE+MS(150),&s));
    assert(!memcmp(&before,&m,sizeof m));
    assert(!s.acceleration_valid&&!s.gyro_valid);
    near(s.direction[0],1,0);near(s.direction[4],1,0);near(s.direction[8],1,0);
}
static void bad_data(void) {
    ControllerMotion m;controller_motion_reset(&m);InputMotionSnapshot s;
    assert(!controller_motion_read(&m,BASE,&s));
    accel(&m,BASE,0,G,0);gyro(&m,BASE,0,0,0);
    accel(&m,BASE-1,NAN,0,0);gyro(&m,BASE-1,INFINITY,0,0);
    s=read_motion(&m,BASE);assert(s.acceleration_valid&&s.gyro_valid&&s.at_rest);
    accel(&m,BASE+MS(1),NAN,0,0);
    s=read_motion(&m,BASE+MS(1));assert(!s.acceleration_valid&&s.gyro_valid);
    accel(&m,BASE+MS(1)-1,0,G,0);
    s=read_motion(&m,BASE+MS(1));assert(!s.acceleration_valid&&s.gyro_valid);
    gyro(&m,BASE+MS(2),INFINITY,0,0);assert(!controller_motion_read(&m,BASE+MS(2),&s));
    gyro(&m,BASE+MS(2)-1,0,0,0);assert(!controller_motion_read(&m,BASE+MS(2),&s));
    gyro(&m,BASE+MS(3),101,0,0);assert(!controller_motion_read(&m,BASE+MS(3),&s));
    accel(&m,BASE+MS(4),0,G,0);gyro(&m,BASE+MS(4),0,0,0);
    uint64_t sampling=m.sampling_number;
    accel(&m,BASE+MS(3),0,-G,0);gyro(&m,BASE+MS(3),99,0,0);
    accel(&m,BASE+MS(4),0,-G,0);gyro(&m,BASE+MS(4),99,0,0);
    s=read_motion(&m,BASE+MS(4));assert(m.sampling_number==sampling);near(s.direction[4],1,1e-6);
    assert(!controller_motion_read(&m,BASE+MS(3),&s));
    controller_motion_reset(&m);assert(!controller_motion_read(&m,BASE+MS(5),&s));
    assert(!controller_motion_read(NULL,BASE,&s));assert(!controller_motion_read(&m,BASE,NULL));
}
static void gaps_and_linear(void) {
    ControllerMotion m;controller_motion_reset(&m);
    accel(&m,BASE,0,G,0);gyro(&m,BASE,0,0,1);
    gyro(&m,BASE+MS(200),0,0,1);
    InputMotionSnapshot s=read_motion(&m,BASE+MS(200));near(s.angle[2],0,0);near(s.direction[0],1,0);
    accel(&m,BASE+MS(201),15,G,0);s=read_motion(&m,BASE+MS(201));
    near(s.acceleration[0],15,0);near(s.direction[0],1,0);assert(!s.at_rest);
    // Fresh acceleration alone can reseed a flipped device after a gap.
    accel(&m,BASE+MS(400),0,-G,0);s=read_motion(&m,BASE+MS(400));
    assert(s.acceleration_valid&&!s.gyro_valid);near(s.direction[4],-1,1e-6);gravity_up(&s);
}
int main(int argc,char **argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"tilt"))tilt();
    else if(!strcmp(argv[1],"spin"))spin();
    else if(!strcmp(argv[1],"paired"))paired();
    else if(!strcmp(argv[1],"polling"))polling();
    else if(!strcmp(argv[1],"bad-data"))bad_data();
    else if(!strcmp(argv[1],"gaps"))gaps_and_linear();
    else return 2;
    puts("PASS native motion math");return 0;
}
