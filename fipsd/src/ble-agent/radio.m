/* The CoreBluetooth radio (upstream io_macos/corebluetooth.rs, with the
 * Listener, DialPlan and AdvertThrottle of io_macos/mod.rs); see radio.h.
 * Every method of FBARadio runs on its serial queue `q`: CoreBluetooth's
 * delegate callbacks are delivered there and the session's commands are
 * re-dispatched there, so no callback ever races another. */
#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#include "radio.h"
#include "transport/ble/agent_proto.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define FIPS_SERVICE_UUID @"9C90B790-2CC5-42C0-9F87-C9CC40648F4C" /* backend.h FIPSD_BLE_SERVICE_UUID */
#define PSM_SERVICE_DATA_UUID16 0x9C90                            /* its leading 16 bits (psm.c) */
#define DIAL_TIMEOUT_S 30         /* CoreBluetooth never times out connectPeripheral itself */
#define PEER_SWEEP 256u           /* peers beyond this prompt a sweep of ones gone quiet... */
#define PEER_FORGET_S 600.0       /* ...for this long */
#define ADVERT_INTERVAL_S 1.0     /* an unchanged peer's adverts are passed on once per interval */
#define THROTTLE_SWEEP 256u
#define RSSI_UNAVAILABLE 127      /* CoreBluetooth's "no reading" */
#define PUBLISH_RETRY_S 10        /* after a failed publish, while advertising is wanted */
#define PUBLISH_RECREATE_AFTER 6u /* consecutive failures that replace the peripheral manager */
#define PUBLISH_WARN_EVERY 60u
#define STALE_MAX 32u

/* ---- the L2CAP listener's bookkeeping (upstream Listener) --------------------
 * A publication outlives the peripheral manager reporting powered off: every
 * PSM bluetoothd handed out is kept until it is unpublished, which needs the
 * manager on, or a Mac that sleeps and wakes all day exhausts bluetoothd's
 * listeners ("No resources available for L2CAP"). Answers to a publish name no
 * request and are matched by order, so a power-on after an abandoned publish
 * holds its own until that answer (then stray) or PUBLISH_RETRY_S. */
typedef struct listener {
    int published; /* -1 none since power-on, 0 failed, else the PSM */
    bool publishing, publish_on_power_on, retry_pending, abandoned;
    uint64_t hold, holds; /* hold 0: none */
    uint32_t failures;
    uint16_t stale[STALE_MAX];
    size_t nstale;
} listener;
typedef enum { PUB_UP, PUB_FAILED, PUB_STRAY } pub_kind;
typedef struct pub_outcome { pub_kind kind; uint32_t failures; bool warn, recreate; } pub_outcome;
static void l_stale(listener *l, uint16_t psm) { if (psm && l->nstale < STALE_MAX) l->stale[l->nstale++] = psm; }
static void l_power_off(listener *l)
{
    if (l->published > 0) l_stale(l, (uint16_t)l->published);
    l->published = -1;
    if (l->publishing) { l->publishing = false; l->abandoned = true; }
    l->hold = 0;
}
static void l_manager_replaced(listener *l) { l->publishing = false; l->publish_on_power_on = true; }
static bool l_wants_publish(const listener *l)
{ return !l->publishing && !l->abandoned && (l->published < 0 || l->publish_on_power_on); }
static bool l_may_republish(const listener *l) { return l->published == 0 && !l->publishing; }
static void l_begin(listener *l) { l->publishing = true; l->publish_on_power_on = false; l->abandoned = false; l->hold = 0; }
static pub_outcome l_on_published(listener *l, uint16_t psm, bool ok)
{
    if (!l->publishing) { /* the abandoned publish's answer: its PSM is stale */
        l->abandoned = false; l->hold = 0;
        if (ok) l_stale(l, psm);
        return (pub_outcome){PUB_STRAY, 0, false, false};
    }
    l->publishing = false;
    if (ok && psm) {
        if (l->published > 0) l_stale(l, (uint16_t)l->published);
        l->published = psm;
        uint32_t failures = l->failures; l->failures = 0;
        return (pub_outcome){PUB_UP, failures, false, false};
    }
    l->published = 0; l->failures++;
    return (pub_outcome){PUB_FAILED, l->failures, l->failures == 1 || l->failures % PUBLISH_WARN_EVERY == 0,
        l->failures % PUBLISH_RECREATE_AFTER == 0};
}
static uint64_t l_begin_hold(listener *l) { if (!l->abandoned || l->hold) return 0; return l->hold = ++l->holds; }
static bool l_end_hold(listener *l, uint64_t hold)
{ if (!hold || l->hold != hold) return false; l->abandoned = false; l->hold = 0; return true; }

/* ---- the dial's PSM, one GATT step at a time (upstream DialPlan) ---------------
 * A peer that advertised a PSM is dialled on the requested one at once; one
 * that did not is asked over GATT, and every way that fails ends on the
 * requested PSM, which a legacy UUID-only advertiser listens on. */
