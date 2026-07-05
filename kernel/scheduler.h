#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

#define SCHED_BASE_WEIGHT (1 << 18)

typedef struct Client Client;
typedef struct {
    size_t count;
    Client* data[256];
} ClientQueue;

typedef enum ClientStatus {
    // newly created task
    CLIENT_FRESH,

    // internally interrupted, we're waiting on something
    CLIENT_BLOCKED,

    // running or runnable
    CLIENT_READY,

    // externally interrupted, something has told us to stop
    // CLIENT_PAUSED,

    // signalled as dead but not acknowledged
    CLIENT_ZOMBIE,
} ClientStatus;

struct Client {
    ClientStatus status;

    int id;

    bool is_blocked;
    bool is_dead;

    // scheduler props
    int64_t weight;
    int64_t slice;

    // physical time
    int64_t start_time;
    int64_t last_signal;
    int64_t wake_time;

    // unweighted
    int64_t lag;

    // virtual time
    int64_t v_time;
    int64_t v_deadline;

    // stats
    int64_t latency, latency_count;

    Client* next_in_blocked;
};

typedef struct Server Server;
struct Server {
    int64_t last_pick;
    int64_t v_time;

    int64_t sum_v_time;
    int64_t total_weight;

    // replace data structure at some point, ideally
    // with some flavor of balanced binary tree.
    int active_count;
    Client* active[256];

    // whichever client is currently running, this client
    // isn't currently in the active queue but is active.
    Client* curr;

    // blocked clients
    ClientQueue sleepers;
    _Atomic(Client*) blocked_list;
};

void sched_resume_thread(Server* server, Client* client);
Client* sched_pick_client(Server* server, uint64_t now_time, uint64_t* next_t);

#endif // SCHED_H

#ifdef SCHED_IMPL

#ifndef SCHED_ASSERT
#include <assert.h>
#define SCHED_ASSERT(x) assert(x)
#endif

#ifdef SCHED_TEST
#include "spall.h"
#endif

static int wakequeue_cmp(Client* a, Client* b) {
    return a->wake_time - b->wake_time;
}

#define PRIO_ELEM     Client*
#define PRIO_TYPE     ClientQueue
#define PRIO_FN(name) wakequeue_ ## name
#include "prio_queue.h"

#if 1
void sched_dump(Server* s) {
    kprintf("%10ld [ ", s->v_time);
    for (int i = 0; i < s->active_count; i++) {
        Client* c = s->active[i];
        kprintf("C%02d:%-12ld ", c->id, c->v_time);
    }
    kprintf("]\n");
}
#endif

static uint64_t mul_weight(uint64_t delta, uint64_t weight) {
    return (delta * weight) / SCHED_BASE_WEIGHT;
}

static uint64_t div_weight(uint64_t delta, uint64_t weight) {
    SCHED_ASSERT(weight);
    return (delta * SCHED_BASE_WEIGHT) / weight;
}

static uint64_t get_current_vt(Server* s) {
    return s->total_weight ? (s->sum_v_time / s->total_weight) : 0;
}

static size_t sched__find(Server* server, int64_t key) {
    size_t left = 0, right = server->active_count;
    while (left != right) {
        size_t i = (left + right) / 2;
        int64_t key_at_idx = server->active[i]->v_time;
        if (key_at_idx <= key) {
            right = i;
        } else {
            left = i + 1;
        }
    }
    return left;
}

static void sched__insert(Server* server, Client* client) {
    int64_t left = sched__find(server, client->v_time);
    SCHED_ASSERT(server->active_count < 256);

    // shift up
    for (size_t i = server->active_count; i-- > left;) {
        server->active[i + 1] = server->active[i];
    }
    server->active[left] = client;
    server->active_count += 1;
}

int64_t sched_pick_slice(Server* server, Client* client) {
    if (server->total_weight == 0) {
        return 1000;
    }

    int64_t slice = (15000 * client->weight) / server->total_weight;
    return slice < 1000 ? 1000 : slice;
}

