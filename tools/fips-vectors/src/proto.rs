use crate::{Exporter, Result, basic::error, noise::handshake, support::*};
use fips_core::utils::index::SessionIndex;
use fips_core::{
    identity::*,
    proto::{
        bloom::*, coord::*, fmp::wire as fmp, fsp::wire as fsp, link::*, lookup::*, mmp::*,
        path::wire::*, stp::*,
    },
};
use serde_json::json;

pub fn export(e: &Exporter<'_>) -> Result<()> {
    let a = Identity::<FixedEntropy<0x5a>>::from_keypair(seeded_keypair(0));
    let b = Identity::<FixedEntropy<0x5a>>::from_keypair(seeded_keypair(1));
    let src = TreeCoordinate::root_with_meta(*a.node_addr(), 0x0123456789abcdef, 1_700_000_000);
    let dst = TreeCoordinate::from_addrs(vec![*b.node_addr(), *a.node_addr()])?;
    let mut cases = Vec::new();
    macro_rules! roundtrip {
        ($name:expr, $ty:ty, $obj:expr, $bytes:expr, $offset:expr, $input:expr) => {{
            let obj: $ty = $obj;
            let bytes: Vec<u8> = $bytes(&obj);
            let decoded = <$ty>::decode(&bytes[$offset..])?;
            assert_eq!($bytes(&decoded), bytes, "{} round trip", $name);
            let short = <$ty>::decode(&[]).unwrap_err();
            cases.push(json!({"name":$name,"input":$input,"bytes":e.bytes($name,&bytes)?,
                "decode_offset":$offset,"decoded_debug":format!("{decoded:?}"),
                "empty_input_error":error(short)}));
        }};
    }
    let mut coords = Vec::new();
    encode_coords(&dst, &mut coords);
    let (decoded, used) = decode_coords(&coords)?;
    assert_eq!(used, coords.len());
    assert_eq!(decoded, dst);
    cases.push(json!({"name":"coords","input":{"addresses_hex":dst.node_addrs().map(|a| hex::encode(a.as_bytes())).collect::<Vec<_>>()},
        "bytes":e.bytes("coords",&coords)?,"consumed":used,"decoded_depth":decoded.depth()}));
    let mut empty = Vec::new();
    encode_empty_coords(&mut empty);
    assert_eq!(decode_optional_coords(&empty)?, (None, 2));
    cases.push(
        json!({"name":"coords-empty","bytes":e.bytes("coords-empty",&empty)?,
        "required_error":error(decode_coords(&empty).unwrap_err()),"optional":null}),
    );
    let mut filter = BloomFilter::new();
    filter.insert(a.node_addr());
    filter.insert(b.node_addr());
    assert!(filter.contains(a.node_addr()) && filter.contains(b.node_addr()));
    cases.push(json!({"name":"bloom","inserted_node_addrs":[hex::encode(a.node_addr().as_bytes()),hex::encode(b.node_addr().as_bytes())],
        "hash_count":filter.hash_count(),"num_bits":filter.num_bits(),"ones":filter.count_ones(),
        "bytes":e.bytes("bloom",filter.as_bytes())?}));
    roundtrip!(
        "filter-announce",
        FilterAnnounce,
        FilterAnnounce::new(filter, 0x0123456789abcdef),
        |v: &FilterAnnounce| v.encode().unwrap(),
        1,
        json!({"sequence":0x0123456789abcdef_u64,"filter_case":"bloom"})
    );
    let mut declaration =
        ParentDeclaration::self_root(*a.node_addr(), 0x0123456789abcdef, 1_700_000_000);
    let signing = declaration.signing_bytes();
    let sig = a.sign(&signing);
    declaration.set_signature(*sig.as_ref());
    cases.push(json!({"name":"parent-declaration","node_addr":hex::encode(a.node_addr().as_bytes()),
        "parent_addr":hex::encode(a.node_addr().as_bytes()),"sequence":declaration.sequence(),"timestamp":declaration.timestamp(),
        "signing_bytes":e.bytes("parent-declaration-signing",&signing)?,
        "signature_hex":hex::encode(sig.as_ref()),"aux_entropy_byte":0x5a}));
    let announce = TreeAnnounce::new(declaration, src.clone());
    announce.validate_semantics()?;
    roundtrip!(
        "tree-announce",
        TreeAnnounce,
        announce,
        |v: &TreeAnnounce| v.encode().unwrap(),
        1,
        json!({"declaration_case":"parent-declaration","ancestry_node_addrs":[hex::encode(a.node_addr().as_bytes())]})
    );
    roundtrip!(
        "lookup-request",
        LookupRequest,
        LookupRequest::new(
            0x0123456789abcdef,
            *b.node_addr(),
            *a.node_addr(),
            src.clone(),
            64,
            1280
        ),
        |v: &LookupRequest| v.encode(),
        1,
        json!({"request_id":0x0123456789abcdef_u64,"target_seed":1,"origin_seed":0,"origin_coord":"src","ttl":64,"min_mtu":1280})
    );
    let signing = LookupResponse::proof_bytes(0x0123456789abcdef, b.node_addr(), &dst);
    let proof = b.sign(&signing);
    roundtrip!(
        "lookup-response",
        LookupResponse,
        LookupResponse::new(0x0123456789abcdef, *b.node_addr(), dst.clone(), proof),
        |v: &LookupResponse| v.encode(),
        1,
        json!({"request_id":0x0123456789abcdef_u64,"target_seed":1,"target_coord":"dst",
            "proof_hex":hex::encode(proof.as_ref()),"proof_signing_hex":hex::encode(signing),"aux_entropy_byte":0x5a})
    );
    let sender = SenderReport {
        interval_start_counter: 0x0123456789abcdef,
        interval_end_counter: 0x0123456789abcdff,
        interval_start_timestamp: 0x89abcdef,
        interval_end_timestamp: 0xffffffff,
        interval_bytes_sent: 0x10203040,
        cumulative_packets_sent: 0x1122334455667788,
        cumulative_bytes_sent: u64::MAX,
    };
    let sender_input = json!({"interval_start_counter":sender.interval_start_counter,"interval_end_counter":sender.interval_end_counter,
        "interval_start_timestamp":sender.interval_start_timestamp,"interval_end_timestamp":sender.interval_end_timestamp,
        "interval_bytes_sent":sender.interval_bytes_sent,"cumulative_packets_sent":sender.cumulative_packets_sent,"cumulative_bytes_sent":sender.cumulative_bytes_sent});
    let ss = SessionSenderReport {
        interval_start_counter: sender.interval_start_counter,
        interval_end_counter: sender.interval_end_counter,
        interval_start_timestamp: sender.interval_start_timestamp,
        interval_end_timestamp: sender.interval_end_timestamp,
        interval_bytes_sent: sender.interval_bytes_sent,
        cumulative_packets_sent: sender.cumulative_packets_sent,
        cumulative_bytes_sent: sender.cumulative_bytes_sent,
    };
    roundtrip!(
        "sender-report",
        SenderReport,
        sender,
        |v: &SenderReport| v.encode(),
        1,
        sender_input
    );
    roundtrip!(
        "session-sender-report",
        SessionSenderReport,
        ss,
        |v: &SessionSenderReport| v.encode(),
        0,
        sender_input
    );
    let receiver = ReceiverReport {
        highest_counter: 0x0123456789abcdef,
        cumulative_packets_recv: u64::MAX,
        cumulative_bytes_recv: 0x1122334455667788,
        timestamp_echo: 0x89abcdef,
        dwell_time: 0xabcd,
        max_burst_loss: 65535,
        mean_burst_loss: 7,
        jitter: 0x12345678,
        ecn_ce_count: 5,
        owd_trend: -12345678,
        burst_loss_count: 3,
        cumulative_reorder_count: 2,
        interval_packets_recv: 99,
        interval_bytes_recv: 12345,
    };
    let receiver_input = json!({"highest_counter":receiver.highest_counter,"cumulative_packets_recv":receiver.cumulative_packets_recv,
        "cumulative_bytes_recv":receiver.cumulative_bytes_recv,"timestamp_echo":receiver.timestamp_echo,"dwell_time":receiver.dwell_time,
        "max_burst_loss":receiver.max_burst_loss,"mean_burst_loss":receiver.mean_burst_loss,"jitter":receiver.jitter,"ecn_ce_count":receiver.ecn_ce_count,
        "owd_trend":receiver.owd_trend,"burst_loss_count":receiver.burst_loss_count,"cumulative_reorder_count":receiver.cumulative_reorder_count,
        "interval_packets_recv":receiver.interval_packets_recv,"interval_bytes_recv":receiver.interval_bytes_recv});
    let sr = SessionReceiverReport {
        highest_counter: receiver.highest_counter,
        cumulative_packets_recv: receiver.cumulative_packets_recv,
        cumulative_bytes_recv: receiver.cumulative_bytes_recv,
        timestamp_echo: receiver.timestamp_echo,
        dwell_time: receiver.dwell_time,
        max_burst_loss: receiver.max_burst_loss,
        mean_burst_loss: receiver.mean_burst_loss,
        jitter: receiver.jitter,
        ecn_ce_count: receiver.ecn_ce_count,
        owd_trend: receiver.owd_trend,
        burst_loss_count: receiver.burst_loss_count,
        cumulative_reorder_count: receiver.cumulative_reorder_count,
        interval_packets_recv: receiver.interval_packets_recv,
        interval_bytes_recv: receiver.interval_bytes_recv,
    };
    roundtrip!(
        "receiver-report",
        ReceiverReport,
        receiver,
        |v: &ReceiverReport| v.encode(),
        1,
        receiver_input
    );
    roundtrip!(
        "session-receiver-report",
        SessionReceiverReport,
        sr,
        |v: &SessionReceiverReport| v.encode(),
        0,
        receiver_input
    );
    roundtrip!(
        "path-mtu",
        PathMtuNotification,
        PathMtuNotification::new(1280),
        |v: &PathMtuNotification| v.encode(),
        0,
        json!({"path_mtu":1280})
    );
    roundtrip!(
        "path-probe",
        PathProbe,
        PathProbe::new(
            0x89abcdef,
            0x12345678,
            PathFlags::with_active(true),
            PathCaps::with_aggregate(true)
        )
        .with_len(64),
        |v: &PathProbe| v.encode(),
        1,
        json!({"probe_id":0x89abcdef_u32,"path_id":0x12345678,"active":true,"aggregate":true,"len":64})
    );
    roundtrip!(
        "path-ack",
        PathAck,
        PathAck {
            probe_id: 0x89abcdef,
            flags: PathFlags::with_active(false),
            path_id: 0x11223344,
            echo_path_id: 0x12345678,
            caps: PathCaps::with_aggregate(true),
            rx_time_us: 0x12345678,
            rx_frames: 0x11223344,
            rx_bytes: 0x55667788,
            len: 64
        },
        |v: &PathAck| v.encode(),
        1,
        json!({"probe_id":0x89abcdef_u32,"active":false,"path_id":0x11223344,"echo_path_id":0x12345678,"aggregate":true,
            "rx_time_us":0x12345678,"rx_frames":0x11223344,"rx_bytes":0x55667788,"len":64})
    );
    roundtrip!(
        "path-close",
        PathClose,
        PathClose {
            path_id: 0x11223344,
            reason: PathCloseReason::InterfaceGone
        },
        |v: &PathClose| v.encode().to_vec(),
        1,
        json!({"path_id":0x11223344,"reason":"InterfaceGone"})
    );
    roundtrip!(
        "disconnect",
        fmp::Disconnect,
        fmp::Disconnect::new(fmp::DisconnectReason::Restart),
        |v: &fmp::Disconnect| v.encode().to_vec(),
        1,
        json!({"reason":"Restart"})
    );
    let ik = handshake(false)?;
    let xk = handshake(true)?;
    for (n, m) in ik.messages.iter().enumerate() {
        let wire = if n == 0 {
            fmp::build_msg1(SessionIndex::new(0x11223344), m)
        } else {
            fmp::build_msg2(
                SessionIndex::new(0x55667788),
                SessionIndex::new(0x11223344),
                m,
            )
        };
        if n == 0 {
            assert_eq!(
                &wire[fmp::Msg1Header::parse(&wire).unwrap().noise_msg1_offset..],
                m
            )
        } else {
            assert_eq!(
                &wire[fmp::Msg2Header::parse(&wire).unwrap().noise_msg2_offset..],
                m
            )
        }
        let name = format!("fmp-msg-{}", n + 1);
        cases.push(json!({"name":name,"noise_case":format!("ik-msg-{}",n+1),"sender_idx":if n==0 {0x11223344} else {0x55667788},
            "receiver_idx":if n==0 {None} else {Some(0x11223344)},"bytes":e.bytes(&name,&wire)?}));
    }
    roundtrip!(
        "fsp-setup",
        fsp::SessionSetup,
        fsp::SessionSetup::new(src.clone(), dst.clone()).with_handshake(xk.messages[0].clone()),
        |v: &fsp::SessionSetup| v.encode(),
        4,
        json!({"src_coord":"src","dest_coord":"dst","noise_case":"xk-msg-1"})
    );
    roundtrip!(
        "fsp-ack",
        fsp::SessionAck,
        fsp::SessionAck::new(dst.clone(), src.clone()).with_handshake(xk.messages[1].clone()),
        |v: &fsp::SessionAck| v.encode(),
        4,
        json!({"src_coord":"dst","dest_coord":"src","noise_case":"xk-msg-2"})
    );
    roundtrip!(
        "fsp-msg3",
        fsp::SessionMsg3,
        fsp::SessionMsg3::new(xk.messages[2].clone()),
        |v: &fsp::SessionMsg3| v.encode(),
        4,
        json!({"noise_case":"xk-msg-3"})
    );
    let mut ik_i = ik.initiator;
    let mut ik_r = ik.responder;
    let mut xk_i = xk.initiator;
    let mut xk_r = xk.responder;
    let inner = fsp::fsp_prepend_inner_header(
        0x12345678,
        fsp::SessionMessageType::DataPacket.to_byte(),
        fsp::FSP_INNER_FLAG_SP,
        &[0, 1, 2, 3, 4],
    );
    assert_eq!(
        fsp::fsp_strip_inner_header(&inner).unwrap().3,
        &[0, 1, 2, 3, 4]
    );
    let header = fsp::build_fsp_header(0, fsp::FSP_FLAG_K, inner.len() as u16);
    let sealed = xk_i.encrypt_with_aad(&inner, &header)?;
    assert_eq!(
        xk_r.decrypt_with_replay_check_and_aad(&sealed, 0, &header)?,
        inner
    );
    let wire = fsp::build_fsp_encrypted(&header, &sealed);
    let parsed = fsp::FspEncryptedHeader::parse(&wire).unwrap();
    fsp::check_header(&parsed)?;
    fsp::check_sealed(&parsed, sealed.len())?;
    cases.push(json!({"name":"fsp-encrypted","counter":0,"flags":fsp::FSP_FLAG_K,"inner_hex":hex::encode(&inner),
        "header_hex":hex::encode(header),"bytes":e.bytes("fsp-encrypted",&wire)?}));
    roundtrip!(
        "session-datagram",
        SessionDatagram,
        SessionDatagram::new(*a.node_addr(), *b.node_addr(), wire)
            .with_ttl(64)
            .with_path_mtu(1280),
        |v: &SessionDatagram| v.encode(),
        1,
        json!({"src_seed":0,"dest_seed":1,"ttl":64,"path_mtu":1280,"payload_case":"fsp-encrypted"})
    );
    let datagram = cases.last().unwrap()["bytes"]["hex"].as_str().unwrap();
    let datagram = hex::decode(datagram)?;
    let inner = fmp::prepend_inner_header(0x89abcdef, &datagram);
    assert_eq!(fmp::strip_inner_header(&inner).unwrap().1, datagram);
    let header = fmp::build_established_header(
        SessionIndex::new(0x55667788),
        0,
        fmp::FLAG_KEY_EPOCH,
        inner.len() as u16,
    );
    let sealed = ik_i.encrypt_with_aad(&inner, &header)?;
    assert_eq!(
        ik_r.decrypt_with_replay_check_and_aad(&sealed, 0, &header)?,
        inner
    );
    let wire = fmp::build_encrypted(&header, &sealed);
    let parsed = fmp::EncryptedHeader::parse(&wire).unwrap();
    assert_eq!(&wire[parsed.ciphertext_offset()..], sealed);
    cases.push(json!({"name":"fmp-encrypted","receiver_idx":0x55667788,"counter":0,"flags":fmp::FLAG_KEY_EPOCH,
        "inner_hex":hex::encode(inner),"header_hex":hex::encode(header),"bytes":e.bytes("fmp-encrypted",&wire)?}));
    let catalog: Vec<_> = (0..=255)
        .filter_map(|b| {
            LinkMessageType::from_byte(b).map(|t| {
                assert_eq!(t.to_byte(), b);
                json!({"name":format!("{t:?}"),"value":b})
            })
        })
        .collect();
    let session_catalog: Vec<_> = (0..=255)
        .filter_map(|b| {
            fsp::SessionMessageType::from_byte(b)
                .map(|t| json!({"name":format!("{t:?}"),"value":t.to_byte()}))
        })
        .collect();
    e.finish(json!({"common_inputs":{"identity_seeds":[0,1],"src_coord_debug":format!("{src:?}"),"dst_coord_debug":format!("{dst:?}")},
        "wire_cases":cases,"link_catalog":catalog,"session_catalog":session_catalog,
        "constants":{"FIPS_OVERHEAD":fips_core::proto::framing::FIPS_OVERHEAD,"SESSION_DATAGRAM_HEADER_SIZE":SESSION_DATAGRAM_HEADER_SIZE,
            "FMP_FRAME_OVERHEAD":fmp::FRAME_OVERHEAD,"FMP_ESTABLISHED_HEADER_SIZE":fmp::ESTABLISHED_HEADER_SIZE,
            "FMP_MSG1_WIRE_SIZE":fmp::MSG1_WIRE_SIZE,"FMP_MSG2_WIRE_SIZE":fmp::MSG2_WIRE_SIZE,
            "FSP_HEADER_SIZE":fsp::FSP_HEADER_SIZE,"FSP_INNER_HEADER_SIZE":fsp::FSP_INNER_HEADER_SIZE,"FSP_PORT_HEADER_SIZE":fsp::FSP_PORT_HEADER_SIZE,
            "payload_limit_1280":fips_core::proto::framing::payload_limit(1280)}}),
        &["Representative wire cases only: not an exhaustive malformed-message, routing, probe, settlement or state-machine sweep.",
          "FIPS_IPV6_OVERHEAD lives in the host IPv6 module, not fips-core; core FIPS_OVERHEAD is 106 (110 including port header)."])
}
