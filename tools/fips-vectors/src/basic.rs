use crate::{Exporter, Result, SEEDS, support::*};
use fips_core::{
    entropy::EntropySource,
    identity::*,
    proto::{
        Error,
        codec::{Reader, Writer},
    },
};
use serde_json::{Value, json};

pub fn entropy(e: &Exporter<'_>) -> Result<()> {
    let mut cases = Vec::new();
    for seed in SEEDS {
        let id = format!("{seed:016x}");
        seed_entropy(seed);
        let mut stream = [0u8; 1024];
        SeededEntropy::fill(&mut stream);
        let mut g = SplitMix64(seed);
        let words: Vec<_> = (0..128).map(|_| g.next_u64()).collect();
        assert_eq!(
            stream.to_vec(),
            words
                .iter()
                .flat_map(|w| w.to_le_bytes())
                .collect::<Vec<_>>()
        );
        seed_entropy(seed);
        let mut chunks = Vec::new();
        let mut combined = Vec::new();
        for (i, len) in [1, 7, 8, 9, 17].into_iter().enumerate() {
            let mut bytes = vec![0; len];
            SeededEntropy::fill(&mut bytes);
            chunks.push(json!({"length":len,"bytes":e.bytes(&format!("{id}-chunk-{i}"), &bytes)?}));
            combined.extend(bytes);
        }
        let pair = seeded_keypair(seed);
        cases.push(
            json!({"seed_hex":id, "stream":e.bytes(&format!("{id}-stream"), &stream)?,
            "splitmix64_words":words.iter().map(|v| format!("{v:016x}")).collect::<Vec<_>>(),
            "chunk_lengths":[1,7,8,9,17], "chunks":chunks,
            "chunked_stream":e.bytes(&format!("{id}-chunked"), &combined)?,
            "keypair":{"secret":hex::encode(pair.secret_bytes()),
                "pubkey_compressed":hex::encode(pair.public_key().serialize()),
                "pubkey_xonly":hex::encode(pair.x_only_public_key().0.serialize())}}),
        );
    }
    e.finish(json!(cases), &[])
}

pub fn identity(e: &Exporter<'_>) -> Result<()> {
    let mut cases = Vec::new();
    for (i, seed) in SEEDS.into_iter().enumerate() {
        let id = Identity::<FixedEntropy<0x5a>>::from_keypair(seeded_keypair(seed));
        let challenge = AuthChallenge::<FixedEntropy<0xa5>>::generate();
        let response = id.sign_challenge(challenge.as_bytes(), 1_700_000_000);
        assert_eq!(challenge.verify(&response)?, *id.node_addr());
        let nsec = encode_nsec(&id.keypair().secret_key());
        assert_eq!(decode_npub(&id.npub())?, id.pubkey());
        assert_eq!(decode_nsec(&nsec)?, id.keypair().secret_key());
        let wrong = AuthChallenge::<FixedEntropy<0>>::generate();
        let error = wrong.verify(&response).unwrap_err();
        cases.push(json!({"seed_hex":format!("{seed:016x}"),
            "secret_hex":hex::encode(id.keypair().secret_bytes()),
            "xonly_pubkey":hex::encode(id.pubkey().serialize()), "npub":id.npub(), "nsec":nsec,
            "node_addr":hex::encode(id.node_addr().as_bytes()),
            "ipv6_bytes":hex::encode(id.address().as_bytes()), "ipv6":id.address().to_ipv6().to_string(),
            "auth":{"challenge":e.bytes(&format!("{i}-challenge"), challenge.as_bytes())?,
                "timestamp":response.timestamp, "aux_entropy_byte":0x5a,
                "signature":e.bytes(&format!("{i}-signature"), response.signature.as_ref())?,
                "verified_node_addr":hex::encode(challenge.verify(&response)?.as_bytes()),
                "wrong_challenge_error":format!("{error:?}")}}));
    }
    e.finish(json!(cases), &[])
}

