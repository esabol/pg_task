#include "include.h"

int WaitLatchOrSocketMy(Latch *latch, WaitEventMy *event, int wakeEvents, queue_t *event_queue, long timeout, uint32 wait_event_info) {
    int ret = 0, count = queue_count(event_queue);
    WaitEventSet *set = CreateWaitEventSet(CurrentMemoryContext, 2 + count);
    if (count > 0) {
        wakeEvents |= WL_SOCKET_MASK;
//        wait_event_info = PG_WAIT_CLIENT;
    }
    if (wakeEvents & WL_TIMEOUT) Assert(timeout >= 0); else timeout = -1;
    if (wakeEvents & WL_LATCH_SET) AddWaitEventToSet(set, WL_LATCH_SET, PGINVALID_SOCKET, latch, NULL);
    Assert(!IsUnderPostmaster || (wakeEvents & WL_EXIT_ON_PM_DEATH) || (wakeEvents & WL_POSTMASTER_DEATH));
    if ((wakeEvents & WL_POSTMASTER_DEATH) && IsUnderPostmaster) AddWaitEventToSet(set, WL_POSTMASTER_DEATH, PGINVALID_SOCKET, NULL, NULL);
    if ((wakeEvents & WL_EXIT_ON_PM_DEATH) && IsUnderPostmaster) AddWaitEventToSet(set, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET, NULL, NULL);
    if (wakeEvents & WL_SOCKET_MASK) {
        queue_each(event_queue, queue) {
            WaitEventMy *event = pointer_data(queue, WaitEventMy, pointer);
            AddWaitEventToSet(set, wakeEvents & event->base.event.events, event->base.event.fd, NULL, event->base.event.user_data);
        }
    }
    if (!WaitEventSetWait(set, timeout, &event->base.event, 1, wait_event_info)) ret |= WL_TIMEOUT; else {
        ret |= event->base.event.events & (WL_LATCH_SET | WL_POSTMASTER_DEATH | WL_SOCKET_MASK);
        if (ret & WL_SOCKET_MASK) pointer_remove(&event->pointer);
        if (ret & WL_LATCH_SET) L("WL_LATCH_SET");
        if (ret & WL_SOCKET_READABLE) L("WL_SOCKET_READABLE");
        if (ret & WL_SOCKET_WRITEABLE) L("WL_SOCKET_WRITEABLE");
        if (ret & WL_TIMEOUT) L("WL_TIMEOUT");
        if (ret & WL_POSTMASTER_DEATH) L("WL_POSTMASTER_DEATH");
        if (ret & WL_EXIT_ON_PM_DEATH) L("WL_EXIT_ON_PM_DEATH");
        if (ret & WL_SOCKET_CONNECTED) L("WL_SOCKET_CONNECTED");
    }
    FreeWaitEventSet(set);
    return ret;
}
