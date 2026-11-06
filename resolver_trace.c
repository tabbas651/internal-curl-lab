#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

typedef int (*resolver_function)(const char *, const char *,
                                const struct addrinfo *, struct addrinfo **);

static resolver_function real_getaddrinfo;
static pthread_once_t resolver_once = PTHREAD_ONCE_INIT;

static void initialize_resolver(void)
{
    real_getaddrinfo = (resolver_function)dlsym(RTLD_NEXT, "getaddrinfo");
}

static double monotonic_ms(void)
{
    struct timespec timestamp;
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
        return -1.0;
    }
    return timestamp.tv_sec * 1000.0 + timestamp.tv_nsec / 1000000.0;
}

int getaddrinfo(const char *hostname, const char *service,
                const struct addrinfo *hints, struct addrinfo **result)
{
    int incoming_errno = errno;
    pthread_once(&resolver_once, initialize_resolver);
    if (!real_getaddrinfo) {
        fprintf(stderr, "resolver_event=setup_error message=getaddrinfo_symbol_missing\n");
        errno = incoming_errno;
        return EAI_SYSTEM;
    }

    long thread_id = syscall(SYS_gettid);
    double begin_ms = monotonic_ms();
    flockfile(stderr);
    fprintf(stderr,
            "resolver_event=begin tid=%ld monotonic_ms=%.3f hostname=\"%s\" "
            "service=\"%s\" family=%d\n",
            thread_id, begin_ms, hostname ? hostname : "(null)",
            service ? service : "(null)", hints ? hints->ai_family : AF_UNSPEC);
    fflush(stderr);
    funlockfile(stderr);

    errno = incoming_errno;
    int status = real_getaddrinfo(hostname, service, hints, result);
    int resolver_errno = errno;
    double end_ms = monotonic_ms();
    flockfile(stderr);
    fprintf(stderr,
            "resolver_event=end tid=%ld monotonic_ms=%.3f duration_ms=%.3f "
            "status=%d errno=%d\n",
            thread_id, end_ms,
            begin_ms >= 0.0 && end_ms >= 0.0 ? end_ms - begin_ms : -1.0,
            status, resolver_errno);
    fflush(stderr);
    funlockfile(stderr);
    errno = resolver_errno;
    return status;
}