package no.phobic.phobicccp.net

import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import no.phobic.phobicccp.model.Desktop
import no.phobic.phobicccp.model.PadJson
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.NetworkInterface
import java.net.SocketTimeoutException

/**
 * Finding phobiCCCP: a "PHOBICCCP-DISCOVER 1" datagram to port 47652 on every
 * broadcast address of this phone (and straight to any host typed in), and
 * every answer collected for a moment. Blocking; call it off the main thread.
 */
object Discovery {

    fun probe(manualHosts: Collection<String> = emptyList(), waitMs: Int = 1200): List<Desktop> {
        val found = LinkedHashMap<String, Desktop>()
        DatagramSocket().use { sock ->
            sock.broadcast = true
            sock.soTimeout = 200
            val hello = Protocol.DISCOVER.toByteArray()
            val manualAddresses = manualHosts.mapNotNull { h -> resolve(h)?.let { it to h } }.toMap()
            val targets = broadcastAddresses() + manualAddresses.keys
            for (t in targets)
                runCatching { sock.send(DatagramPacket(hello, hello.size, t, Protocol.PORT)) }
            val buf = ByteArray(4096)
            val until = System.currentTimeMillis() + waitMs
            while (System.currentTimeMillis() < until) {
                val p = DatagramPacket(buf, buf.size)
                try {
                    sock.receive(p)
                } catch (_: SocketTimeoutException) {
                    continue
                }
                val typed = manualAddresses[p.address].orEmpty()
                parseAnswer(String(p.data, 0, p.length), p.address.hostAddress ?: continue, typed)
                    ?.let { found[it.id] = it }
            }
        }
        return found.values.toList()
    }

    /** One discovery answer, or null when it is not phobiCCCP's. */
    fun parseAnswer(text: String, address: String, typed: String = "", now: Long = System.currentTimeMillis()): Desktop? {
        val o = runCatching { PadJson.parseToJsonElement(text).jsonObject }.getOrNull() ?: return null
        fun str(k: String) = o[k]?.jsonPrimitive?.contentOrNull.orEmpty()
        if (str("app") != "phobiCCCP") return null
        val host = str("host").ifEmpty { address }
        return Desktop(
            id = str("id").ifEmpty { "$host@$address" },
            host = host,
            address = address,
            port = o["port"]?.jsonPrimitive?.intOrNull ?: Protocol.PORT,
            version = str("version"),
            machine = str("machine"),
            lastSeen = now,
            typed = typed,
        )
    }

    private fun resolve(host: String): InetAddress? = runCatching { InetAddress.getByName(host.trim()) }.getOrNull()

    private fun broadcastAddresses(): List<InetAddress> {
        val out = mutableListOf<InetAddress>()
        runCatching {
            for (nif in NetworkInterface.getNetworkInterfaces()) {
                if (!nif.isUp || nif.isLoopback) continue
                for (a in nif.interfaceAddresses)
                    if (a.address is Inet4Address) a.broadcast?.let { out += it }
            }
        }
        runCatching { out += InetAddress.getByName("255.255.255.255") }
        return out.distinct()
    }
}
