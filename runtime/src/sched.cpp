// N64 threads and message queues on KallistiOS threads.
//
// Every N64 thread gets a KOS thread, but only one of them runs game code at a time: the one in
// `running`. The others wait on their own semaphore. Control changes hands the way it does in
// libultra, by priority, whenever the running thread blocks or makes a higher-priority thread
// ready. Events from the runtime's own threads (VI, timers) can make a higher-priority thread
// ready while game code runs; that switch happens at the running thread's next runtime call.

#include <kos.h>

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "ultra.h"

// OSThread fields.
#define OSTHREAD_PRIORITY 0x04
#define OSTHREAD_STATE    0x10
#define OSTHREAD_ID       0x14

#define OS_STATE_STOPPED  1
#define OS_STATE_RUNNABLE 2
#define OS_STATE_RUNNING  4
#define OS_STATE_WAITING  8

// OSMesgQueue fields.
#define MQ_MTQUEUE    0x00
#define MQ_FULLQUEUE  0x04
#define MQ_VALIDCOUNT 0x08
#define MQ_FIRST      0x0C
#define MQ_MSGCOUNT   0x10
#define MQ_MSG        0x14

#define GAME_THREAD_STACK_SIZE (64 * 1024)
#define GAME_THREAD_KOS_PRIO   PRIO_DEFAULT

