/*
 * Copyright 2014, General Dynamics C4 Systems
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(GD_GPL)
 */

#include <assert.h>

#include <types.h>
#include <kernel/thread.h>
#include <object/structures.h>
#include <object/tcb.h>
#include <object/endpoint.h>
#include <model/statedata.h>
#include <machine/io.h>

#include <object/notification.h>

static inline tcb_queue_t PURE
ntfn_ptr_get_queue(notification_t *ntfnPtr)
{
    tcb_queue_t ntfn_queue;

    ntfn_queue.head = (tcb_t*)notification_ptr_get_ntfnQueue_head(ntfnPtr);
    ntfn_queue.end = (tcb_t*)notification_ptr_get_ntfnQueue_tail(ntfnPtr);

    return ntfn_queue;
}

static inline void
ntfn_ptr_set_queue(notification_t *ntfnPtr, tcb_queue_t ntfn_queue)
{
    notification_ptr_set_ntfnQueue_head(ntfnPtr, (word_t)ntfn_queue.head);
    notification_ptr_set_ntfnQueue_tail(ntfnPtr, (word_t)ntfn_queue.end);
}

static inline void
ntfn_set_active(notification_t *ntfnPtr, word_t badge)
{
    notification_ptr_set_state(ntfnPtr, NtfnState_Active);
    notification_ptr_set_ntfnMsgIdentifier(ntfnPtr, badge);
}


void
sendSignal(notification_t *ntfnPtr, word_t badge)
{
    switch (notification_ptr_get_state(ntfnPtr)) {
    case NtfnState_Idle: {
        tcb_t *tcb = (tcb_t*)notification_ptr_get_ntfnBoundTCB(ntfnPtr);
        /* Check if we are bound and that thread is waiting for a message */
        if (tcb) {
            if (thread_state_ptr_get_tsType(&tcb->tcbState) == ThreadState_BlockedOnReceive) {
                /* Send and start thread running */
                cancelIPC(tcb);
                setThreadState(tcb, ThreadState_Running);
                setRegister(tcb, badgeRegister, badge);
                switchIfRequiredTo(tcb);
            } else {
                ntfn_set_active(ntfnPtr, badge);
            }
        } else {
            ntfn_set_active(ntfnPtr, badge);
        }
        break;
    }
    case NtfnState_Waiting: {
        tcb_queue_t ntfn_queue;
        tcb_t *dest;

        ntfn_queue = ntfn_ptr_get_queue(ntfnPtr);
        dest = ntfn_queue.head;

        /* Haskell error "WaitingNtfn N must have non-empty queue" */
        assert(dest);

        /* Dequeue TCB */
        ntfn_queue = tcbEPDequeue(dest, ntfn_queue);
        ntfn_ptr_set_queue(ntfnPtr, ntfn_queue);

        /* set the thread state to idle if the queue is empty */
        if (!ntfn_queue.head) {
            notification_ptr_set_state(ntfnPtr, NtfnState_Idle);
        }

        setThreadState(dest, ThreadState_Running);
        setRegister(dest, badgeRegister, badge);
        switchIfRequiredTo(dest);
        break;
    }

    case NtfnState_Active: {
        word_t badge2;

        badge2 = notification_ptr_get_ntfnMsgIdentifier(ntfnPtr);
        badge2 |= badge;

        notification_ptr_set_ntfnMsgIdentifier(ntfnPtr, badge2);
        break;
    }
    }
}

