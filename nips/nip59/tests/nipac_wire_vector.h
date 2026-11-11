/* Hand-derived NIP-AC wire-shape vector, not a captured Amethyst event.
 * Source: vitorpamplona/quartz @ ce558d90e70d432157cc0672762b2d4119d7f904
 * quartz/src/commonMain/kotlin/com/vitorpamplona/quartz/
 *   nipACWebRtcCalls/WebRtcCallFactory.kt (createCallOffer: sign, then wrap)
 *   nipACWebRtcCalls/events/CallOfferEvent.kt (raw SDP, p/call-id/call-type)
 *   nip59Giftwrap/wraps/EphemeralGiftWrapEvent.kt (21059, now, no seal)
 * https://github.com/vitorpamplona/quartz/tree/ce558d90e70d432157cc0672762b2d4119d7f904/quartz/src/commonMain/kotlin/com/vitorpamplona/quartz/nipACWebRtcCalls
 *
 * Independently derived using Python secp256k1 (BIP-340, zero auxiliary
 * randomness), stdlib SHA-256/HMAC/HKDF and OpenSSL ChaCha20, not libnostr.
 * Test-only secret scalars: sender=1, recipient=2, ephemeral=3.
 * NIP-44 v2 nonce=00..1f; both timestamps=1700000000. The optional alt tag
 * is supplied via CallOfferEvent.build's initializer. The plaintext is the
 * compact JSON below (field order matters for this fixed ciphertext).
 */
#ifndef NIPAC_WIRE_VECTOR_H
#define NIPAC_WIRE_VECTOR_H

static const char NIPAC_RECIPIENT_SK[] =
    "0000000000000000000000000000000000000000000000000000000000000002";
static const char NIPAC_EPHEMERAL_SK[] =
    "0000000000000000000000000000000000000000000000000000000000000003";

static const char NIPAC_INNER_JSON[] =
    "{\"pubkey\":\"79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798\","
    "\"created_at\":1700000000,\"kind\":25050,\"tags\":["
    "[\"p\",\"c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5\"],"
    "[\"call-id\",\"123e4567-e89b-12d3-a456-426614174000\"],"
    "[\"call-type\",\"voice\"],[\"alt\",\"WebRTC call offer\"]],"
    "\"content\":\"v=0\\r\\ns=-\\r\\nt=0 0\\r\\nm=audio 9 UDP/TLS/RTP/SAVPF 111\\r\\n"
    "a=rtpmap:111 opus/48000/2\\r\\n\","
    "\"id\":\"498bc946b0523537bb06d540dd0bd6098204a10adf5508e980649d9887f5a2f6\","
    "\"sig\":\"ac934580c40d200fbcfa76bac3bdf34e2b76c61c4308cb977e9fe7993dfcf139f"
    "e7b0460e2668a89736be0282e70d3616a835a694171825cdc1416be425a9591\"}";

static const char NIPAC_OUTER_JSON[] =
    "{\"pubkey\":\"f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9\","
    "\"created_at\":1700000000,\"kind\":21059,\"tags\":["
    "[\"p\",\"c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5\"]],"
    "\"content\":\"AgABAgMEBQYHCAkKCwwNDg8QERITFBUWFxgZGhscHR4fSQZMmWoxgonQLwhk0xVrnNVclgqBqStefop6zcIATguP8a9kT9wfWjTc4whJRmfRbRNhFafu4sso/FTaqVs557qV+eGakzjLue0Lu7VFgJ+6yfSyyqde3mQ7AGEf+Zh3ww3V54G2peqq0ykbDX27p4oBJTpdDD8azpuA29b3NDBiF0FwXc4JhN6v7nG7zJBdZoFjC609+rnLBhWz+l1m1/mpS4+m5rgrOWQ4YHNz1xOrZTNX/MyNq9Bu8dmVuctUCgddbecwUdYkbeoPS0RlEJamHZzWMaNHcjqfwmdHGPufKq7F+zcoNVVYoao/IEkPuEt66ESKY0h+BHyewQUcV7EPgPxOGMoyINdSndIIXeUHdtmuz0TsEJ+V3YovSD3nK7Eck/a3PUDx/8O/rYSqAHJz6TtY82HorIFzbXWdLDQozHXmdzrS9bcwrEjZs75SKoDI98eiBYgo8f2qb27mryX4gdYvGU//xB4/nralFFtSibzegj0Oaq4KbIwa/iU/ATwcctxVn3o/lo0OhGO6/zIe9P/5mslj9avXfxzTV3OujAD5f1CFgAlQVYNoNzk08JpRIgp/gW89q1hMvaR0hKvK6svlF7p8uxA4KG2yY8kU2DZU1MvEvSE61PVCcCF5g+QtPwd777EDoVDAiVe0k0RtaNgI7opI+VOLPRT2Kg/Cw412I+CbGWO3rMzvbry6MaOSiJk0bm1LG8wKirl8G0a6fT3wjVOWE0AfoX+ht0p57js7NQVtSf80/NDGDVdLhelD0rcVdPr2o9YX6KBkt083vLo8Ibe0LY3WwOtQ0eIJ9+Z2vqsLJnvsOttxvEwnm7XizVDDFnvQqel0oHY4tPEXUeHLC5lGcp85RDLYhSPVqRffQLm48aJFJB3eSLv8ELg=\","
    "\"id\":\"92d9a890c3c8a0032ec56807ef3f660d7aa8c6c50005502b2323c155ec9f2d13\","
    "\"sig\":\"783fc82105792efedb7a41e6f9bb2eb985ebbf0531b0b3f39ef8b896cac98eb06"
    "c5c497c8257b750bc475a7a8ff595623fae8bfae2d0c67aab803b17e0b6f56a\"}";

#endif