namespace {

struct GameThread {
    uint32_t vram;
    uint32_t entry;
    uint32_t arg;
    uint32_t sp;
    int32_t id;
    kthread_t* kthd;
    semaphore_t run;
    recomp_context ctx;
    uint32_t ready_seq;
    bool started;
};

struct Waiters {
    std::vector<GameThread*> recv;
    std::vector<GameThread*> send;
};

mutex_t lock = MUTEX_INITIALIZER;
std::unordered_map<uint32_t, GameThread*> threads;
std::vector<GameThread*> ready;
std::unordered_map<uint32_t, Waiters> waiters;
GameThread* running;
uint32_t ready_seq_counter;
bool boot_handed_off;
volatile bool yield_requested;

int32_t priority(const GameThread* t) {
    return MEM_W(OSTHREAD_PRIORITY, t->vram);
}

void set_state(const GameThread* t, uint16_t state) {
    MEM_HU(OSTHREAD_STATE, t->vram) = state;
}

GameThread* self() {
    GameThread* r = running;
    if (r != nullptr && r->kthd == thd_get_current()) {
        return r;
    }
    return nullptr;
}

// Highest priority first, then the one that became ready first.
bool runs_before(const GameThread* a, const GameThread* b) {
    int32_t pa = priority(a), pb = priority(b);
    return pa != pb ? pa > pb : a->ready_seq < b->ready_seq;
}

GameThread* best_ready() {
    GameThread* best = nullptr;
    for (GameThread* t : ready) {
        if (best == nullptr || runs_before(t, best)) {
            best = t;
        }
    }
    return best;
}

void remove_from(std::vector<GameThread*>& list, GameThread* t) {
    list.erase(std::remove(list.begin(), list.end(), t), list.end());
}

void dispatch_next_locked() {
    GameThread* next = best_ready();
    running = next;
    if (next != nullptr) {
        remove_from(ready, next);
        set_state(next, OS_STATE_RUNNING);
        sem_signal(&next->run);
    }
}

void make_ready_locked(GameThread* t) {
    if (std::find(ready.begin(), ready.end(), t) != ready.end()) {
        return;
    }
    t->ready_seq = ++ready_seq_counter;
    ready.push_back(t);
    set_state(t, OS_STATE_RUNNABLE);
    if (running == nullptr) {
        dispatch_next_locked();
    } else if (priority(t) > priority(running)) {
        yield_requested = true;
    }
}

// Gives up the CPU and returns once the scheduler hands it back. With `requeue` the thread stays
// ready; without, something else has to make it ready again (a message, osStartThread).
void switch_away_locked(GameThread* me, bool requeue) {
    if (requeue) {
        me->ready_seq = ++ready_seq_counter;
        ready.push_back(me);
        set_state(me, OS_STATE_RUNNABLE);
    } else {
        set_state(me, OS_STATE_WAITING);
    }
    dispatch_next_locked();
    mutex_unlock(&lock);
    sem_wait(&me->run);
    mutex_lock(&lock);
}

void preempt_if_needed_locked(GameThread* me) {
    yield_requested = false;
    GameThread* best = best_ready();
    if (best != nullptr && priority(best) > priority(me)) {
        switch_away_locked(me, true);
    }
}

GameThread* wake_best(std::vector<GameThread*>& list) {
    if (list.empty()) {
        return nullptr;
    }
    GameThread* best = list.front();
    for (GameThread* t : list) {
        if (priority(t) > priority(best)) {
            best = t;
        }
    }
    remove_from(list, best);
    make_ready_locked(best);
    return best;
}

void* game_thread_main(void* param) {
    GameThread* t = static_cast<GameThread*>(param);
    sem_wait(&t->run);

    printf("[q64dc] thread %d running (entry 0x%08X, priority %d)\n", (int)t->id, (unsigned)t->entry, (int)priority(t));

    t->ctx.r29 = t->sp;
    t->ctx.r4 = t->arg;
    LOOKUP_FUNC(t->entry)(rdram, &t->ctx);

    printf("[q64dc] thread %d returned\n", (int)t->id);
    mutex_lock(&lock);
    set_state(t, OS_STATE_STOPPED);
    dispatch_next_locked();
    mutex_unlock(&lock);
    return nullptr;
}

GameThread* find_thread(uint32_t vram) {
    auto it = threads.find(vram);
    return it == threads.end() ? nullptr : it->second;
}

GameThread* thread_arg(uint32_t vram) {
    if (vram == 0) {
        GameThread* me = self();
        if (me == nullptr) {
            runtime_fatal("[q64dc] thread function called with a null thread outside a game thread");
        }
        return me;
    }
    GameThread* t = find_thread(vram);
    if (t == nullptr) {
        runtime_fatal("[q64dc] unknown OSThread 0x%08X", (unsigned)vram);
    }
    return t;
}

int32_t recv_locked(GameThread* me, uint32_t mq, uint32_t msg_out, int32_t flag) {
    while (MEM_W(MQ_VALIDCOUNT, mq) == 0) {
        if (flag == OS_MESG_NOBLOCK) {
            return -1;
        }
        if (me == nullptr) {
            runtime_fatal("[q64dc] blocking osRecvMesg outside a game thread (queue 0x%08X)", (unsigned)mq);
        }
        waiters[mq].recv.push_back(me);
        switch_away_locked(me, false);
    }

    int32_t first = MEM_W(MQ_FIRST, mq);
    int32_t count = MEM_W(MQ_MSGCOUNT, mq);
    uint32_t msg = (uint32_t)MEM_W(first * 4, (uint32_t)MEM_W(MQ_MSG, mq));
    if (msg_out != 0) {
        MEM_W(0, msg_out) = msg;
    }
    MEM_W(MQ_FIRST, mq) = (first + 1) % count;
    MEM_W(MQ_VALIDCOUNT, mq) = MEM_W(MQ_VALIDCOUNT, mq) - 1;

    auto it = waiters.find(mq);
    if (it != waiters.end()) {
        wake_best(it->second.send);
    }
    return 0;
}

int32_t send_locked(GameThread* me, uint32_t mq, uint32_t msg, int32_t flag, bool jam) {
    while (MEM_W(MQ_VALIDCOUNT, mq) >= MEM_W(MQ_MSGCOUNT, mq)) {
        if (flag == OS_MESG_NOBLOCK || me == nullptr) {
            return -1;
        }
        waiters[mq].send.push_back(me);
        switch_away_locked(me, false);
    }

    int32_t first = MEM_W(MQ_FIRST, mq);
    int32_t count = MEM_W(MQ_MSGCOUNT, mq);
    int32_t valid = MEM_W(MQ_VALIDCOUNT, mq);
    uint32_t msgs = (uint32_t)MEM_W(MQ_MSG, mq);
    if (jam) {
        first = (first + count - 1) % count;
        MEM_W(first * 4, msgs) = msg;
        MEM_W(MQ_FIRST, mq) = first;
    } else {
        MEM_W(((first + valid) % count) * 4, msgs) = msg;
    }
    MEM_W(MQ_VALIDCOUNT, mq) = valid + 1;

    auto it = waiters.find(mq);
    if (it != waiters.end()) {
        wake_best(it->second.recv);
    }
    return 0;
}

}

