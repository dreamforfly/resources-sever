/*
 * communication.h
 *
 *  Created on: 2010-12-07
 *      Author: Jakub Bartodziej
 */

#ifndef COMMON_H_
#define COMMON_H_

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

/* ftok() arguments */

#define PATHNAME "./serwer"
#define PROJ_ID_REQUEST 1668299678
#define PROJ_ID_NOTIFY_CLIENT -1101675245
#define PROJ_ID_NOTIFY_SERVER 2095464388
#define PROJ_ID_TERMINATE_QUEUE -1850980645

/* messages */

#define RESOURCE_IDS_SIZE 20

typedef struct {
    long pid;
    int resource_ids[RESOURCE_IDS_SIZE];
} Request;

typedef enum {
    FROM_THREAD, FROM_CLIENT
} NotificationType;

typedef struct {
    long pid;
    NotificationType type;
} Notification;

/* Macros which help with handling message queues. */

#define SEND(QUEUE, MSG)                                                      \
    msgsnd(QUEUE, &MSG, sizeof(MSG) - sizeof(MSG.pid), 0);                    \

#define RECEIVE(QUEUE, MSG, TYPE)                                             \
    msgrcv(QUEUE, &MSG, sizeof(MSG) - sizeof(MSG.pid), TYPE, 0);              \

#endif /* COMMON_H_ */
