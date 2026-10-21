package com.phonemic

import android.content.Context
import android.net.ConnectivityManager
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.SocketTimeoutException
import java.security.SecureRandom

/**
 * Finds PhoneMic PCs on the local network: broadcasts a HELLO datagram and
 * collects ANNOUNCE replies (format: shared/include/phonemic/wire.h). Blocking —
 * call it off the main thread.
 */
object Discovery {

    data class Pc(
        val name: String,
        val ip: String,
        val port: Int,
        val pinRequired: Boolean,
        val busy: Boolean,
    )

    private const val MAGIC_LO = 0x50.toByte() // 'P'
    private const val MAGIC_HI = 0x4D.toByte() // 'M'
    private const val VERSION = 2.toByte()
    private const val KIND_HELLO = 1.toByte()
    private const val KIND_ANNOUNCE = 2
    const val DEFAULT_PORT = 4010

    fun find(context: Context, timeoutMs: Int = 1500): List<Pc> {
        val nonce = SecureRandom().nextInt()
        val hello = byteArrayOf(
            MAGIC_LO, MAGIC_HI, VERSION, KIND_HELLO,
            nonce.toByte(), (nonce shr 8).toByte(), (nonce shr 16).toByte(), (nonce shr 24).toByte(),
            0, 0, 0, 0,
        )
        val found = LinkedHashMap<String, Pc>()
        DatagramSocket().use { sock ->
            sock.broadcast = true
            sock.soTimeout = 200
            val targets = broadcastAddresses(context)
            val deadline = System.currentTimeMillis() + timeoutMs
            var nextSend = 0L
            val buf = ByteArray(256)
            while (System.currentTimeMillis() < deadline) {
                // Re-send a few times: broadcasts are often dropped on busy Wi-Fi.
                if (System.currentTimeMillis() >= nextSend) {
                    for (t in targets) {
                        runCatching { sock.send(DatagramPacket(hello, hello.size, t, DEFAULT_PORT)) }
                    }
                    nextSend = System.currentTimeMillis() + 400
                }
                val pkt = DatagramPacket(buf, buf.size)
                try {
                    sock.receive(pkt)
                } catch (_: SocketTimeoutException) {
                    continue
                }
                parseAnnounce(pkt.data, pkt.length, nonce, pkt.address.hostAddress ?: continue)
                    ?.let { found[it.ip] = it }
            }
        }
        return found.values.toList()
    }

    private fun parseAnnounce(b: ByteArray, len: Int, nonce: Int, ip: String): Pc? {
        if (len < 12 || b[0] != MAGIC_LO || b[1] != MAGIC_HI || b[2] != VERSION || b[3].toInt() != KIND_ANNOUNCE) return null
        val n = (b[4].toInt() and 0xFF) or ((b[5].toInt() and 0xFF) shl 8) or
            ((b[6].toInt() and 0xFF) shl 16) or ((b[7].toInt() and 0xFF) shl 24)
        if (n != nonce) return null
        val port = (b[8].toInt() and 0xFF) or ((b[9].toInt() and 0xFF) shl 8)
        val flags = b[10].toInt() and 0xFF
        val nameLen = b[11].toInt() and 0xFF
        if (len != 12 + nameLen) return null
        val name = String(b, 12, nameLen, Charsets.UTF_8).ifBlank { ip }
        return Pc(name, ip, port, pinRequired = flags and 1 != 0, busy = flags and 2 != 0)
    }

    /** 255.255.255.255 plus the directed broadcast of the active network (some routers drop the former). */
    private fun broadcastAddresses(context: Context): List<InetAddress> {
        val out = mutableListOf<InetAddress>(InetAddress.getByName("255.255.255.255"))
        runCatching {
            val cm = context.getSystemService(ConnectivityManager::class.java)
            val lp = cm.getLinkProperties(cm.activeNetwork) ?: return@runCatching
            for (la in lp.linkAddresses) {
                val a = la.address as? Inet4Address ?: continue
                val prefix = la.prefixLength
                if (prefix !in 8..30) continue
                val ip = a.address.fold(0) { acc, x -> (acc shl 8) or (x.toInt() and 0xFF) }
                val bcast = ip or ((1 shl (32 - prefix)) - 1)
                out += InetAddress.getByAddress(
                    byteArrayOf((bcast shr 24).toByte(), (bcast shr 16).toByte(), (bcast shr 8).toByte(), bcast.toByte()),
                )
            }
        }
        return out
    }
}