typedef enum { STEP_SERVICE, STEP_CHARACTERISTIC, STEP_READ, STEP_OPEN } dial_step;

/* ---- helpers ---------------------------------------------------------------------- */
static int uuid16(NSData *d)
{
    static const uint8_t base_tail[12] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB};
    const uint8_t *b = d.bytes;
    if (d.length == 2) return b[0] << 8 | b[1];
    if (d.length == 16 && !b[0] && !b[1] && !memcmp(b + 4, base_tail, 12)) return b[2] << 8 | b[3];
    return -1;
}
/* The PSM a scanned advert carries in its 0x9C90 service data, or -1. */
static int advertised_psm(NSDictionary<NSString *, id> *adv)
{
    NSDictionary *data = adv[CBAdvertisementDataServiceDataKey];
    if (![data isKindOfClass:[NSDictionary class]]) return -1;
    for (id key in data) {
        if (![key isKindOfClass:[CBUUID class]] || uuid16(((CBUUID *)key).data) != PSM_SERVICE_DATA_UUID16) continue;
        NSData *value = data[key];
        if (![value isKindOfClass:[NSData class]] || value.length < 2) continue;
        const uint8_t *b = value.bytes;
        return b[0] | b[1] << 8;
    }
    return -1;
}
/* The link address for a CoreBluetooth peer: its identifier, hashed (upstream peer_addr). */
static NSData *device_of(CBPeer *peer)
{
    uuid_t id;
    [peer.identifier getUUIDBytes:id];
    uint8_t d[6];
    fipsd_ble_agent_device_from_identifier(id, d);
    return [NSData dataWithBytes:d length:6];
}
static const char *why(NSError *e, const char *otherwise)
{
    const char *text = e ? e.localizedDescription.UTF8String : NULL;
    return text ? text : otherwise;
}
static NSString *device_text(NSData *d)
{
    const uint8_t *b = d.bytes;
    return [NSString stringWithFormat:@"ble0/%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]];
}

@class FBARadio;
@interface FBAPeer : NSObject
@property(nonatomic, strong) CBPeripheral *peripheral;
@property(nonatomic) int psm; /* advertised, or -1 */
@property(nonatomic) NSTimeInterval seen;
@end
@implementation FBAPeer
@end
@interface FBADial : NSObject
@property(nonatomic) int64_t token;
@property(nonatomic) uint16_t requested;
@property(nonatomic) BOOL advertised;
@property(nonatomic, strong) NSData *device;
@property(nonatomic, strong) CBPeripheral *peripheral;
@end
@implementation FBADial
@end
@interface FBASeen : NSObject
@property(nonatomic) NSTimeInterval at;
@property(nonatomic) int psm;
@end
@implementation FBASeen
@end
/* An L2CAP channel handed to the session: its fba_channel. */
@interface FBAChannel : NSObject {
@public
    CBL2CAPChannel *channel; /* held so the channel lives as long as the session's pumps */
    NSInputStream *input;
    NSOutputStream *output;
    NSData *outboundTo; /* the peer, when this side dialled it and so owns the connection */
    __weak FBARadio *radio;
    atomic_bool closed;
}
@end
@implementation FBAChannel
@end

@interface FBARadio : NSObject <CBCentralManagerDelegate, CBPeripheralManagerDelegate, CBPeripheralDelegate>
@end
@implementation FBARadio {
@public
    dispatch_queue_t _q;
    fba_cb_config _cfg;
    fba_agent *_agent;
    CBCentralManager *_central;
    CBPeripheralManager *_manager;
    CBUUID *_service, *_psmChar;
    BOOL _centralOn, _managerOn, _managerPending;
    listener _listener;
    BOOL _wantsAdvertising;
    CBMutableService *_gatt;
    BOOL _up;
    uint16_t _upPsm;
    NSMutableDictionary<NSData *, FBAPeer *> *_peers;
    NSMutableDictionary<NSData *, FBASeen *> *_throttle;
    NSMutableDictionary<NSData *, FBADial *> *_dials;
    NSMutableSet<FBAChannel *> *_channels;
    CBManagerState _centralState, _managerState;
    BOOL _advertising, _advertiseFailed, _scanning;
    size_t _adverts;
    NSMutableSet<NSUUID *> *_heard, *_heardFips;
}

- (void)log:(int)level format:(const char *)fmt, ... __attribute__((format(printf, 2, 3)))
{
    if (!_cfg.log) return;
    char msg[512];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    _cfg.log(_cfg.log_ctx, level, msg);
}

- (instancetype)initWithConfig:(const fba_cb_config *)cfg
{
    if (!(self = [super init])) return nil;
    _cfg = *cfg;
    if (!_cfg.mtu) _cfg.mtu = 2048;
    _q = dispatch_queue_create("com.fips.ble", DISPATCH_QUEUE_SERIAL);
    _listener.published = -1;
    _peers = [NSMutableDictionary dictionary]; _throttle = [NSMutableDictionary dictionary];
    _dials = [NSMutableDictionary dictionary]; _channels = [NSMutableSet set];
    _heard = [NSMutableSet set]; _heardFips = [NSMutableSet set];
    return self;
}

/* ---- power ----------------------------------------------------------------------- */
- (void)startWithAgent:(fba_agent *)agent
{
    _agent = agent;
    _service = [CBUUID UUIDWithString:FIPS_SERVICE_UUID];
    _psmChar = [CBUUID UUIDWithString:CBUUIDL2CAPPSMCharacteristicString]; /* Apple's GATT PSM characteristic */
    /* The managers hold their delegate weakly; this object outlives them. */
    _central = [[CBCentralManager alloc] initWithDelegate:self queue:_q options:nil];
    _manager = [[CBPeripheralManager alloc] initWithDelegate:self queue:_q options:nil];
    [self log:FBA_LOG_DEBUG format:"BLE: CoreBluetooth managers created"];
}
- (void)reportState:(const char *)which state:(CBManagerState)state
{
    switch (state) {
    case CBManagerStatePoweredOn: [self log:FBA_LOG_DEBUG format:"BLE: %s powered on", which]; break;
    case CBManagerStateUnauthorized:
        [self log:FBA_LOG_ERROR format:"BLE: Bluetooth permission denied to fips-ble-agent (%s). Allow it under System Settings > "
            "Privacy & Security > Bluetooth, then restart the agent", which];
        break;
    case CBManagerStatePoweredOff: [self log:FBA_LOG_INFO format:"BLE: Bluetooth is off"]; break;
    case CBManagerStateUnsupported: [self log:FBA_LOG_WARN format:"BLE: Bluetooth LE is unsupported on this Mac"]; break;
    case CBManagerStateResetting: [self log:FBA_LOG_INFO format:"BLE: Bluetooth resetting"]; break;
    default: [self log:FBA_LOG_DEBUG format:"BLE: %s state unknown (%ld)", which, (long)state]; break;
    }
}
/* Re-evaluate whether the radio is up after either manager or the listener changed. */
- (void)powerChanged
{
    if (!_managerOn) {
        /* Power-off clears the GATT database, not the L2CAP publication. */
        l_power_off(&_listener);
        _gatt = nil;
    } else if (!_managerPending) {
        uint64_t hold = l_begin_hold(&_listener);
        if (hold) { [self log:FBA_LOG_DEBUG format:"BLE: holding the publish for an abandoned publish's answer"]; [self endHoldLater:hold]; }
        if (l_wants_publish(&_listener)) [self publish];
    }
    BOOL up = _centralOn && _managerOn && _listener.published >= 0;
    uint16_t psm = _listener.published > 0 ? (uint16_t)_listener.published : 0;
    if (up && !_up) [self log:FBA_LOG_INFO format:"BLE: CoreBluetooth radio up (psm 0x%04x)", psm];
    if (!up && _up) { [self log:FBA_LOG_INFO format:"BLE: CoreBluetooth radio down"]; [self goDown]; }
    if (up != _up || psm != _upPsm) {
        _up = up; _upPsm = psm;
        if (_agent) fba_agent_radio(_agent, up, psm);
        if (up && _cfg.probe) { [self startAdvertising]; [self startScanning]; }
    }
}
/* Fail pending dials, close every channel, forget every peer (their
 * peripheral objects are dead now); the next session says what it wants. */
- (void)goDown
{
    for (FBADial *d in _dials.allValues) [self fail:d];
    [_dials removeAllObjects];
    for (FBAChannel *c in _channels) { [c->input close]; [c->output close]; }
    [_peers removeAllObjects]; [_throttle removeAllObjects];
    _wantsAdvertising = NO; _advertising = NO; _scanning = NO;
}

/* ---- the listener ----------------------------------------------------------------- */
- (void)unpublishStaleOn:(CBPeripheralManager *)m
{
    for (size_t i = 0; i < _listener.nstale; ++i) {
        [self log:FBA_LOG_DEBUG format:"BLE: unpublishing stale L2CAP listener 0x%04x", _listener.stale[i]];
        [m unpublishL2CAPChannel:_listener.stale[i]];
    }
    _listener.nstale = 0;
}
- (void)publish
{
    if (!_manager) return;
    l_begin(&_listener);
    [self unpublishStaleOn:_manager];
    [self log:FBA_LOG_DEBUG format:"BLE: publishing the L2CAP listener"];
    [_manager publishL2CAPChannelWithEncryption:NO];
}
- (void)republish
{
    if (!_managerOn || _managerPending || !l_may_republish(&_listener)) return;
    [self publish];
}
- (void)retryPublishLater
{
    if (_listener.retry_pending) return;
    _listener.retry_pending = true;
    __weak FBARadio *weak = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)PUBLISH_RETRY_S * NSEC_PER_SEC), _q, ^{
        FBARadio *r = weak;
        if (!r) return;
        r->_listener.retry_pending = false;
        if (r->_wantsAdvertising) [r republish];
    });
}
- (void)endHoldLater:(uint64_t)hold
{
    __weak FBARadio *weak = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)PUBLISH_RETRY_S * NSEC_PER_SEC), _q, ^{
        FBARadio *r = weak;
        if (r && l_end_hold(&r->_listener, hold)) {
            [r log:FBA_LOG_DEBUG format:"BLE: no answer to the abandoned publish; publishing"];
            [r powerChanged];
        }
    });
}
/* bluetoothd answers "No resources available for L2CAP" once it holds too
 * many listeners; a new manager starts a new session with it. */
