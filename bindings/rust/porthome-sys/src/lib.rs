//! Raw, unsafe bindings generated from installed libporthome headers.
//! C ownership, lifetimes and error contracts apply; this is not a safe wrapper.
#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]
include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