void sched_init(void) {
}

void sched_lock(void) {
    mutex_lock(&lock);
}

void sched_unlock(void) {
    mutex_unlock(&lock);
}

void sched_check_yield(void) {
    if (!yield_requested) {
        return;
    }
    mutex_lock(&lock);
    GameThread* me = self();
    if (me != nullptr) {
        preempt_if_needed_locked(me);
    }
    mutex_unlock(&lock);
}

int32_t mq_send_nonblock(uint32_t mq, uint32_t msg) {
    mutex_lock(&lock);
    GameThread* me = self();
    int32_t ret = send_locked(me, mq, msg, OS_MESG_NOBLOCK, false);
    if (me != nullptr) {
        preempt_if_needed_locked(me);
    }
    mutex_unlock(&lock);
    return ret;
}

extern "C" void osCreateThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t vram = ctx->r4;
    int32_t id = (int32_t)ctx->r5;
    uint32_t entry = ctx->r6;
    uint32_t arg = ctx->r7;
    uint32_t sp = STACK_ARG(ctx, 4);
    int32_t pri = (int32_t)STACK_ARG(ctx, 5);

    mutex_lock(&lock);
    if (find_thread(vram) != nullptr) {
        runtime_fatal("[q64dc] osCreateThread on live thread 0x%08X", (unsigned)vram);
    }

    MEM_W(0x00, vram) = 0;
    MEM_W(OSTHREAD_PRIORITY, vram) = pri;
    MEM_W(0x08, vram) = 0;
    MEM_W(0x0C, vram) = 0;
    MEM_HU(OSTHREAD_STATE, vram) = OS_STATE_STOPPED;
    MEM_HU(0x12, vram) = 0;
    MEM_W(OSTHREAD_ID, vram) = id;

    GameThread* t = new GameThread{};
    t->vram = vram;
    t->entry = entry;
    t->arg = arg;
    // Leave room for the o32 argument save area, as a caller would.
    t->sp = sp - 0x10;
    t->id = id;
    t->ctx.f_odd = &t->ctx.f0.u32h;
    sem_init(&t->run, 0);

    kthread_attr_t attr = {};
    attr.stack_size = GAME_THREAD_STACK_SIZE;
    attr.prio = GAME_THREAD_KOS_PRIO;
    attr.label = "n64 thread";
    attr.create_detached = true;
    t->kthd = thd_create_ex(&attr, game_thread_main, t);
    if (t->kthd == nullptr) {
        runtime_fatal("[q64dc] couldn't create a KOS thread for N64 thread %d", (int)id);
    }
    threads[vram] = t;
    mutex_unlock(&lock);

    printf("[q64dc] osCreateThread id %d entry 0x%08X priority %d stack 0x%08X\n", (int)id, (unsigned)entry, (int)pri, (unsigned)sp);
}

