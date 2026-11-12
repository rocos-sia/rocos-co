#pragma once
#include "301/CO_driver.h"
#ifdef __cplusplus
extern "C" {
#endif

/** Event-loop owned transport boundary. try_send returns NO, TX_BUSY, or SYSCALL. */
typedef struct {
    void *context;
    CO_ReturnError_t (*try_send)(void *context, const CO_CANtx_t *tx);
} RocosCanPort;

/** Dispatches one already validated frame against CANopenNode's registered RX filters. */
bool_t rocos_can_dispatch(CO_CANmodule_t *module, RocosCanRxMessage *message);
/** Cancel software-pending traffic before a new phase or group stop. */
void rocos_can_discard(CO_CANmodule_t *module);
#ifdef __cplusplus
}
#endif