pub fn error(e: Error) -> Value {
    match e {
        Error::MessageTooShort { expected, got } => {
            json!({"variant":"MessageTooShort", "expected":expected, "got":got})
        }
        other => json!({"debug":format!("{other:?}")}),
    }
}
pub fn codec(e: &Exporter<'_>) -> Result<()> {
    let mut cases = Vec::new();
    for (kind, width, value) in [
        ("u8", 1, 0),
        ("u8", 1, 255),
        ("u16_le", 2, 0),
        ("u16_le", 2, 0xabcd),
        ("u16_le", 2, 65535),
        ("u32_le", 4, 0),
        ("u32_le", 4, 0x89abcdef),
        ("u32_le", 4, u32::MAX as u64),
        ("u64_le", 8, 0),
        ("u64_le", 8, 0x0123456789abcdef),
        ("u64_le", 8, u64::MAX),
    ] {
        let mut w = Writer::new();
        match width {
            1 => w.write_u8(value as u8),
            2 => w.write_u16_le(value as u16),
            4 => w.write_u32_le(value as u32),
            8 => w.write_u64_le(value),
            _ => unreachable!(),
        }
        assert_eq!(w.len(), width);
        let wire = w.into_vec();
        let read = |r: &mut Reader<'_>| -> std::result::Result<u64, Error> {
            Ok(match width {
                1 => r.read_u8()? as u64,
                2 => r.read_u16_le()? as u64,
                4 => r.read_u32_le()? as u64,
                8 => r.read_u64_le()?,
                _ => unreachable!(),
            })
        };
        let mut r = Reader::new(&wire);
        assert_eq!(read(&mut r)?, value);
        assert_eq!(r.position(), width);
        assert_eq!(r.remaining(), 0);
        let mut truncations = Vec::new();
        for len in 0..width {
            let err = read(&mut Reader::new(&wire[..len])).unwrap_err();
            assert!(
                matches!(err, Error::MessageTooShort {expected, got} if expected == width && got == len)
            );
            truncations.push(json!({"input_hex":hex::encode(&wire[..len]),"error":error(err)}));
        }
        cases.push(json!({"kind":kind,"value":value,"bytes":e.bytes(&format!("{kind}-{value:x}"), &wire)?,"truncations":truncations}));
    }
    for (name, data) in [
        ("array", vec![0, 0x7f, 0x80, 0xff]),
        ("bytes", vec![1, 2, 3, 4, 5]),
        ("empty", vec![]),
    ] {
        let mut w = Writer::with_capacity(data.len());
        w.write_bytes(&data);
        let wire = w.into_vec();
        let mut r = Reader::new(&wire);
        if name == "array" {
            assert_eq!(r.read_array::<4>()?.as_slice(), data);
        } else {
            assert_eq!(r.read_bytes(data.len())?, data);
        }
        let mut errors = Vec::new();
        for len in 0..data.len() {
            let mut r = Reader::new(&wire[..len]);
            let err = if name == "array" {
                r.read_array::<4>().unwrap_err()
            } else {
                r.read_bytes(data.len()).unwrap_err()
            };
            errors.push(json!({"input_hex":hex::encode(&wire[..len]),"error":error(err)}));
        }
        cases.push(json!({"kind":name,"bytes":e.bytes(name,&wire)?,"truncations":errors}));
    }
    let mut r = Reader::new(&[1, 2, 3, 4]);
    r.advance(1);
    r.require(3)?;
    let err = r.require(4).unwrap_err();
    assert!(matches!(
        err,
        Error::MessageTooShort {
            expected: 5,
            got: 4
        }
    ));
    cases.push(json!({"kind":"cursor", "input_hex":"01020304", "advance":1,
        "position":r.position(),"remaining":r.remaining(),"rest_hex":hex::encode(r.rest()),
        "require_3":"Ok", "require_4":error(err)}));
    e.finish(json!(cases), &[])
}
