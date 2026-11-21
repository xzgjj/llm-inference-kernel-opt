// GCC 11 的 libtsan 未拦截 pthread_cond_clockwait：libstdc++ 的
// condition_variable::wait_for / wait_until(steady_clock) 走这个函数，
// 于是 TSan 看不到定时等待期间的解锁/重锁，把"同一把锁保护的队列"误报成
// data race，并连带把经该队列传递的对象误报成竞争。
//
// 这里把 CLOCK_MONOTONIC 的绝对期限折算成 CLOCK_REALTIME 期限后转调
// pthread_cond_timedwait，剩余时间语义不变，只让 TSan 能观测到 cond wait
// 的解锁/重锁。仅用于 GCC 11 环境的本地验证：
//   gcc -shared -fPIC -O1 scripts/ci/tsan_clockwait_shim.c -o /tmp/clockwait_shim.so -lpthread
//   LD_PRELOAD=/tmp/clockwait_shim.so TSAN_OPTIONS=halt_on_error=1 ctest -L fast
// CI（ubuntu-latest，GCC 13+）的 libtsan 已拦截该函数，无需 shim。
#define _GNU_SOURCE
#include <pthread.h>
#include <time.h>

int pthread_cond_clockwait(pthread_cond_t *cond, pthread_mutex_t *mutex, clockid_t clock_id,
                           const struct timespec *abstime) {
    if (clock_id == CLOCK_REALTIME) {
        return pthread_cond_timedwait(cond, mutex, abstime);
    }
    struct timespec now_mono;
    struct timespec now_real;
    clock_gettime(CLOCK_MONOTONIC, &now_mono);
    clock_gettime(CLOCK_REALTIME, &now_real);
    struct timespec deadline = now_real;
    deadline.tv_sec += abstime->tv_sec - now_mono.tv_sec;
    deadline.tv_nsec += abstime->tv_nsec - now_mono.tv_nsec;
    if (deadline.tv_nsec < 0) {
        deadline.tv_nsec += 1000000000L;
        deadline.tv_sec -= 1;
    }
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec += 1;
    }
    return pthread_cond_timedwait(cond, mutex, &deadline);
}
