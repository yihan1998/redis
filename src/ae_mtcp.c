/* mTCP epoll based ae.c module */

#include <mtcp_epoll.h>

mctx_t mctx;

typedef struct aeApiState {
    int epfd;
    struct mtcp_epoll_event * events;
} aeApiState;

static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState));

    if (!state) return -1;
    state->events = zmalloc(sizeof(struct mtcp_epoll_event)*eventLoop->setsize);
    if (!state->events) {
        zfree(state);
        return -1;
    }
    state->epfd = mtcp_epoll_create(mctx, 1024); /* 1024 is just a hint for the kernel */
    if (state->epfd == -1) {
        zfree(state->events);
        zfree(state);
        return -1;
    }
    printf(" [%s:%d] create epoll socket %d\n", __func__, __LINE__, state->epfd);
    eventLoop->apidata = state;
    return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;

    state->events = zrealloc(state->events, sizeof(struct mtcp_epoll_event)*setsize);
    return 0;
}

static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    mtcp_close(mctx, state->epfd);
    zfree(state->events);
    zfree(state);
}

static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct mtcp_epoll_event ee;
    /* If the fd was already monitored for some event, we need a MOD
     * operation. Otherwise we need an ADD operation. */
    int op = eventLoop->events[fd].mask == AE_NONE ?
            MTCP_EPOLL_CTL_ADD : MTCP_EPOLL_CTL_MOD;

    ee.events = 0;
    mask |= eventLoop->events[fd].mask; /* Merge old events */
    if (mask & AE_READABLE) ee.events |= MTCP_EPOLLIN;
    if (mask & AE_WRITABLE) ee.events |= MTCP_EPOLLOUT;
    ee.data.u64 = 0; /* avoid valgrind warning */
    ee.data.sockid = fd;
    if (mtcp_epoll_ctl(mctx, state->epfd,op,fd,&ee) == -1) return -1;
    return 0;
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask) {
    aeApiState *state = eventLoop->apidata;
    struct mtcp_epoll_event ee;
    int mask = eventLoop->events[fd].mask & (~delmask);

    ee.events = 0;
    if (mask & AE_READABLE) ee.events |= MTCP_EPOLLIN;
    if (mask & AE_WRITABLE) ee.events |= MTCP_EPOLLOUT;
    ee.data.u64 = 0; /* avoid valgrind warning */
    ee.data.sockid = fd;
    if (mask != AE_NONE) {
        mtcp_epoll_ctl(mctx, state->epfd,MTCP_EPOLL_CTL_MOD,fd,&ee);
    } else {
        /* Note, Kernel < 2.6.9 requires a non null event pointer even for
         * EPOLL_CTL_DEL. */
        mtcp_epoll_ctl(mctx, state->epfd,MTCP_EPOLL_CTL_DEL,fd,&ee);
    }
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int retval, numevents = 0;

    retval = mtcp_epoll_wait(mctx, state->epfd,state->events,eventLoop->setsize,
            tvp ? (tvp->tv_sec*1000 + tvp->tv_usec/1000) : -1);
    if (retval > 0) {
        int j;

        numevents = retval;
        for (j = 0; j < numevents; j++) {
            int mask = 0;
            struct mtcp_epoll_event *e = state->events+j;

            if (e->events & MTCP_EPOLLIN) mask |= AE_READABLE;
            if (e->events & MTCP_EPOLLOUT) mask |= AE_WRITABLE;
            if (e->events & MTCP_EPOLLERR) mask |= AE_WRITABLE;
            if (e->events & MTCP_EPOLLHUP) mask |= AE_WRITABLE;
            eventLoop->fired[j].fd = e->data.sockid;
            eventLoop->fired[j].mask = mask;
        }
    }
    return numevents;
}

static char *aeApiName(void) {
    return "mtcp_epoll";
}
