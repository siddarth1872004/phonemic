//! `--driver` output path: instead of playing to the speakers, write received
//! PCM into a named shared section that the PhoneMic ACX driver maps and drains.
//! Completes the browser → gateway → ring → driver → Windows-mic chain.
//!
//! The gateway *creates* the global section (needs admin, as does loading the
//! driver); the driver opens it by the well-known name. Layout is
//! `phonemic_ipc::Ring`, mirroring the driver's `ring.h`.
//!
//! STATUS: compiles, but is runtime-verifiable only with the signed driver
//! loaded — there is no driver in the dev environment.

use std::error::Error;
use std::mem::size_of;

use phonemic_ipc::{Producer, Ring};
use windows::core::PCWSTR;
use windows::Win32::Foundation::{CloseHandle, HANDLE, INVALID_HANDLE_VALUE};
use windows::Win32::System::Memory::{
    CreateFileMappingW, MapViewOfFile, UnmapViewOfFile, FILE_MAP_ALL_ACCESS,
    MEMORY_MAPPED_VIEW_ADDRESS, PAGE_READWRITE,
};

/// Must match `PHONEMIC_RING_SECTION_NAME` in the driver (`\BaseNamedObjects\…`
/// == the Win32 `Global\` namespace).
const SECTION_NAME: &str = "Global\\PhonemicRing";

pub struct DriverFeed {
    producer: Producer,
    view: MEMORY_MAPPED_VIEW_ADDRESS,
    mapping: HANDLE,
}

// The mapping/view handles are owned solely here and the producer is the single
// writer, so moving this to the playout task is sound.
unsafe impl Send for DriverFeed {}

impl DriverFeed {
    /// Create + map the shared section and initialise its ring header.
    pub fn create() -> Result<Self, Box<dyn Error>> {
        let size = size_of::<Ring>();
        let name: Vec<u16> = SECTION_NAME.encode_utf16().chain(std::iter::once(0)).collect();

        unsafe {
            let mapping = CreateFileMappingW(
                INVALID_HANDLE_VALUE,
                None,
                PAGE_READWRITE,
                (size >> 32) as u32,
                (size & 0xFFFF_FFFF) as u32,
                PCWSTR(name.as_ptr()),
            )?;

            let view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
            if view.Value.is_null() {
                let _ = CloseHandle(mapping);
                return Err("MapViewOfFile failed".into());
            }

            let ring = view.Value as *mut Ring;
            (*ring).init_header();
            let producer = Producer::new(ring);

            Ok(DriverFeed { producer, view, mapping })
        }
    }

    /// Write samples into the ring; returns how many were accepted.
    pub fn feed(&mut self, samples: &[i16]) -> usize {
        self.producer.push(samples)
    }
}

impl Drop for DriverFeed {
    fn drop(&mut self) {
        unsafe {
            let _ = UnmapViewOfFile(self.view);
            let _ = CloseHandle(self.mapping);
        }
    }
}
