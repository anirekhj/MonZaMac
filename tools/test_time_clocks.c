// Host-backed SDK clock APIs, without a game, network or graphics device.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../runtime/services/time.c"
uint64_t rt_ticks(void) { return 19200000; }
KSharedMem *kernel_create_shared_memory(uint64_t n, void (*map)(KSharedMem *), uint32_t *h) {
    (void)n; (void)map; (void)h; abort();
}
Service *service_new(const char *name, ServiceHandler handler, void *state) {
    (void)name; (void)handler; (void)state; abort();
}
void ipc_return_object(IpcResp *response, Service *service) { (void)response; (void)service; abort(); }
int main(void) {
    int64_t user = 0, network = 0;
    Cpu c = {0}; c.x[0] = (uintptr_t)&user; time_std_user_now(&c); assert(c.x[0] == 0);
    c.x[0] = (uintptr_t)&network; time_std_network_now(&c); assert(c.x[0] == 0);
    int64_t now = (int64_t)time(NULL);
    assert(user > 0 && user <= now && now - user <= 1);
    assert(network > 0 && network <= now && now - network <= 1);
    assert(llabs(network - user) <= 1);
    c.x[0] = 0; time_std_network_now(&c); assert(c.x[0] == 0);
    puts("PASS user and network SDK clocks share host time without IPC or networking");
}
