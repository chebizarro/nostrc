//! W24 slice I (nostrc-qp24.5.2): MDK v0.11.0's own verdicts on component
//! states, for libmarmot's differential tests. Input lines: "<id hex> <state hex>".
use cgka_traits::agent_text_stream::AgentTextStreamQuicPolicyV1;
use cgka_traits::app_components::{decode_encrypted_media_policy_v2, encode_encrypted_media_policy_v2,
    EncryptedMediaPolicyV2, BlobStoreEndpointV2, validate_and_normalize_blob_endpoint_url_v2};
use std::io::BufRead;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() > 1 && args[1] == "normalize" {
        // Endpoint URL normalization verdicts: one raw URL per line (hex).
        for line in std::io::stdin().lock().lines() {
            let line = line.unwrap();
            let raw = String::from_utf8(hex::decode(line.trim()).unwrap()).unwrap();
            let v = match validate_and_normalize_blob_endpoint_url_v2(&raw) {
                Ok(n) => serde_json::json!({"in": raw, "ok": true, "normalized": n}),
                Err(e) => serde_json::json!({"in": raw, "ok": false, "error": e}),
            };
            println!("{v}");
        }
        return;
    }
    if args.len() > 1 && args[1] == "encode-default" {
        let p = EncryptedMediaPolicyV2 {
            media_format: "encrypted-media-v2".into(),
            allowed_locator_kinds: vec!["blossom-v1".into()],
            default_blob_endpoints: args[2..].iter().map(|u| BlobStoreEndpointV2 {
                locator_kind: "blossom-v1".into(), base_url: u.clone() }).collect(),
        };
        match encode_encrypted_media_policy_v2(&p) {
            Ok(b) => println!("{}", hex::encode(b)),
            Err(e) => println!("ERR {e}"),
        }
        return;
    }
    for line in std::io::stdin().lock().lines() {
        let line = line.unwrap();
        let mut it = line.split_whitespace();
        let id = u16::from_str_radix(it.next().unwrap(), 16).unwrap();
        let bytes = hex::decode(it.next().unwrap_or("")).unwrap();
        let verdict = match id {
            0x8006 => AgentTextStreamQuicPolicyV1::decode_component_state(&bytes)
                .map(|p| serde_json::json!({"required": p.required_member_roles, "allowed": p.allowed_member_roles}))
                .map_err(|e| e.to_string()),
            0x800b => decode_encrypted_media_policy_v2(&bytes)
                .map(|p| serde_json::to_value(p).unwrap())
                .map_err(|e| e.to_string()),
            _ => Err("unknown id".into()),
        };
        let out = match verdict {
            Ok(v) => serde_json::json!({"id": format!("{id:04x}"), "hex": hex::encode(&bytes), "ok": true, "value": v}),
            Err(e) => serde_json::json!({"id": format!("{id:04x}"), "hex": hex::encode(&bytes), "ok": false, "error": e}),
        };
        println!("{out}");
    }
}
