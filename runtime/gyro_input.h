// SDL sensor ownership stays on the host UI thread; guest input reads published axes only.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

typedef struct SDL_Window SDL_Window;
typedef enum {
    GYRO_STATUS_DISCONNECTED,
    GYRO_STATUS_UNAVAILABLE,
    GYRO_STATUS_OFF,
    GYRO_STATUS_CALIBRATING,
    GYRO_STATUS_READY,
    GYRO_STATUS_ERROR
} GyroInputStatus;

GyroInputStatus input_gyro_status(void);
void input_gyro_calibrate(void);  // thread-safe request; main-thread update performs it
// Main thread, after each bounded event batch. NULL makes gameplay ineligible
// (start screen/preparation); enabled sensors can still learn their resting bias.
void input_gyro_update(SDL_Window *window);

#ifdef __cplusplus
}
#endif
