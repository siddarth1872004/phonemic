//! Transport abstraction.
//!
//! Every way of getting packets from the phone to the PC — Wi-Fi UDP (Phase 1),
//! the `adb forward` USB tunnel (Phase 2), and Bluetooth RFCOMM (Phase 3) — is
//! just a source of framed datagrams. The rest of the core only ever sees the
//! [`Transport`] trait, so swapping transports never touches the jitter buffer,
//! decoder, or sink.

use std::io;
use std::net::{SocketAddr, UdpSocket};

/// A bidirectional, datagram-oriented link to the phone.
///
/// Implementations must preserve message boundaries: one `recv` returns exactly
/// one packet as it was handed to one `send`. UDP and RFCOMM's SEQPACKET-like
/// framing both satisfy this. (The `adb forward` tunnel is TCP/stream-oriented,
/// so its Phase 2 implementation will add length-prefix framing to restore this
/// invariant — noted here so that requirement is not forgotten.)
pub trait Transport {
    /// Send one packet. Returns the number of bytes written.
    /// Unused by the Phase 0 pure receiver; needed for control replies and the
    /// bidirectional RFCOMM transport in later phases.
    #[allow(dead_code)]
    fn send(&self, packet: &[u8]) -> io::Result<usize>;

    /// Block until one packet arrives, copying it into `buf`. Returns the number
    /// of bytes received and the peer address it came from.
    fn recv(&self, buf: &mut [u8]) -> io::Result<(usize, SocketAddr)>;

    /// The local address this transport is bound to (for logging / discovery).
    fn local_addr(&self) -> io::Result<SocketAddr>;
}

/// UDP transport for the same-Wi-Fi case (Phases 0–1).
pub struct UdpTransport {
    socket: UdpSocket,
}

impl UdpTransport {
    /// Bind a UDP socket to `bind_addr` (e.g. `0.0.0.0:4010`) and listen for the
    /// phone's stream.
    pub fn bind(bind_addr: SocketAddr) -> io::Result<Self> {
        let socket = UdpSocket::bind(bind_addr)?;
        Ok(Self { socket })
    }

    /// Fix the peer so [`Transport::send`] can be used without repeating the
    /// address. Optional for a pure receiver, but handy for control replies.
    #[allow(dead_code)]
    pub fn connect(&self, peer: SocketAddr) -> io::Result<()> {
        self.socket.connect(peer)
    }
}

impl Transport for UdpTransport {
    fn send(&self, packet: &[u8]) -> io::Result<usize> {
        self.socket.send(packet)
    }

    fn recv(&self, buf: &mut [u8]) -> io::Result<(usize, SocketAddr)> {
        self.socket.recv_from(buf)
    }

    fn local_addr(&self) -> io::Result<SocketAddr> {
        self.socket.local_addr()
    }
}
