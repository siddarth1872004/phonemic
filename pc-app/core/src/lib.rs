//! PhoneMic PC-side core library.
//!
//! Split into a library (this crate) plus a thin `phonemic-receiver` binary so
//! the tricky, bug-prone pieces — the jitter buffer especially — are ordinary
//! unit-tested library code rather than buried in `main`.

#[cfg(feature = "denoise")]
pub mod denoise;
pub mod jitter_buffer;
pub mod sink;
pub mod transport;