- (void)replaceManager
{
    [self log:FBA_LOG_INFO format:"BLE: replacing the CoreBluetooth peripheral manager"];
    CBPeripheralManager *old = _manager;
    if (old) {
        [old stopAdvertising]; [old removeAllServices];
        [self unpublishStaleOn:old];
        dispatch_async(_q, ^{ (void)old; }); /* released on a later turn, not inside its own callback */
    }
    _gatt = nil; _managerPending = YES; _managerOn = NO;
    l_manager_replaced(&_listener);
    _manager = [[CBPeripheralManager alloc] initWithDelegate:self queue:_q options:nil];
}
/* The PSM characteristic of the FIPS service, a static value CoreBluetooth
 * answers reads of itself (psm.c: 2 bytes little-endian). */
- (void)servePsm:(uint16_t)psm
{
    if (!_manager) return;
    if (_gatt) [_manager removeService:_gatt];
    uint8_t v[2] = {(uint8_t)psm, (uint8_t)(psm >> 8)};
    CBMutableCharacteristic *ch = [[CBMutableCharacteristic alloc] initWithType:_psmChar
        properties:CBCharacteristicPropertyRead value:[NSData dataWithBytes:v length:2] permissions:CBAttributePermissionsReadable];
    CBMutableService *service = [[CBMutableService alloc] initWithType:_service primary:YES];
    service.characteristics = @[ch];
    [_manager addService:service];
    _gatt = service;
}
- (void)published:(uint16_t)psm error:(NSError *)error
{
    pub_outcome o = l_on_published(&_listener, psm, error == nil);
    if (o.kind == PUB_STRAY) {
        [self log:FBA_LOG_DEBUG format:"BLE: answer to an abandoned publish (0x%04x)", psm];
        if (_managerOn) [self unpublishStaleOn:_manager];
        [self powerChanged]; /* a publish held for this answer goes out now */
        return;
    }
    if (o.kind == PUB_FAILED) {
        /* Still up: scanning and dialling work without a listener, and the
         * hello's PSM 0 says so. Advertising waits for one. */
        [self log:o.warn ? FBA_LOG_WARN : FBA_LOG_DEBUG format:"BLE: no L2CAP listener (%s; %u failures)",
            why(error, "unknown"), o.failures];
        if (o.recreate) [self replaceManager];
        if (_wantsAdvertising) [self retryPublishLater];
    } else {
        [self log:o.failures ? FBA_LOG_INFO : FBA_LOG_DEBUG format:"BLE: L2CAP listener published on 0x%04x", psm];
        [self servePsm:psm];
    }
    [self powerChanged];
    if (_listener.published > 0 && _wantsAdvertising && _up) [self startAdvertising];
}