static void sched__join(Server* server, Client* client) {
    uint64_t slice = sched_pick_slice(server, client);
    uint64_t v_slice = div_weight(client->slice, client->weight);

    // give newly created threads half the time slice since
    // they're joining into existing competition which is "on average"
    // halfway executed.
    if (client->status == CLIENT_FRESH) {
        v_slice = (v_slice + 1) / 2;
    }

    uint64_t old = client->v_time;

    // issue request
    client->status     = CLIENT_READY;
    client->is_blocked = false;
    client->wake_time  = 0;
    client->v_time     = get_current_vt(server) - client->lag;
    client->v_deadline = client->v_time + v_slice;

    // kprintf("JOIN %ld C%d %ld %ld (%ld)\n", get_current_vt(server), client->id, client->v_time, client->lag, old);

    server->sum_v_time   += client->v_time * client->weight;
    server->total_weight += client->weight;
    sched__insert(server, client);
}

static void sched__leave(Server* server, Client* client) {
    // kprintf("LAG C%d %ld %ld\n", client->id, server->v_time, client->v_time);

    server->sum_v_time   -= client->v_time * client->weight;
    server->total_weight -= client->weight;

    client->status = client->is_dead ? CLIENT_ZOMBIE : CLIENT_BLOCKED;
    client->lag    = server->v_time - client->v_time;

    int64_t limit = div_weight(15000, client->weight);
    if (client->lag >  limit) { client->lag =  limit; }
    if (client->lag < -limit) { client->lag = -limit; }
}

static Client* sched__pop(Server* server) {
    int64_t v_time = server->v_time;

    #if 0
    int64_t sum_t = 0, sum_w = 0, sum_lag = 0;
    for (size_t i = 0; i < server->active_count; i++) {
        Client* c = server->active[i];

        int64_t lag = (server->v_time - c->v_time) * c->weight;
        sum_lag += lag;

        sum_t += c->v_time * c->weight;
        sum_w += c->weight;
    }
    SCHED_ASSERT(v_time == sum_t / sum_w);
    #endif

    int least_i = 0;
    Client* least = NULL;

    // skip all ineligible clients
    int64_t left = sched__find(server, v_time);
    for (size_t i = left; i < server->active_count; i++) {
        Client* t = server->active[i];
        SCHED_ASSERT(t->v_time <= v_time);

        if (least == NULL || least->v_deadline >= t->v_deadline) {
            least = t;
            least_i = i;
        }
    }

    // remove-shift
    SCHED_ASSERT(least != NULL);
    server->active_count -= 1;
    for (size_t i = least_i; i < server->active_count; i++) {
        server->active[i] = server->active[i + 1];
    }
    return least;
}

Client* sched_pick_client(Server* server, uint64_t now_time, uint64_t* next_t) {
    // simple monotonic time assertion
    SCHED_ASSERT(server->last_pick <= now_time);
    server->last_pick = now_time;

    // update current
    Client* curr = server->curr;
    if (curr) {
        #ifdef SCHED_TEST
        spall_end_event(curr->id, now_time);
        #endif

        // adjust virtual timeline
        uint64_t delta = now_time - curr->start_time;
        server->sum_v_time += div_weight(delta, curr->weight) * curr->weight;
        curr->v_time += div_weight(delta, curr->weight);

        if (curr->wake_time > 0 || curr->is_blocked || curr->is_dead) {
            // yield remaining time slice
            server->v_time = get_current_vt(server);
            sched__leave(server, curr);

            // uint64_t old_dead = curr->start_time + mul_weight(curr->v_deadline - curr->v_time, curr->weight);
            // kprintf("STOPPING (%ld %ld)\n", now_time, old_dead);

            if (curr->wake_time > 0) {
                wakequeue_insert(&server->sleepers, curr);
            }
        } else {
            // issue new request
            int64_t dist_to_deadline = (curr->v_deadline - curr->v_time) * curr->weight;
            if (dist_to_deadline < SCHED_BASE_WEIGHT) { // curr->v_time >= curr->v_deadline) {
                uint64_t slice = sched_pick_slice(server, curr);
                curr->v_deadline = curr->v_time + div_weight(slice, curr->weight);
            }

            curr->status = CLIENT_READY;
            sched__insert(server, curr);
        }
    }

    // wake up sleeping clients
    int64_t next_wake_time = INT64_MAX;
    while (server->sleepers.count > 0) {
        Client* c = wakequeue_peek(&server->sleepers);
        if (now_time < c->wake_time) {
            next_wake_time = c->wake_time;
            break;
        }

        wakequeue_pop(&server->sleepers);
        sched__join(server, c);
    }

    // wake up all blocked clients
    Client* list = atomic_exchange(&server->blocked_list, NULL);
    while (list) {
        sched__join(server, list);
        list = list->next_in_blocked;
    }

    server->v_time = get_current_vt(server);
    if (server->active_count == 0) {
        int64_t t = server->sleepers.count ? wakequeue_peek(&server->sleepers)->wake_time : INT64_MAX;
        kassert(t == next_wake_time, "WOAH %ld %ld\n", t, next_wake_time);

        *next_t = next_wake_time;
        return NULL;
    }

    /* kprintf("\nNOW: %ld\nSLEEPER [", now_time);
    for (size_t i = 0; i < server->sleepers.count; i++) {
        kprintf("%p:%ld  ", server->sleepers.data[i], now_time - server->sleepers.data[i]->wake_time);
    }
    kprintf("]\n"); */

    // pick task with earliest eligible deadline
    curr = server->curr = sched__pop(server);
    {
        curr->latency += now_time - curr->start_time;
        curr->latency_count += 1;
    }
    curr->start_time = now_time;

    // in real time
    uint64_t delta = mul_weight(curr->v_deadline - curr->v_time, curr->weight);
    uint64_t deadline = now_time + delta;
    if (deadline > next_wake_time) {
        deadline = next_wake_time;
    }

    #ifdef SCHED_TEST
    char name[10];
    snprintf(name, 10, "S%ld", (long) curr->id);
    spall_begin_event(name, curr->id, now_time);
    #endif

    *next_t = deadline;
    return curr;
}

