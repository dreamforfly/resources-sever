/*
 * serwer.c
 *
 *  Created on: 2010-12-05
 *      Author: Jakub Bartodziej
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <errno.h>
#include <unistd.h>

#include <signal.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>

#include "err.h"
#include "common.h"

static int request_queue, notify_server_queue, notify_client_queue,
        terminate_queue;
static pthread_t signal_handler, main_loop, new_thread;
static pthread_mutex_t mutex;
static sigset_t int_mask;
static pthread_cond_t* resources;
static int* resource_empty;
static int terminating, thread_created, cancel_handler_created;
static pthread_cond_t for_handlers_to_terminate, for_thread_to_create,
        for_cancel_handler_to_create;

static int client_handlers_count, resources_count;

void unlock_mutex(void* arg) {
    int rc;

    CHECK_VALUE(pthread_mutex_unlock(&mutex), 0, rc);
}

/**
 * Client handler threads - they're created after receiving a request from
 * a client and terminate when the client yields his resources.
 */

typedef struct {
    int* first_resource;
    int reserved_resources_count;
} ClientHandlerCleanupArg;

/**
 * When a client handler thread is cancelled it yields all acquired resources
 * and signals the condition variables representing them.
 */
static void client_handler_cleanup(void* arg) {
    int rc, i;
    ClientHandlerCleanupArg* cleanup_arg = (ClientHandlerCleanupArg*) arg;
    int* ptr = cleanup_arg->first_resource;

    CHECK_VALUE(pthread_mutex_lock(&mutex), 0, rc);
    for (i = 0; i < cleanup_arg->reserved_resources_count; ++i, ++ptr) {
        resource_empty[*ptr - 1] = true;
        CHECK_VALUE(pthread_cond_signal(&resources[*ptr - 1]), 0, rc);
    }

    --client_handlers_count;
    if (client_handlers_count == 0) {
        CHECK_VALUE(pthread_cond_signal(&for_handlers_to_terminate), 0, rc);
    }

    CHECK_VALUE(pthread_mutex_unlock(&mutex), 0, rc);
}


/**
 * Client handlers acquire the resources in ascending order and handle the
 * communication with the client after receiving the Request.
 */
static void* client_handler_routine(void* arg) {
    int* resource;
    int rc;
    Notification notification;
    Request request;
    ClientHandlerCleanupArg cleanup_arg;

    pthread_cleanup_push(client_handler_cleanup, (void*) &cleanup_arg);
    pthread_cleanup_push(unlock_mutex, NULL);

    CHECK_VALUE(pthread_mutex_lock(&mutex), 0, rc);

    request = *((Request*) arg);
    cleanup_arg.first_resource = request.resource_ids;
    cleanup_arg.reserved_resources_count = 0;
    notification.pid = request.pid;

    ++client_handlers_count;

    thread_created = true;
    CHECK_VALUE(pthread_cond_signal(&for_thread_to_create), 0, rc);

    for (resource = request.resource_ids; *resource != 0; ++resource) {
        while (!resource_empty[*resource - 1] && !terminating) {
            CHECK_VALUE(pthread_cond_wait(&resources[*resource - 1], &mutex), 0, rc);
        }
        if (!terminating) {
            resource_empty[*resource - 1] = false;
            ++cleanup_arg.reserved_resources_count;
        } else {
            CHECK_VALUE(pthread_cancel(pthread_self()), 0, rc);
            pthread_testcancel();
        }
    }

    pthread_cleanup_pop(true);

    rc = SEND(notify_client_queue, notification);
    CHECK_QUEUE(rc);
    if (rc == QREMOVED) {
        CHECK_VALUE(pthread_cancel(pthread_self()), 0, rc);
        pthread_testcancel();
    } else {
        CHECK_VALUE(rc, QSUCCESS, rc);
    }

    rc = RECEIVE(notify_server_queue, notification, request.pid);
    CHECK_QUEUE(rc);
    if (rc == QREMOVED) {
        CHECK_VALUE(pthread_cancel(pthread_self()), 0, rc);
        pthread_testcancel();
    } else {
        CHECK_VALUE(rc, QSUCCESS, rc);
    }

    notification.type = FROM_THREAD;
    rc = SEND(terminate_queue, notification);
    CHECK_QUEUE(rc);
    if (rc != QREMOVED) {
        CHECK_VALUE(rc, QSUCCESS, rc);
    }

    pthread_cleanup_pop(true);

    return NULL;
}