/* ---- commands -------------------------------------------------------------------- */
- (void)startAdvertising
{
    _wantsAdvertising = YES;
    if (_listener.published == 0) { [self republish]; return; } /* no listener to advertise yet */
    if (!_manager || !_managerOn || _manager.isAdvertising) return;
    /* The legacy UUID-only advert: CoreBluetooth carries no service data, so
     * the PSM is served over GATT instead. */
    [_manager startAdvertising:@{CBAdvertisementDataServiceUUIDsKey: @[_service]}];
}
- (void)stopAdvertising
{
    _wantsAdvertising = NO; _advertising = NO;
    if (_manager) [_manager stopAdvertising];
}
- (void)startScanning
{
    if (!_central || !_centralOn) return;
    /* Duplicates allowed, so a peer that restarts on a new PSM is heard;
     * the throttle keeps the rate down. */
    [_central scanForPeripheralsWithServices:_cfg.probe ? nil : @[_service]
        options:@{CBCentralManagerScanOptionAllowDuplicatesKey: @YES}];
    _scanning = YES;
    [self log:FBA_LOG_DEBUG format:"BLE: scanning"];
}
- (void)stopScanning
{
    if (_central) [_central stopScan];
    _scanning = NO;
}

/* ---- scanning --------------------------------------------------------------------- */
- (BOOL)admit:(NSData *)device psm:(int)psm now:(NSTimeInterval)now
{
    FBASeen *last = _throttle[device];
    if (last && last.psm == psm && now - last.at < ADVERT_INTERVAL_S) return NO;
    if (_throttle.count >= THROTTLE_SWEEP) { /* rotating private addresses mint an entry each */
        for (NSData *k in _throttle.allKeys) if (now - _throttle[k].at >= ADVERT_INTERVAL_S * 60) [_throttle removeObjectForKey:k];
    }
    FBASeen *seen = [FBASeen new]; seen.at = now; seen.psm = psm;
    _throttle[device] = seen;
    return YES;
}
- (void)centralManager:(CBCentralManager *)central didDiscoverPeripheral:(CBPeripheral *)peripheral
    advertisementData:(NSDictionary<NSString *, id> *)adv RSSI:(NSNumber *)RSSI
{
    if (central != _central) return;
    NSArray *uuids = adv[CBAdvertisementDataServiceUUIDsKey];
    BOOL fips = [uuids isKindOfClass:[NSArray class]] && [uuids containsObject:_service];
    NSData *device = device_of(peripheral);
    int psm = advertised_psm(adv);
    int16_t rssi = RSSI.shortValue == RSSI_UNAVAILABLE ? INT16_MIN : RSSI.shortValue;
    _adverts++;
    if (_cfg.probe && ![_heard containsObject:peripheral.identifier]) {
        NSString *name = peripheral.name;
        if (!name) name = [adv[CBAdvertisementDataLocalNameKey] isKindOfClass:[NSString class]] ? adv[CBAdvertisementDataLocalNameKey] : @"unnamed";
        [self log:FBA_LOG_INFO format:"BLE: advert from %s (%s) rssi %d%s", device_text(device).UTF8String, name.UTF8String,
            (int)RSSI.shortValue, fips ? ", FIPS service" : ""];
    }
    [_heard addObject:peripheral.identifier];
    if (fips) [_heardFips addObject:peripheral.identifier];
    if (!_up || !fips) return;
    NSTimeInterval now = [NSProcessInfo processInfo].systemUptime;
    if (_peers.count >= PEER_SWEEP)
        for (NSData *k in _peers.allKeys) if (now - _peers[k].seen >= PEER_FORGET_S) [_peers removeObjectForKey:k];
    FBAPeer *peer = [FBAPeer new];
    peer.peripheral = peripheral; peer.psm = psm; peer.seen = now;
    _peers[device] = peer;
    if (![self admit:device psm:psm now:now]) return;
    if (_cfg.log_adverts)
        [self log:FBA_LOG_DEBUG format:"BLE: FIPS advert from %s psm %d rssi %d", device_text(device).UTF8String, psm, (int)rssi];
    if (_agent) fba_agent_advert(_agent, device.bytes, psm > 0 ? (uint16_t)psm : FIPSD_BLE_AGENT_NO_PSM, rssi);
}