void
receiveSignal(tcb_t *thread, cap_t cap, bool_t isBlocking)
{
    notification_t *ntfnPtr;

    ntfnPtr = NTFN_PTR(cap_notification_cap_get_capNtfnPtr(cap));

    switch (notification_ptr_get_state(ntfnPtr)) {
    case NtfnState_Idle:
    case NtfnState_Waiting: {
        tcb_queue_t ntfn_queue;

        if (isBlocking) {
            /* Block thread on notification object */
            thread_state_ptr_set_tsType(&thread->tcbState,
                                        ThreadState_BlockedOnNotification);
            thread_state_ptr_set_blockingObject(&thread->tcbState,
                                                NTFN_REF(ntfnPtr));
            scheduleTCB(thread);

            /* Enqueue TCB */
            ntfn_queue = ntfn_ptr_get_queue(ntfnPtr);
            ntfn_queue = tcbEPAppend(thread, ntfn_queue);

            notification_ptr_set_state(ntfnPtr, NtfnState_Waiting);
            ntfn_ptr_set_queue(ntfnPtr, ntfn_queue);
        } else {
            doNBRecvFailedTransfer(thread);
        }

        break;
    }

    case NtfnState_Active:
        setRegister(
            thread, badgeRegister,
            notification_ptr_get_ntfnMsgIdentifier(ntfnPtr));
        notification_ptr_set_state(ntfnPtr, NtfnState_Idle);
        break;
    }
}

void
cancelAllSignals(notification_t *ntfnPtr)
{
    if (notification_ptr_get_state(ntfnPtr) == NtfnState_Waiting) {
        tcb_t *thread = TCB_PTR(notification_ptr_get_ntfnQueue_head(ntfnPtr));

        notification_ptr_set_state(ntfnPtr, NtfnState_Idle);
        notification_ptr_set_ntfnQueue_head(ntfnPtr, 0);
        notification_ptr_set_ntfnQueue_tail(ntfnPtr, 0);

        /* Set all waiting threads to Restart */
        for (; thread; thread = thread->tcbEPNext) {
            setThreadState(thread, ThreadState_Restart);
            tcbSchedEnqueue(thread);
        }
        rescheduleRequired();
    }
}

void
cancelSignal(tcb_t *threadPtr, notification_t *ntfnPtr)
{
    tcb_queue_t ntfn_queue;

    /* Haskell error "asyncIPCCancel: notification object must be in a waiting" state */
    assert(notification_ptr_get_state(ntfnPtr) == NtfnState_Waiting);

    /* Dequeue TCB */
    ntfn_queue = ntfn_ptr_get_queue(ntfnPtr);
    ntfn_queue = tcbEPDequeue(threadPtr, ntfn_queue);
    ntfn_ptr_set_queue(ntfnPtr, ntfn_queue);

    /* Make notification object idle */
    if (!ntfn_queue.head) {
        notification_ptr_set_state(ntfnPtr, NtfnState_Idle);
    }

    /* Make thread inactive */
    setThreadState(threadPtr, ThreadState_Inactive);
}

void
completeSignal(notification_t *ntfnPtr, tcb_t *tcb)
{
    word_t badge;

    if (likely(tcb && notification_ptr_get_state(ntfnPtr) == NtfnState_Active)) {
        badge = notification_ptr_get_ntfnMsgIdentifier(ntfnPtr);
        setRegister(tcb, badgeRegister, badge);
        notification_ptr_set_state(ntfnPtr, NtfnState_Idle);
    } else {
        fail("tried to complete signal with inactive notification object");
    }
}

static inline void
doUnbindNotification(notification_t *ntfnPtr, tcb_t *tcbptr)
{
    notification_ptr_set_ntfnBoundTCB(ntfnPtr, (word_t) 0);
    tcbptr->tcbBoundNotification = NULL;
}

void
unbindMaybeNotification(notification_t *ntfnPtr)
{
    tcb_t *boundTCB;
    boundTCB = (tcb_t*)notification_ptr_get_ntfnBoundTCB(ntfnPtr);

    if (boundTCB) {
        doUnbindNotification(ntfnPtr, boundTCB);
    }
}

void
unbindNotification(tcb_t *tcb)
{
    notification_t *ntfnPtr;
    ntfnPtr = tcb->tcbBoundNotification;

    if (ntfnPtr) {
        doUnbindNotification(ntfnPtr, tcb);
    }
}

void
bindNotification(tcb_t *tcb, notification_t *ntfnPtr)
{
    notification_ptr_set_ntfnBoundTCB(ntfnPtr, (word_t)tcb);
    tcb->tcbBoundNotification = ntfnPtr;
}

