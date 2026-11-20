// Reference for the LIBM_COUNT pins in bloom_vectors.py.
// Run in a scratch Cargo project with libm = "=0.2.16", default features
// (arch) enabled, matching upstream core/Cargo.toml and Cargo.lock.
// Emits m, k, population, binary64 estimate bits; no IO other than stdout.
fn main() {
    for (m,k) in [(64,3),(1024,5),(1024,7),(2048,7),(4096,5),(8192,1),(8192,4),(8192,5),(8192,6),(8192,7),(8192,255),(16384,7)] {
        for x in 0..m {
            let n = -(m as f64 / k as f64)*libm::log(1.0-x as f64/m as f64);
            println!("{m} {k} {x} {:016x}", n.to_bits());
        }
    }
}