/* ---- dialling --------------------------------------------------------------------- */
- (void)fail:(FBADial *)d
{
    if (_agent) fba_agent_dialled(_agent, d.token, d.device.bytes, NULL, 0);
}
- (CBService *)fipsService:(CBPeripheral *)p
{
    for (CBService *s in p.services) if ([s.UUID isEqual:_service]) return s;
    return nil;
}
- (CBCharacteristic *)psmCharacteristic:(CBService *)s
{
    for (CBCharacteristic *c in s.characteristics) if ([c.UUID isEqual:_psmChar]) return c;
    return nil;
}
- (void)step:(FBADial *)d to:(dial_step)step psm:(uint16_t)psm
{
    CBPeripheral *p = d.peripheral;
    switch (step) {
    case STEP_SERVICE: [p discoverServices:@[_service]]; break;
    case STEP_CHARACTERISTIC: {
        CBService *s = [self fipsService:p];
        if (s) [p discoverCharacteristics:@[_psmChar] forService:s]; else [p openL2CAPChannel:d.requested];
        break;
    }
    case STEP_READ: {
        CBCharacteristic *c = [self psmCharacteristic:[self fipsService:p]];
        if (c) [p readValueForCharacteristic:c]; else [p openL2CAPChannel:d.requested];
        break;
    }
    case STEP_OPEN: [p openL2CAPChannel:psm]; break;
    }
}
- (void)connectedStep:(FBADial *)d
{
    if (d.advertised) [self step:d to:STEP_OPEN psm:d.requested]; else [self step:d to:STEP_SERVICE psm:0];
}
- (void)dialToken:(int64_t)token device:(NSData *)device psm:(uint16_t)psm
{
    FBADial *d = [FBADial new];
    d.token = token; d.device = device; d.requested = psm;
    FBAPeer *peer = _up ? _peers[device] : nil;
    if (!peer || !_central) {
        [self log:FBA_LOG_DEBUG format:"BLE: dial to %s, which this Mac has not scanned", device_text(device).UTF8String];
        [self fail:d];
        return;
    }
    d.peripheral = peer.peripheral; d.advertised = peer.psm >= 0;
    FBADial *stale = _dials[device];
    if (stale) [self fail:stale]; /* the daemon re-dials only once it has given up on the last */
    _dials[device] = d;
    [self log:FBA_LOG_DEBUG format:"BLE: dialling %s psm 0x%04x", device_text(device).UTF8String, psm];
    d.peripheral.delegate = self;
    if (d.peripheral.state == CBPeripheralStateConnected) [self connectedStep:d];
    else [_central connectPeripheral:d.peripheral options:nil];
    __weak FBARadio *weak = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)DIAL_TIMEOUT_S * NSEC_PER_SEC), _q, ^{
        FBARadio *r = weak;
        if (r && r->_dials[device].token == token) {
            [r log:FBA_LOG_DEBUG format:"BLE: dial to %s timed out", device_text(device).UTF8String];
            [r abandonDial:device];
        }
    });
}
- (void)abandonDial:(NSData *)device
{
    FBADial *d = _dials[device];
    if (!d) return;
    [_dials removeObjectForKey:device];
    [self releaseIfIdle:device peripheral:d.peripheral];
    [self fail:d];
}
/* Connections are a scarce controller resource, and CoreBluetooth keeps one
 * up for as long as anyone asked for it: drop ours once unused. */
