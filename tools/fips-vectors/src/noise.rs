use crate::{EPOCH, Exporter, Result, support::*};
use fips_core::noise::{HandshakeState, NoiseSession, ReplayWindow};
use serde_json::{Value, json};

pub struct Transcript {
    pub messages: Vec<Vec<u8>>,
    pub initiator: NoiseSession,
    pub responder: NoiseSession,
    pub steps: Vec<Value>,
}
fn state(i: &HandshakeState<SeededEntropy>, r: &HandshakeState<SeededEntropy>) -> Value {
    json!({"initiator":{"progress":format!("{:?}",i.progress()),"h":hex::encode(i.handshake_hash())},
        "responder":{"progress":format!("{:?}",r.progress()),"h":hex::encode(r.handshake_hash())}})
}
pub fn handshake(xk: bool) -> Result<Transcript> {
    seed_entropy(if xk { 0x584b } else { 0x494b });
    let a = seeded_keypair(0);
    let b = seeded_keypair(1);
    let (mut i, mut r) = if xk {
        (
            HandshakeState::<SeededEntropy>::new_xk_initiator(a, b.public_key()),
            HandshakeState::new_xk_responder(b),
        )
    } else {
        (
            HandshakeState::<SeededEntropy>::new_initiator(a, b.public_key()),
            HandshakeState::new_responder(b),
        )
    };
    i.set_local_epoch(EPOCH);
    r.set_local_epoch(EPOCH);
    let mut steps = vec![json!({"event":"constructed","state":state(&i,&r)})];
    let mut messages = Vec::new();
    let m1 = if xk {
        i.write_xk_message_1()?
    } else {
        i.write_message_1()?
    };
    steps.push(json!({"event":"initiator_write_1","state":state(&i,&r)}));
    if xk {
        r.read_xk_message_1(&m1)?
    } else {
        r.read_message_1(&m1)?
    };
    steps.push(json!({"event":"responder_read_1","state":state(&i,&r)}));
    messages.push(m1);
    let m2 = if xk {
        r.write_xk_message_2()?
    } else {
        r.write_message_2()?
    };
    steps.push(json!({"event":"responder_write_2","state":state(&i,&r)}));
    i = if xk {
        i.fork_xk2(&m2)?
    } else {
        i.fork_msg2(&m2)?
    };
    steps.push(json!({"event":"initiator_read_2_on_fork","state":state(&i,&r)}));
    messages.push(m2);
    if xk {
        let m3 = i.write_xk_message_3()?;
        steps.push(json!({"event":"initiator_write_3","state":state(&i,&r)}));
        r = r.fork_xk3(&m3)?;
        steps.push(json!({"event":"responder_read_3_on_fork","state":state(&i,&r)}));
        messages.push(m3);
    }
    assert!(i.is_complete() && r.is_complete());
    assert_eq!(i.handshake_hash(), r.handshake_hash());
    assert_eq!(i.remote_epoch(), Some(EPOCH));
    assert_eq!(r.remote_epoch(), Some(EPOCH));
    Ok(Transcript {
        messages,
        initiator: i.into_session()?,
        responder: r.into_session()?,
        steps,
    })
}

pub fn export(e: &Exporter<'_>) -> Result<()> {
    let mut patterns = Vec::new();
    for xk in [false, true] {
        let name = if xk { "xk" } else { "ik" };
        let mut t = handshake(xk)?;
        let messages: Vec<Value> = t
            .messages
            .iter()
            .enumerate()
            .map(|(n, m)| {
                Ok(
                    json!({"number":n+1, "sender":if n==1 {"responder"} else {"initiator"},
                "bytes":e.bytes(&format!("{name}-msg-{}",n+1),m)?}),
                )
            })
            .collect::<Result<_>>()?;
        let mut frames = Vec::new();
        for direction in 0..2 {
            let (send, recv) = if direction == 0 {
                (&mut t.initiator, &mut t.responder)
            } else {
                (&mut t.responder, &mut t.initiator)
            };
            for n in 0..8 {
                let plaintext = format!("fips-vectors/{name}/{direction}/{n}").into_bytes();
                let counter = send.current_send_counter();
                let aad = if n % 2 == 0 {
                    Vec::new()
                } else {
                    vec![direction, n, 0xa5]
                };
                let ciphertext = send.encrypt_with_aad(&plaintext, &aad)?;
                // Corrupt only the ciphertext, keeping the correct unused
                // counter/AAD. Failure must not consume the replay slot.
                let mut corrupt = ciphertext.clone();
                corrupt[0] ^= 1;
                let bad = recv
                    .decrypt_with_replay_check_and_aad(&corrupt, counter, &aad)
                    .unwrap_err();
                assert!(matches!(
                    bad,
                    fips_core::noise::NoiseError::DecryptionFailed
                ));
                recv.check_replay(counter)?;
                assert_eq!(
                    recv.decrypt_with_replay_check_and_aad(&ciphertext, counter, &aad)?,
                    plaintext
                );
                let replay_error = recv
                    .decrypt_with_replay_check_and_aad(&ciphertext, counter, &aad)
                    .unwrap_err();
                assert!(
                    matches!(replay_error,fips_core::noise::NoiseError::ReplayDetected(c) if c==counter)
                );
                frames.push(json!({"direction":if direction==0 {"initiator_to_responder"} else {"responder_to_initiator"},
                    "counter":counter,"plaintext_hex":hex::encode(&plaintext),"aad_hex":hex::encode(&aad),
                    "sealed":e.bytes(&format!("{name}-frame-{direction}-{n}"),&ciphertext)?,
                    "opened_hex":hex::encode(&plaintext),"replay_error":format!("{replay_error:?}"),
                    "corrupt_input_hex":hex::encode(corrupt),"corrupt_counter":counter,
                    "authentication_error":format!("{bad:?}"),"failed_counter_still_acceptable":true}));
            }
        }
        patterns.push(json!({"pattern":name.to_uppercase(),
            "entropy_seed_hex":if xk {"000000000000584b"} else {"000000000000494b"},
            "keypair_seeds":[0,1],"startup_epoch_hex":hex::encode(EPOCH),"messages":messages,
            "steps":t.steps,"post_handshake":{"initiator_h":hex::encode(t.initiator.handshake_hash()),
                "responder_h":hex::encode(t.responder.handshake_hash()),"ck":null},"frames":frames}));
    }
    let mut w = ReplayWindow::new();
    let mut replay = Vec::new();
    for counter in [
        0, 0, 2, 1, 2, 8191, 8192, 0, 1, 8192, 16384, 8192, 8193, 16383, 16384, 20000,
    ] {
        let accept = w.check(counter);
        if accept {
            w.accept(counter)
        }
        replay.push(json!({"counter":counter,"accept":accept,"highest_after":w.highest()}));
    }
    e.finish(json!({"patterns":patterns,"replay_window_size":fips_core::noise::REPLAY_WINDOW_SIZE,"replay_trace":replay}),
        &["SymmetricState.ck is private; no public or test-hook getter at this pin. h is exported via HandshakeState::handshake_hash (test-hooks)."])
}