/**
 * Client handlers work in pairs with cancel handlers. A cancel handler receives
 * a notification that the pair of threads should be terminated and safely terminates
 * itself and it's corresponding client handler. A termination notification may be
 * sent from the client handler.
 */
static void* cancel_handler_routine(void* arg) {
    int rc;
    pthread_t my_thread;
    long my_pid;
    Request* request = (Request*) arg;
    Notification notification;

    CHECK_VALUE(pthread_mutex_lock(&mutex), 0, rc);
    while (!thread_created) {
        CHECK_VALUE(pthread_cond_wait(&for_thread_to_create, &mutex), 0, rc);
    }
    my_thread = new_thread;
    my_pid = request->pid;
    cancel_handler_created = true;
    pthread_cond_signal(&for_cancel_handler_to_create);
    CHECK_VALUE(pthread_mutex_unlock(&mutex), 0, rc);

    rc = RECEIVE(terminate_queue, notification, my_pid);
    CHECK_QUEUE(rc);
    if (rc != QREMOVED) {
        CHECK_VALUE(rc, QSUCCESS, rc);
        CHECK_VALUE(pthread_mutex_lock(&mutex), 0, rc);
        if (!terminating && notification.type == FROM_CLIENT) {
            CHECK_VALUE(pthread_cancel(my_thread), 0, rc);
        }
        CHECK_VALUE(pthread_mutex_unlock(&mutex), 0, rc);
    }

    return NULL;
}

/**
 * The main loop thread receives the Requests and creates client handlers.
 */
static void* main_loop_routine(void* arg) {
    Request request;
    pthread_t new_cancel_handler;
    int rc, rc2;
    pthread_attr_t detached;

    CHECK_VALUE(pthread_attr_init(&detached), 0, rc);
    CHECK_VALUE(pthread_attr_setdetachstate(&detached, PTHREAD_CREATE_DETACHED), 0, rc);

    for (;;) {
        rc = RECEIVE(request_queue, request, 0L);
        CHECK_QUEUE(rc);
        if (rc == QREMOVED) {
            break;
        } else {
            CHECK_VALUE(rc, QSUCCESS, rc);
        }

        CHECK_VALUE(pthread_mutex_lock(&mutex), 0, rc);

        rc = pthread_create(&new_thread, &detached, client_handler_routine,
                (void*) &request);
        rc2 = pthread_create(&new_cancel_handler, &detached,
                cancel_handler_routine, (void*) &request);
        if (rc == EAGAIN || rc2 == EAGAIN) {
            if (rc == 0) {
                CHECK_VALUE(pthread_cancel(new_thread), 0, rc);
            } else if (rc != EAGAIN) {
                CHECK_VALUE(rc, 0, rc);
            }

            if (rc2 == 0) {
                CHECK_VALUE(pthread_cancel(new_cancel_handler), 0, rc);
            } else if (rc2 != EAGAIN) {
                CHECK_VALUE(rc2, 0, rc2);
            }

            CHECK_VALUE(pthread_mutex_unlock(&mutex), 0, rc);
            fprintf(stderr, "%s\n", "Couldn't create enough threads.");
            CHECK_SYSFUN(kill(getpid(), SIGINT));
        } else {
            CHECK_VALUE(rc, 0, rc);
            cancel_handler_created = false;
            thread_created = false;
            while (!cancel_handler_created) {
                CHECK_VALUE(pthread_cond_wait(&for_cancel_handler_to_create, &mutex), 0, rc);
            }
            CHECK_VALUE(pthread_mutex_unlock(&mutex), 0, rc);
        }

    }

    CHECK_VALUE(pthread_mutex_lock(&mutex), 0, rc);
    while (client_handlers_count > 0) {
        CHECK_VALUE(pthread_cond_wait(&for_handlers_to_terminate, &mutex), 0, rc);
    }
    CHECK_VALUE(pthread_mutex_unlock(&mutex), 0, rc);

    CHECK_VALUE(pthread_attr_destroy(&detached), 0, rc);

    return NULL;
}

/**
 * Signal handler cancels main loop and waits for all detached threads to reach
 * end of control upon receiving a SIGINT.
 */