void sched_resume_thread(Server* server, Client* client) {
    Client* latest = atomic_load_explicit(&server->blocked_list, memory_order_relaxed);
    do {
        client->next_in_blocked = latest;
    } while (!atomic_compare_exchange_strong_explicit(&server->blocked_list, &latest, client, memory_order_acq_rel, memory_order_acquire));
}
#endif // SCHED_IMPL

#if SCHED_TEST
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

static Client* pick_client2(Server* server, uint64_t time, uint64_t* next_t) {
    // put to bed after 1ms
    Client* c = sched_pick_client(server, time, next_t);
    if (c->id == 1) {
        spall_end_event(99, time);
        printf("SIGNAL,%ld\n", time - c->last_signal);

        if (*next_t >= time+1000) {
            *next_t = time+1000;
        }

        c->is_blocked = true;
        c = sched_pick_client(server, *next_t, next_t);
    }
    return c;
}

static int ID_TICKER = 0;
int main() {
    spall_header();

    static Server server = { 0 };

    // spawn some tasks and let them run
    static Client clients[10] = { 0 };
    int64_t period = 0;
    for (int i = 0; i < 10; i++) {
        Client* c = &clients[i];
        c->id = ++ID_TICKER;
        c->weight = 10;
        if (i == 0 || i == 9) {
            c->slice = 1500;
        } else if (i == 9) {
            c->slice = 100;
        } else if (i & 1) {
            c->slice = 300;
        } else {
            c->slice = 700;
        }
        period += c->slice;

        if (i == 0) {
            c->weight = 54;
        }

        if (i != 0) {
            sched_resume_thread(&server, &clients[i]);
        }
    }

    printf("Period %ld\n", period);

    uint64_t time = 0;
    uint64_t next_vsync = 2000;
    for (int i = 0; i < 1200; i++) {
        // sched_dump(&server);

        uint64_t next_t;
        Client* c = pick_client2(&server, time, &next_t);

        // interrupt at VSYNC
        if (next_t >= next_vsync) {
            if (next_vsync > time) {
                next_t = next_vsync;
            }

            // wake up S1, pick_client again to simulate an interrupt
            if (clients[0].status == CLIENT_FRESH || clients[0].status == CLIENT_BLOCKED) {
                clients[0].last_signal = next_vsync;

                spall_begin_event("SIGNAL", 99, next_vsync);
                sched_resume_thread(&server, &clients[0]);
            }

            next_vsync += 16666;
            c = pick_client2(&server, next_t, &next_t);
        }
        time = next_t;
    }

    for (int i = 0; i < 10; i++) {
        Client* c = &clients[i];
        printf("%ld;", c->latency / c->latency_count);
    }
    printf("\n");

    fclose(spall_file);
    return 0;
}
#endif