- (void)releaseIfIdle:(NSData *)device peripheral:(CBPeripheral *)p
{
    if (!p || _dials[device]) return;
    for (FBAChannel *c in _channels) if ([c->outboundTo isEqual:device]) return;
    [_central cancelPeripheralConnection:p];
}
- (void)centralManager:(CBCentralManager *)central didConnectPeripheral:(CBPeripheral *)peripheral
{
    if (central != _central) return;
    NSData *device = device_of(peripheral);
    FBADial *d = _dials[device];
    if (d) [self connectedStep:d]; else [self releaseIfIdle:device peripheral:peripheral]; /* the dial was abandoned */
}
- (void)centralManager:(CBCentralManager *)central didFailToConnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error
{
    if (central != _central) return;
    NSData *device = device_of(peripheral);
    FBADial *d = _dials[device];
    if (!d) return;
    [_dials removeObjectForKey:device];
    [self log:FBA_LOG_DEBUG format:"BLE: dial to %s failed: %s", device_text(device).UTF8String, why(error, "?")];
    [self fail:d];
}
- (void)centralManager:(CBCentralManager *)central didDisconnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error
{
    (void)error;
    if (central != _central) return;
    NSData *device = device_of(peripheral);
    FBADial *d = _dials[device];
    if (d) { [_dials removeObjectForKey:device]; [self fail:d]; }
    /* Open channels to it end on their own: their streams end. */
}
- (void)peripheral:(CBPeripheral *)peripheral didDiscoverServices:(NSError *)error
{
    FBADial *d = _dials[device_of(peripheral)];
    if (!d) return;
    BOOL found = !error && [self fipsService:peripheral];
    [self step:d to:found ? STEP_CHARACTERISTIC : STEP_OPEN psm:d.requested];
}
- (void)peripheral:(CBPeripheral *)peripheral didDiscoverCharacteristicsForService:(CBService *)service error:(NSError *)error
{
    FBADial *d = _dials[device_of(peripheral)];
    if (!d) return;
    BOOL found = !error && [self psmCharacteristic:service];
    [self step:d to:found ? STEP_READ : STEP_OPEN psm:d.requested];
}
- (void)peripheral:(CBPeripheral *)peripheral didUpdateValueForCharacteristic:(CBCharacteristic *)characteristic error:(NSError *)error
{
    FBADial *d = _dials[device_of(peripheral)];
    if (!d) return;
    NSData *v = error ? nil : characteristic.value;
    const uint8_t *b = v.bytes;
    uint16_t psm = v.length >= 2 ? (uint16_t)(b[0] | b[1] << 8) : 0; /* psm.c: zero is no PSM */
    [self log:FBA_LOG_DEBUG format:"BLE: read PSM 0x%04x over GATT from %s", psm, device_text(d.device).UTF8String];
    [self step:d to:STEP_OPEN psm:psm ? psm : d.requested];
}

/* ---- channels ---------------------------------------------------------------------- */
/* Open the streams unscheduled, so reads and writes are synchronous and the
 * session's pump threads block in them; no run loop. */