static void* signal_handler_routine(void* arg) {
    int sig, i, rc;
    CHECK_SYSFUN(sigwait(&int_mask, &sig));

    CHECK_VALUE(pthread_mutex_lock(&mutex), 0, rc);

    terminating = true;

    CHECK_SYSFUN(msgctl(request_queue, IPC_RMID, NULL));
    CHECK_SYSFUN(msgctl(notify_server_queue, IPC_RMID, NULL));
    CHECK_SYSFUN(msgctl(notify_client_queue, IPC_RMID, NULL));
    CHECK_SYSFUN(msgctl(terminate_queue, IPC_RMID, NULL));

    for (i = 0; i < resources_count; ++i) {
        CHECK_VALUE(pthread_cond_broadcast(&resources[i]), 0, rc);
    }

    CHECK_VALUE(pthread_mutex_unlock(&mutex), 0, rc);

    return NULL;
}

static void parse_arguments(int argc, char** argv) {
    sscanf(argv[1], "%d", &resources_count);
}

static void initialize() {
    int i, rc;

    CHECK_SYSFUN(sigemptyset(&int_mask));
    CHECK_SYSFUN(sigaddset(&int_mask, SIGINT));

    CHECK_VALUE(pthread_sigmask(SIG_BLOCK, &int_mask, NULL), 0, rc);

    /* Separating request queues from notification queues allows to avoid a
     * situation where too much requests prevent notifications from being sent
     * and make yielding resources impossible. */

    CHECK_SYSFUN(notify_server_queue = msgget(ftok(PATHNAME, PROJ_ID_NOTIFY_SERVER), 0666 | IPC_CREAT | IPC_EXCL));
    CHECK_SYSFUN(notify_client_queue = msgget(ftok(PATHNAME, PROJ_ID_NOTIFY_CLIENT), 0666 | IPC_CREAT | IPC_EXCL));
    CHECK_SYSFUN(request_queue = msgget(ftok(PATHNAME, PROJ_ID_REQUEST), 0666 | IPC_CREAT | IPC_EXCL));
    CHECK_SYSFUN(terminate_queue = msgget(ftok(PATHNAME, PROJ_ID_TERMINATE_QUEUE), 0666 | IPC_CREAT | IPC_EXCL));

    CHECK_VALUE(pthread_mutex_init(&mutex, NULL), 0, rc);

    client_handlers_count = 0;
    terminating = false;

    CHECK_VALUE(pthread_cond_init(&for_handlers_to_terminate, NULL), 0, rc);
    CHECK_VALUE(pthread_cond_init(&for_thread_to_create, NULL), 0, rc);

    TRY_MALLOC(resources_count * sizeof(*resources), resources);
    TRY_MALLOC(resources_count * sizeof(*resource_empty), resource_empty);

    for (i = 0; i < resources_count; ++i) {
        CHECK_VALUE(pthread_cond_init(&resources[i], NULL), 0, rc);
        resource_empty[i] = true;
    }
}

static void create_threads() {
    int crc[2], rc;
    pthread_attr_t joinable;

    CHECK_VALUE(pthread_attr_init(&joinable), 0, rc);
    CHECK_VALUE(pthread_attr_setdetachstate(&joinable, PTHREAD_CREATE_JOINABLE), 0, rc);

    CHECK_VALUE(pthread_mutex_lock(&mutex), 0, rc);

    crc[0] = pthread_create(&signal_handler, &joinable, signal_handler_routine,
            NULL);
    crc[1] = pthread_create(&main_loop, &joinable, main_loop_routine, NULL);

    if (crc[0] || crc[1]) {
        if (!crc[0]) {
            CHECK_VALUE(pthread_cancel(signal_handler), 0, rc);
        }

        if (!crc[1]) {
            CHECK_VALUE(pthread_cancel(main_loop), 0, rc);
        }

        exit(EXIT_FAILURE);
    }

    CHECK_VALUE(pthread_mutex_unlock(&mutex), 0, rc);
}

static void join_threads() {
    int rc;

    CHECK_VALUE(pthread_join(signal_handler, NULL), 0, rc);
    CHECK_VALUE(pthread_join(main_loop, NULL), 0, rc);
}

static void deinitialize() {
    int i, rc;

    CHECK_VALUE(pthread_mutex_destroy(&mutex), 0, rc);

    CHECK_VALUE(pthread_cond_destroy(&for_handlers_to_terminate), 0, rc);
    CHECK_VALUE(pthread_cond_destroy(&for_thread_to_create), 0, rc);

    for (i = 0; i < resources_count; ++i) {
        CHECK_VALUE(pthread_cond_destroy(&resources[i]), 0, rc);
    }

    free(resources);
    free(resource_empty);
}

int main(int argc, char **argv) {
    parse_arguments(argc, argv);

    initialize();
    create_threads();
    join_threads();
    deinitialize();

    return EXIT_SUCCESS;
}
