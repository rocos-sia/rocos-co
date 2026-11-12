#include "driver.h"
#include <string.h>

void CO_CANsetConfigurationMode(void *port) { (void)port; }
void CO_CANsetNormalMode(CO_CANmodule_t *m) {
    if (m)
        m->CANnormal = true;
}

CO_ReturnError_t CO_CANmodule_init(CO_CANmodule_t *m, void *port, CO_CANrx_t rx[], uint16_t nr, CO_CANtx_t tx[],
                                   uint16_t nt, uint16_t bitrate) {
    (void)bitrate;
    if (!m || !port || !rx || !tx || !((RocosCanPort *)port)->try_send)
        return CO_ERROR_ILLEGAL_ARGUMENT;
    memset(m, 0, sizeof(*m));
    memset(rx, 0, nr * sizeof(*rx));
    memset(tx, 0, nt * sizeof(*tx));
    m->CANptr = port;
    m->rxArray = rx;
    m->rxSize = nr;
    m->txArray = tx;
    m->txSize = nt;
    m->firstCANtxMessage = true;
    return CO_ERROR_NO;
}
void rocos_can_discard(CO_CANmodule_t *m) {
    for (uint16_t i = 0; i < m->txSize; ++i)
        m->txArray[i].bufferFull = false;
    m->CANtxCount = 0;
}
void CO_CANmodule_disable(CO_CANmodule_t *m) {
    if (m) {
        m->CANnormal = false;
        rocos_can_discard(m);
    }
}
CO_ReturnError_t CO_CANrxBufferInit(CO_CANmodule_t *m, uint16_t i, uint16_t id, uint16_t mask, bool_t rtr,
                                    void *object, void (*callback)(void *, void *)) {
    if (!m || i >= m->rxSize || !object || !callback || id > 0x7ff || mask > 0x7ff)
        return CO_ERROR_ILLEGAL_ARGUMENT;
    m->rxArray[i] = (CO_CANrx_t){id | (rtr ? 0x800 : 0), mask | 0x800, object, callback};
    return CO_ERROR_NO;
}
bool_t rocos_can_dispatch(CO_CANmodule_t *m, RocosCanRxMessage *message) {
    if (!m || !m->CANnormal || !message || message->dlc > 8)
        return false;
    for (uint16_t i = 0; i < m->rxSize; ++i) {
        CO_CANrx_t *rx = &m->rxArray[i];
        if (rx->CANrx_callback && ((message->ident ^ rx->ident) & rx->mask) == 0) {
            rx->CANrx_callback(rx->object, message);
            return true;
        }
    }
    return false;
}
CO_CANtx_t *CO_CANtxBufferInit(CO_CANmodule_t *m, uint16_t i, uint16_t id, bool_t rtr, uint8_t size,
                               bool_t sync) {
    if (!m || i >= m->txSize || id > 0x7ff || size > 8)
        return NULL;
    CO_CANtx_t *tx = &m->txArray[i];
    if (tx->bufferFull)
        m->CANtxCount--;
    memset(tx, 0, sizeof(*tx));
    tx->ident = id | (rtr ? 0x800 : 0);
    tx->DLC = size;
    tx->syncFlag = sync;
    return tx;
}
static CO_ReturnError_t transmit(CO_CANmodule_t *m, CO_CANtx_t *tx) {
    RocosCanPort *p = m->CANptr;
    CO_ReturnError_t r = p->try_send(p->context, tx);
    if (r == CO_ERROR_NO)
        m->firstCANtxMessage = false;
    else if (r != CO_ERROR_TX_BUSY)
        m->CANerrorStatus |= CO_CAN_ERRTX_OVERFLOW;
    return r;
}
CO_ReturnError_t CO_CANsend(CO_CANmodule_t *m, CO_CANtx_t *tx) {
    if (!m || !tx || !m->CANnormal)
        return CO_ERROR_TX_UNCONFIGURED;
    if (tx->bufferFull) {
        m->CANerrorStatus |= CO_CAN_ERRTX_OVERFLOW;
        return CO_ERROR_TX_OVERFLOW;
    }
    CO_ReturnError_t r = m->CANtxCount ? CO_ERROR_TX_BUSY : transmit(m, tx);
    if (r == CO_ERROR_TX_BUSY) {
        tx->bufferFull = true;
        ++m->CANtxCount;
        return CO_ERROR_NO;
    }
    return r;
}
void CO_CANclearPendingSyncPDOs(CO_CANmodule_t *m) {
    for (uint16_t i = 0; i < m->txSize; ++i) {
        CO_CANtx_t *tx = &m->txArray[i];
        if (tx->bufferFull && tx->syncFlag) {
            tx->bufferFull = false;
            --m->CANtxCount;
            m->CANerrorStatus |= CO_CAN_ERRTX_PDO_LATE;
        }
    }
}
void CO_CANmodule_process(CO_CANmodule_t *m) {
    if (!m->CANnormal)
        return;
    for (uint16_t i = 0; m->CANtxCount && i < m->txSize; ++i) {
        CO_CANtx_t *tx = &m->txArray[i];
        if (!tx->bufferFull)
            continue;
        if (transmit(m, tx) == CO_ERROR_TX_BUSY)
            break;
        tx->bufferFull = false;
        --m->CANtxCount;
    }
}
