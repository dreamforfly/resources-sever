/*
 * klient.c
 *
 *  Created on: 2010-12-05
 *      Author: Jakub Bartodziej
 */

/**
 * Most of the function names tell exaclty what they do, hence the
 * restrained commentary.
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
static pthread_t signal_handler, main_loop;
static pthread_mutex_t mutex;
static sigset_t int_mask;

static int sleep_time, loop_count;
static Request request;

static char const* queue_removed =
        "Error while operating on queue. It has probably been removed.";

static void deinitialize() {
    int rc; CHECK_VALUE(pthread_mutex_destroy(&mutex), 0, rc);
}

static void unlock_mutex(void *arg) {
    int rc; CHECK_VALUE(pthread_mutex_unlock(&mutex), 0, rc);
}

/**
 * Main loop and signal handler can use this to cancel each other.
 */
static void synchronized_cancel(pthread_t const* thread) {
    int rc;

    pthread_cleanup_push(unlock_mutex, NULL);

    CHECK_VALUE(pthread_mutex_lock(&mutex), 0, rc);
    pthread_testcancel();
    CHECK_VALUE(pthread_cancel(*thread), 0, rc);

    pthread_cleanup_pop(true);
}

static void* signal_handler_routine(void* arg) {
    int sig, rc;

    CHECK_VALUE(sigwait(&int_mask, &sig), 0, rc);
    synchronized_cancel(&main_loop);

    return NULL;
}

static void cancel_server_thread(void* arg) {
    Notification notification;
    int rc;

    notification.pid = getpid();
    notification.type = FROM_CLIENT;

    rc = SEND(terminate_queue, notification);
    CHECK_QUEUE(rc);
    if (rc == QREMOVED) {
        fprintf(stderr, "%s\n", queue_removed);
    } else {
        CHECK_VALUE(rc, QSUCCESS, rc);
    }
}

static void* main_loop_routine(void* arg) {
    int rc;
    Notification notification;

    while (loop_count--) {
        rc = SEND(request_queue, request);
        CHECK_QUEUE(rc);
        if (rc == QREMOVED) {
            fprintf(stderr, "%s\n", queue_removed);
            break;
        } else {
            CHECK_VALUE(rc, QSUCCESS, rc);
        }

        pthread_cleanup_push(cancel_server_thread, NULL);

        rc = RECEIVE(notify_client_queue, notification, (long) getpid());
        CHECK_QUEUE(rc);
        if (rc == QREMOVED) {
            fprintf(stderr, "%s\n", queue_removed);
            break;
        } else {
            CHECK_VALUE(rc, QSUCCESS, rc);
        }

        sleep(sleep_time);

        rc = SEND(notify_server_queue, notification);
        CHECK_QUEUE(rc);
        if (rc == QREMOVED) {
            fprintf(stderr, "%s\n", queue_removed);
            break;
        } else {
            CHECK_VALUE(rc, QSUCCESS, rc);
        }

        pthread_cleanup_pop(false);
    }

    synchronized_cancel(&signal_handler);
    return NULL;
}

static void initialize() {
    int rc;

    CHECK_SYSFUN(sigemptyset(&int_mask));
    CHECK_SYSFUN(sigaddset(&int_mask, SIGINT));

    CHECK_VALUE(pthread_sigmask(SIG_BLOCK, &int_mask, NULL), 0, rc);

    notify_server_queue = msgget(ftok(PATHNAME, PROJ_ID_NOTIFY_SERVER), 0666);
    notify_client_queue = msgget(ftok(PATHNAME, PROJ_ID_NOTIFY_CLIENT), 0666);
    request_queue = msgget(ftok(PATHNAME, PROJ_ID_REQUEST), 0666);
    terminate_queue = msgget(ftok(PATHNAME, PROJ_ID_TERMINATE_QUEUE), 0666);

    if (notify_server_queue == -1 || request_queue == -1 || notify_client_queue
            == -1 || terminate_queue == -1) {
        fprintf(stderr, "%s\n",
                "At least one of the required IPC message queues could not be accessed.");
        exit(EXIT_FAILURE);
    }

    CHECK_VALUE(pthread_mutex_init(&mutex, NULL), 0, rc);
}

static int int_compare(const void* a, const void* b) {
    return (*(int*) a - *(int*) b);
}

static void parse_arguments(int argc, char** argv) {
    int i;

    sscanf(argv[1], "%d", &loop_count);
    sscanf(argv[2], "%d", &sleep_time);

    request.pid = getpid();
    for (i = 3; i < argc; ++i) {
        sscanf(argv[i], "%d", &request.resource_ids[i - 3]);
    }

    /* Resources will be acquired in ascending order. */
    request.resource_ids[argc - 3] = 0;
    qsort(request.resource_ids, argc - 3, sizeof(*request.resource_ids),
            int_compare);
}

static void create_threads() {
    int rc;
    pthread_attr_t joinable;

    CHECK_VALUE(pthread_attr_init(&joinable), 0, rc);
    CHECK_VALUE(pthread_attr_setdetachstate(&joinable, PTHREAD_CREATE_JOINABLE), 0, rc);

    CHECK_VALUE(pthread_mutex_lock(&mutex), 0, rc);

    rc = pthread_create(&signal_handler, &joinable, signal_handler_routine,
            NULL);
    if (rc == EAGAIN) {
        fprintf(stderr, "%s\n", "Cannot create the signal handler thread");
        exit(EXIT_FAILURE);
    }

    rc = pthread_create(&main_loop, &joinable, main_loop_routine, NULL);
    if (rc == EAGAIN) {
        CHECK_VALUE(pthread_cancel(signal_handler), 0, rc);
        fprintf(stderr, "%s\n", "Cannot create the main loop thread.");
        exit(EXIT_FAILURE);
    }

    CHECK_VALUE(pthread_mutex_unlock(&mutex), 0, rc);

    CHECK_VALUE(pthread_attr_destroy(&joinable), 0, rc);
}

static void join_threads() {
    int rc;

    CHECK_VALUE(pthread_join(signal_handler, NULL), 0, rc);
    CHECK_VALUE(pthread_join(main_loop, NULL), 0, rc);
}

int main(int argc, char** argv) {
    parse_arguments(argc, argv);

    initialize();
    create_threads();
    join_threads();
    deinitialize();

    return EXIT_SUCCESS;
}