- (FBAChannel *)attach:(CBL2CAPChannel *)channel outboundTo:(NSData *)device
{
    NSInputStream *input = channel.inputStream;
    NSOutputStream *output = channel.outputStream;
    if (!input || !output) { [input close]; [output close]; return nil; }
    [input open]; [output open];
    FBAChannel *c = [FBAChannel new];
    c->channel = channel; c->input = input; c->output = output; c->outboundTo = device; c->radio = self;
    atomic_init(&c->closed, false);
    [_channels addObject:c];
    return c;
}
- (void)peripheral:(CBPeripheral *)peripheral didOpenL2CAPChannel:(CBL2CAPChannel *)channel error:(NSError *)error
{
    NSData *device = device_of(peripheral);
    FBADial *d = _dials[device];
    if (d) [_dials removeObjectForKey:device];
    if (!d || !channel || error) {
        if (d) [self log:FBA_LOG_DEBUG format:"BLE: L2CAP open to %s failed: %s", device_text(device).UTF8String,
            why(error, "no channel")];
        [channel.inputStream close]; [channel.outputStream close]; /* opened after the dial was abandoned: nobody wants it */
        if (d) [self fail:d];
        [self releaseIfIdle:device peripheral:peripheral];
        return;
    }
    FBAChannel *c = [self attach:channel outboundTo:device];
    if (!c || !_agent) {
        if (c) { [_channels removeObject:c]; [c->input close]; [c->output close]; }
        [self fail:d];
        [self releaseIfIdle:device peripheral:peripheral];
        return;
    }
    [self log:FBA_LOG_DEBUG format:"BLE: outbound L2CAP channel to %s open", device_text(device).UTF8String];
    fba_agent_dialled(_agent, d.token, device.bytes, (__bridge_retained fba_channel *)c, _cfg.mtu);
}
- (void)peripheralManager:(CBPeripheralManager *)manager didOpenL2CAPChannel:(CBL2CAPChannel *)channel error:(NSError *)error
{
    (void)manager;
    if (!channel || error) {
        [self log:FBA_LOG_DEBUG format:"BLE: inbound L2CAP channel failed: %s", why(error, "no channel")];
        return;
    }
    CBPeer *peer = channel.peer;
    if (!_up || !_agent || !peer) { [channel.inputStream close]; [channel.outputStream close]; return; }
    NSData *device = device_of(peer);
    FBAChannel *c = [self attach:channel outboundTo:nil];
    if (!c) return;
    [self log:FBA_LOG_DEBUG format:"BLE: inbound L2CAP channel from %s open", device_text(device).UTF8String];
    fba_agent_inbound(_agent, (__bridge_retained fba_channel *)c, device.bytes, _cfg.mtu);
}
- (void)channelReleased:(FBAChannel *)c
{
    [c->input close]; [c->output close];
    [_channels removeObject:c];
    NSData *device = c->outboundTo;
    if (device) [self releaseIfIdle:device peripheral:_peers[device].peripheral];
}

/* ---- manager callbacks -------------------------------------------------------------- */
- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
    if (central != _central) return;
    _centralState = central.state;
    _centralOn = central.state == CBManagerStatePoweredOn;
    if (!_centralOn) _scanning = NO;
    [self reportState:"central" state:central.state];
    [self powerChanged];
}
- (void)peripheralManagerDidUpdateState:(CBPeripheralManager *)manager
{
    if (manager != _manager) return; /* a replaced manager's late callback */
    _managerState = manager.state;
    _managerOn = manager.state == CBManagerStatePoweredOn;
    _managerPending = NO;
    if (!_managerOn) _advertising = NO;
    [self reportState:"peripheral manager" state:manager.state];
    [self powerChanged];
}
- (void)peripheralManager:(CBPeripheralManager *)manager didPublishL2CAPChannel:(CBL2CAPPSM)PSM error:(NSError *)error
{
    if (manager == _manager) [self published:PSM error:error];
}
- (void)peripheralManager:(CBPeripheralManager *)manager didAddService:(CBService *)service error:(NSError *)error
{
    (void)manager; (void)service;
    if (error) [self log:FBA_LOG_WARN format:"BLE: GATT PSM not served: %s", error.localizedDescription.UTF8String];
    else [self log:FBA_LOG_DEBUG format:"BLE: GATT PSM served"];
}
- (void)peripheralManagerDidStartAdvertising:(CBPeripheralManager *)manager error:(NSError *)error
{
    if (manager != _manager) return;
    if (error) { _advertiseFailed = YES; [self log:FBA_LOG_WARN format:"BLE: advertising failed: %s", error.localizedDescription.UTF8String]; }
    else { _advertising = YES; [self log:FBA_LOG_INFO format:"BLE: advertising the FIPS service"]; }
}

/* ---- shutdown ---------------------------------------------------------------------- */
- (void)shutdown
{
    if (_manager) {
        [_manager stopAdvertising]; [_manager removeAllServices];
        if (_managerOn) { l_power_off(&_listener); [self unpublishStaleOn:_manager]; }
    }
    if (_central) [_central stopScan];
    [self goDown];
    _agent = NULL;
    _central.delegate = nil; _manager.delegate = nil;
    _central = nil; _manager = nil; _gatt = nil;
}
@end

