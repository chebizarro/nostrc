use porthome_sys as ph;
use std::{env, fs, slice};
use vault_sys as vault;
use zeroize::Zeroizing;

fn check(status: i32) -> Result<(), String> {
    if status == 0 {
        Ok(())
    } else {
        Err(format!("C library error: {status}"))
    }
}

struct Ring(vault::vault_ring);
impl Drop for Ring {
    fn drop(&mut self) {
        // SAFETY: initialized once below; the ring owns its C allocation.
        unsafe { vault::vault_ring_dispose(&mut self.0) }
    }
}

#[derive(Default)]
struct Buffer {
    ptr: *mut u8,
    len: usize,
}
impl Buffer {
    fn bytes(&self) -> &[u8] {
        if self.len == 0 {
            &[]
        } else {
            // SAFETY: successful C calls return len readable malloc bytes.
            unsafe { slice::from_raw_parts(self.ptr, self.len) }
        }
    }
}
impl Drop for Buffer {
    fn drop(&mut self) {
        // SAFETY: C outputs are malloc-owned (or NULL), never Rust allocations.
        unsafe {
            if self.len != 0 {
                zeroize::Zeroize::zeroize(slice::from_raw_parts_mut(self.ptr, self.len));
            }
            libc::free(self.ptr.cast());
        }
    }
}

fn run(args: &[String]) -> Result<(), String> {
    let sealing = args.get(1).map(String::as_str) == Some("seal");
    let opening = args.get(1).map(String::as_str) == Some("unseal");
    if !(sealing && args.len() == 5 || opening && args.len() == 7) {
        return Err("usage: vault-cli seal KEY_FILE INPUT OUTPUT\n       vault-cli unseal KEY_FILE INPUT OUTPUT SIZE_PLAIN SHA256\nKEY_FILE is exactly 32 raw node-key bytes; example uses epoch 1.".into());
    }
    let key = Zeroizing::new(fs::read(&args[2]).map_err(|e| e.to_string())?);
    if key.len() != 32 {
        return Err("KEY_FILE must contain exactly 32 raw bytes".into());
    }
    let input = Zeroizing::new(fs::read(&args[3]).map_err(|e| e.to_string())?);
    // SAFETY: C requires a fresh zeroed destination. Drop disposes its key copy.
    let mut ring = Ring(unsafe { std::mem::zeroed() });
    unsafe { check(vault::vault_ring_init(&mut ring.0, 1, key.as_ptr()))? };
    let mut output = Buffer::default();
    let mut address = [0u8; 32];
    // SAFETY: input/key live through the calls; output pointers are fresh and
    // owned by Buffer. vault_ring_view borrows ring, which outlives decryption.
    unsafe {
        if sealing {
            check(ph::porthome_encrypt_chunk_v2(
                key.as_ptr(),
                input.as_ptr(),
                input.len(),
                &mut output.ptr,
                &mut output.len,
                address.as_mut_ptr(),
            ))?;
        } else {
            let size_plain: usize = args[5].parse().map_err(|_| "invalid SIZE_PLAIN")?;
            let expected =
                std::ffi::CString::new(args[6].as_str()).map_err(|_| "invalid SHA256")?;
            let mut expected_hash = [0u8; 32];
            if args[6].len() != 64 {
                return Err("SHA256 must be 64 hex characters".into());
            }
            check(ph::porthome_from_hex64(
                expected.as_ptr(),
                expected_hash.as_mut_ptr(),
            ))?;
            check(ph::porthome_sha256(
                input.as_ptr(),
                input.len(),
                address.as_mut_ptr(),
            ))?;
            if address != expected_hash {
                return Err("sealed object SHA256 mismatch".into());
            }
            let view = vault::vault_ring_view(&ring.0);
            check(ph::porthome_decrypt_chunk_v2(
                &view,
                1,
                input.as_ptr(),
                input.len(),
                size_plain,
                &mut output.ptr,
                &mut output.len,
            ))?;
        }
    }
    fs::write(&args[4], output.bytes()).map_err(|e| e.to_string())?;
    if sealing {
        let hex: String = address.iter().map(|b| format!("{b:02x}")).collect();
        println!("{hex} {}", input.len());
    }
    Ok(())
}

fn main() {
    if let Err(error) = run(&env::args().collect::<Vec<_>>()) {
        eprintln!("{error}");
        std::process::exit(1);
    }
}
