#ifndef FIPS_BLE_AGENT_RADIO_H
#define FIPS_BLE_AGENT_RADIO_H
/* The CoreBluetooth radio (upstream io_macos/corebluetooth.rs and the
 * platform-neutral parts of io_macos/mod.rs): the only Objective-C in the
 * agent. A CBCentralManager (scan, dial) and a CBPeripheralManager (L2CAP
 * listener, advertising, the GATT PSM characteristic) deliver their delegate
 * callbacks on one private serial dispatch queue, and every CoreBluetooth
 * call is made there, so all radio state is the queue's. Results reach the
 * session only through its non-blocking fba_agent_* reports; nothing runs on
 * a run loop. A channel's bytes are pumped by the session's threads doing
 * blocking I/O on the CBL2CAPChannel's streams, opened unscheduled.
 *
 * The radio is up once both managers report powered on and the listener's
 * publish has completed (its OS-assigned PSM, or 0 if it failed: scanning and
 * dialling still work). A power-off withdraws everything; the PSMs bluetoothd
 * handed out are given back once the manager is on again (upstream Listener). */
#include "session.h"

typedef struct fba_cb_radio fba_cb_radio;
typedef struct fba_cb_config {
    uint16_t mtu;      /* reported for every channel, which CoreBluetooth hides: 0 = 2048 (transports.ble.mtu) */
    bool probe;        /* diagnostics: once up, advertise and scan for every advertiser by itself */
    bool log_adverts;  /* log each admitted advert at debug level */
    void (*log)(void *ctx, int level, const char *message);
    void *log_ctx;
} fba_cb_config;
fba_cb_radio *fba_cb_radio_create(const fba_cb_config *);
extern const fba_radio_ops fba_cb_radio_ops; /* `radio` is the fba_cb_radio */
/* Creates the managers and starts reporting to `agent` (NULL: probe mode). */
void fba_cb_radio_start(fba_cb_radio *, fba_agent *agent);
/* Stops advertising and scanning, closes every channel and unpublishes every
 * L2CAP PSM (bluetoothd keeps them otherwise), then frees. */
void fba_cb_radio_destroy(fba_cb_radio *);

typedef struct fba_cb_probe {
    int authorization;         /* CBManagerAuthorization: 0 not determined, 1 restricted, 2 denied, 3 allowed */
    int central_state, manager_state; /* CBManagerState */
    bool up, advertising, advertise_failed, scanning;
    int published;             /* -1 none yet, 0 failed, else the L2CAP PSM */
    size_t adverts, devices, fips_devices;
} fba_cb_probe;
fba_cb_probe fba_cb_radio_probe(fba_cb_radio *);
int fba_cb_authorization(void);
const char *fba_cb_authorization_name(int);
const char *fba_cb_state_name(int);
#endif