extern "C" void osStartThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    mutex_lock(&lock);
    GameThread* t = thread_arg(ctx->r4);
    GameThread* me = self();
    t->started = true;
    if (t != running) {
        make_ready_locked(t);
    }

    if (me == nullptr && !boot_handed_off) {
        // The boot code starts the first thread and never runs again, as on the N64.
        boot_handed_off = true;
        mutex_unlock(&lock);
        for (;;) {
            thd_sleep(1000000);
        }
    }
    if (me != nullptr) {
        preempt_if_needed_locked(me);
    }
    mutex_unlock(&lock);
}

extern "C" void osStopThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    mutex_lock(&lock);
    GameThread* t = thread_arg(ctx->r4);
    GameThread* me = self();
    if (t == me) {
        switch_away_locked(me, false);
    } else {
        remove_from(ready, t);
        for (auto& [mq, w] : waiters) {
            remove_from(w.recv, t);
            remove_from(w.send, t);
        }
        set_state(t, OS_STATE_STOPPED);
    }
    mutex_unlock(&lock);
}

extern "C" void osSetThreadPri_recomp(uint8_t* rdram, recomp_context* ctx) {
    mutex_lock(&lock);
    GameThread* t = thread_arg(ctx->r4);
    MEM_W(OSTHREAD_PRIORITY, t->vram) = (int32_t)ctx->r5;
    GameThread* me = self();
    if (me != nullptr) {
        preempt_if_needed_locked(me);
    }
    mutex_unlock(&lock);
}

extern "C" void osGetThreadPri_recomp(uint8_t* rdram, recomp_context* ctx) {
    mutex_lock(&lock);
    GameThread* t = thread_arg(ctx->r4);
    ctx->r2 = (gpr)priority(t);
    mutex_unlock(&lock);
}

extern "C" void pause_self(uint8_t* rdram) {
    (void)rdram;
    // The idle thread's endless loop: it only ever runs when nothing else can, so park it.
    mutex_lock(&lock);
    GameThread* me = self();
    if (me == nullptr) {
        runtime_fatal("[q64dc] pause_self outside a game thread");
    }
    printf("[q64dc] thread %d is idling\n", (int)me->id);
    for (;;) {
        switch_away_locked(me, false);
    }
}

extern "C" void osCreateMesgQueue_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t mq = ctx->r4;
    mutex_lock(&lock);
    MEM_W(MQ_MTQUEUE, mq) = 0;
    MEM_W(MQ_FULLQUEUE, mq) = 0;
    MEM_W(MQ_VALIDCOUNT, mq) = 0;
    MEM_W(MQ_FIRST, mq) = 0;
    MEM_W(MQ_MSGCOUNT, mq) = (int32_t)ctx->r6;
    MEM_W(MQ_MSG, mq) = (int32_t)ctx->r5;
    waiters.erase(mq);
    mutex_unlock(&lock);
}

extern "C" void osRecvMesg_recomp(uint8_t* rdram, recomp_context* ctx) {
    mutex_lock(&lock);
    GameThread* me = self();
    int32_t ret = recv_locked(me, ctx->r4, ctx->r5, (int32_t)ctx->r6);
    if (me != nullptr) {
        preempt_if_needed_locked(me);
    }
    mutex_unlock(&lock);
    ctx->r2 = (gpr)ret;
}

extern "C" void osSendMesg_recomp(uint8_t* rdram, recomp_context* ctx) {
    mutex_lock(&lock);
    GameThread* me = self();
    int32_t ret = send_locked(me, ctx->r4, ctx->r5, (int32_t)ctx->r6, false);
    if (me != nullptr) {
        preempt_if_needed_locked(me);
    }
    mutex_unlock(&lock);
    ctx->r2 = (gpr)ret;
}

extern "C" void osJamMesg_recomp(uint8_t* rdram, recomp_context* ctx) {
    mutex_lock(&lock);
    GameThread* me = self();
    int32_t ret = send_locked(me, ctx->r4, ctx->r5, (int32_t)ctx->r6, true);
    if (me != nullptr) {
        preempt_if_needed_locked(me);
    }
    mutex_unlock(&lock);
    ctx->r2 = (gpr)ret;
}
