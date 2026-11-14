//! Adapters transcribed from MIT-licensed jmcorgan/fips core/src/testutil.rs
//! at PIN. Upstream keeps these private under cfg(test), NOT test-hooks.
//! Copyright (c) 2026 Johnathan Corgan; see ../LICENSE.upstream.
use fips_core::{entropy::EntropySource, identity, time};
use secp256k1::{Keypair, SecretKey};
use std::cell::Cell;

#[derive(Clone, Copy, Debug)]
pub struct SplitMix64(pub u64);
impl SplitMix64 {
    pub fn next_u64(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9e37_79b9_7f4a_7c15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        z ^ (z >> 31)
    }
}
thread_local! {
    static SEEDED: Cell<SplitMix64> = const { Cell::new(SplitMix64(0)) };
}
#[derive(Clone, Copy, Debug)]
pub struct SeededEntropy;
impl EntropySource for SeededEntropy {
    fn fill(dest: &mut [u8]) {
        SEEDED.with(|cell| {
            let mut g = cell.get();
            for chunk in dest.chunks_mut(8) {
                chunk.copy_from_slice(&g.next_u64().to_le_bytes()[..chunk.len()]);
            }
            cell.set(g);
        });
    }
}
pub fn seed_entropy(seed: u64) {
    SEEDED.with(|cell| cell.set(SplitMix64(seed)));
}
#[derive(Clone, Copy, Debug)]
pub struct FixedEntropy<const B: u8>;
impl<const B: u8> EntropySource for FixedEntropy<B> {
    fn fill(dest: &mut [u8]) {
        dest.fill(B);
    }
}
pub fn seeded_keypair(seed: u64) -> Keypair {
    let mut g = SplitMix64(seed ^ 0x6b65_7970_6169_7273);
    let mut secret = [0u8; 32];
    for chunk in secret.chunks_mut(8) {
        chunk.copy_from_slice(&g.next_u64().to_le_bytes());
    }
    let key = SecretKey::from_slice(&secret).expect("fixture secret is valid");
    Keypair::from_secret_key(identity::secp(), &key)
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct TestInstant(pub u64);
impl time::Instant for TestInstant {
    fn saturating_duration_since(self, earlier: Self) -> time::Duration {
        time::Duration::from_millis(self.0.saturating_sub(earlier.0))
    }
    fn checked_add(self, d: time::Duration) -> Option<Self> {
        self.0
            .checked_add(u64::try_from(d.as_millis()).ok()?)
            .map(Self)
    }
    fn checked_sub(self, d: time::Duration) -> Option<Self> {
        self.0
            .checked_sub(u64::try_from(d.as_millis()).ok()?)
            .map(Self)
    }
}
#[derive(Debug, Default)]
pub struct TestClock(Cell<TestInstant>);
impl TestClock {
    pub fn advance(&self, ms: u64) {
        self.0.set(TestInstant(self.0.get().0 + ms));
    }
}
impl time::Clock for TestClock {
    type Instant = TestInstant;
    fn now(&self) -> TestInstant {
        self.0.get()
    }
}