/* ---- the C surface ----------------------------------------------------------------------- */
static FBARadio *R(void *radio) { return (__bridge FBARadio *)radio; }
static FBAChannel *C(fba_channel *ch) { return (__bridge FBAChannel *)(void *)ch; }
static void cb_connect(void *radio, int64_t token, const uint8_t device[6], uint16_t psm)
{
    @autoreleasepool {
        FBARadio *r = R(radio);
        NSData *d = [NSData dataWithBytes:device length:6];
        dispatch_async(r->_q, ^{ [r dialToken:token device:d psm:psm]; });
    }
}
/* `psm` is ignored: a Mac cannot advertise it, and serves its own over GATT. */
static void cb_start_advertising(void *radio, uint16_t psm)
{ (void)psm; FBARadio *r = R(radio); dispatch_async(r->_q, ^{ if (r->_up) [r startAdvertising]; }); }
static void cb_stop_advertising(void *radio) { FBARadio *r = R(radio); dispatch_async(r->_q, ^{ [r stopAdvertising]; }); }
static void cb_start_scanning(void *radio) { FBARadio *r = R(radio); dispatch_async(r->_q, ^{ if (r->_up) [r startScanning]; }); }
static void cb_stop_scanning(void *radio) { FBARadio *r = R(radio); dispatch_async(r->_q, ^{ [r stopScanning]; }); }
/* The pumps: blocking stream I/O on the session's threads. A close from the
 * queue ends a read or write blocked here (upstream's hardware-checked
 * assumption about unscheduled CBL2CAPChannel streams). */
static ssize_t cb_read(fba_channel *ch, uint8_t *buf, size_t cap)
{
    @autoreleasepool {
        FBAChannel *c = C(ch);
        if (atomic_load(&c->closed)) return -1;
        return (ssize_t)[c->input read:buf maxLength:cap];
    }
}
static ssize_t cb_write(fba_channel *ch, const uint8_t *buf, size_t len)
{
    @autoreleasepool {
        FBAChannel *c = C(ch);
        if (atomic_load(&c->closed)) return -1;
        return (ssize_t)[c->output write:buf maxLength:len];
    }
}
static void cb_close(fba_channel *ch)
{
    FBAChannel *c = C(ch);
    if (atomic_exchange(&c->closed, true)) return;
    FBARadio *r = c->radio;
    NSInputStream *input = c->input;
    NSOutputStream *output = c->output;
    if (r) dispatch_async(r->_q, ^{ [input close]; [output close]; });
    else { [input close]; [output close]; }
}
static void cb_release(fba_channel *ch)
{
    FBAChannel *c = CFBridgingRelease(ch);
    FBARadio *r = c->radio;
    if (r) dispatch_async(r->_q, ^{ [r channelReleased:c]; });
}
const fba_radio_ops fba_cb_radio_ops = {.connect = cb_connect, .start_advertising = cb_start_advertising,
    .stop_advertising = cb_stop_advertising, .start_scanning = cb_start_scanning, .stop_scanning = cb_stop_scanning,
    .read = cb_read, .write = cb_write, .close = cb_close, .release = cb_release};

fba_cb_radio *fba_cb_radio_create(const fba_cb_config *cfg)
{
    FBARadio *r = [[FBARadio alloc] initWithConfig:cfg];
    return r ? (__bridge_retained fba_cb_radio *)r : NULL;
}
void fba_cb_radio_start(fba_cb_radio *radio, fba_agent *agent)
{
    FBARadio *r = R(radio);
    dispatch_async(r->_q, ^{ [r startWithAgent:agent]; });
}
void fba_cb_radio_destroy(fba_cb_radio *radio)
{
    if (!radio) return;
    FBARadio *r = CFBridgingRelease(radio);
    dispatch_sync(r->_q, ^{ [r shutdown]; });
}
fba_cb_probe fba_cb_radio_probe(fba_cb_radio *radio)
{
    FBARadio *r = R(radio);
    __block fba_cb_probe p = {0};
    dispatch_sync(r->_q, ^{
        p.authorization = fba_cb_authorization();
        p.central_state = (int)r->_centralState; p.manager_state = (int)r->_managerState;
        p.up = r->_up; p.advertising = r->_advertising; p.advertise_failed = r->_advertiseFailed; p.scanning = r->_scanning;
        p.published = r->_listener.published;
        p.adverts = r->_adverts; p.devices = r->_heard.count; p.fips_devices = r->_heardFips.count;
    });
    return p;
}
int fba_cb_authorization(void) { return (int)CBManager.authorization; }
const char *fba_cb_authorization_name(int a)
{
    switch (a) {
    case CBManagerAuthorizationNotDetermined: return "not determined";
    case CBManagerAuthorizationRestricted: return "restricted";
    case CBManagerAuthorizationDenied: return "denied";
    case CBManagerAuthorizationAllowedAlways: return "allowed";
    default: return "unknown";
    }
}
const char *fba_cb_state_name(int s)
{
    switch (s) {
    case CBManagerStatePoweredOn: return "powered on";
    case CBManagerStatePoweredOff: return "powered off";
    case CBManagerStateUnauthorized: return "unauthorized";
    case CBManagerStateUnsupported: return "unsupported";
    case CBManagerStateResetting: return "resetting";
    default: return "unknown";
    }
}
