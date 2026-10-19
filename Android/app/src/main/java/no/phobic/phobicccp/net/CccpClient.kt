package no.phobic.phobicccp.net

import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.decodeFromJsonElement
import kotlinx.serialization.json.encodeToJsonElement
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import no.phobic.phobicccp.model.Design
import no.phobic.phobicccp.model.DesignEntry
import no.phobic.phobicccp.model.DesktopInfo
import no.phobic.phobicccp.model.PadJson
import no.phobic.phobicccp.model.SendResult
import no.phobic.phobicccp.model.ToolLibrary
import java.io.BufferedInputStream
import java.io.ByteArrayOutputStream
import java.io.Closeable
import java.io.IOException
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket

/** The link to phobiCCCP, version 1 (src/projectreceiver.h on the desktop). */
object Protocol {
    const val PORT = 47652
    const val DISCOVER = "PHOBICCCP-DISCOVER 1"
    const val API_HELLO = "PHOBICCCP-API 1\n"
}

class CccpException(message: String) : IOException(message)

/**
 * One connection to a phobiCCCP: "PHOBICCCP-API 1", then one JSON request per
 * line, each answered by one JSON line before the next is sent. Blocking; call
 * it off the main thread.
 */
class CccpConnection(
    host: String,
    port: Int = Protocol.PORT,
    connectTimeoutMs: Int = 4000,
    private val readTimeoutMs: Int = 15_000,
) : Closeable {
    private val socket = Socket()
    private val input: BufferedInputStream
    private val output: OutputStream

    init {
        try {
            socket.connect(InetSocketAddress(host, port), connectTimeoutMs)
            socket.soTimeout = readTimeoutMs
            socket.tcpNoDelay = true
            input = BufferedInputStream(socket.getInputStream())
            output = socket.getOutputStream()
            output.write(Protocol.API_HELLO.toByteArray())
        } catch (e: IOException) {
            close()
            throw e
        }
    }

    /**
     * Send one request and wait for its answer. [timeoutMs] is for requests
     * that wait on a person at the computer (send may ask to save first).
     */
    fun request(req: JsonObject, timeoutMs: Int = readTimeoutMs): JsonObject {
        socket.soTimeout = timeoutMs
        output.write((PadJson.encodeToString(JsonObject.serializer(), req) + "\n").toByteArray())
        output.flush()
        val line = readLine() ?: throw CccpException("phobiCCCP closed the connection")
        socket.soTimeout = readTimeoutMs
        val reply = PadJson.parseToJsonElement(line).jsonObject
        if (reply["ok"]?.jsonPrimitive?.boolean != true)
            throw CccpException(reply["error"]?.jsonPrimitive?.contentOrNull ?: "phobiCCCP said no")
        return reply
    }

    private fun readLine(): String? {
        val buf = ByteArrayOutputStream()
        while (true) {
            val b = input.read()
            if (b < 0) return if (buf.size() == 0) null else buf.toString(Charsets.UTF_8.name())
            if (b == '\n'.code) return buf.toString(Charsets.UTF_8.name())
            buf.write(b)
            if (buf.size() > MAX_REPLY) throw CccpException("answer too long")
        }
    }

    fun info(): DesktopInfo = PadJson.decodeFromJsonElement(request(op("info")))

    fun tools(): ToolLibrary = PadJson.decodeFromJsonElement(request(op("tools")))

    fun designs(): List<DesignEntry> =
        request(op("designs"))["designs"]?.jsonArray?.map { PadJson.decodeFromJsonElement<DesignEntry>(it) }
            .orEmpty()

    fun get(id: String): Design =
        PadJson.decodeFromJsonElement(request(op("get") { put("id", id) })["design"]
            ?: throw CccpException("no design in the answer"))

    /** True when phobiCCCP kept it (it had no copy, or an older one). */
    fun put(design: Design): Boolean =
        request(op("put") { put("design", PadJson.encodeToJsonElement(design)) })["stored"]
            ?.jsonPrimitive?.boolean == true

    fun selectMachine(id: String): DesktopInfo =
        PadJson.decodeFromJsonElement(request(op("machine") { put("id", id) }))

    /**
     * Open [design] on that computer, on [machine] (its id; empty for the one
     * in use), and start cutting it when [start] and the computer allows it.
     */
    fun send(design: Design, machine: String, start: Boolean, from: String): SendResult {
        val reply = request(op("send") {
            put("design", PadJson.encodeToJsonElement(design))
            put("machine", machine)
            put("start", start)
            put("from", from)
        }, timeoutMs = SEND_TIMEOUT_MS)
        return PadJson.decodeFromJsonElement(reply)
    }

    override fun close() {
        runCatching { socket.close() }
    }

    companion object {
        const val MAX_REPLY = 32 * 1024 * 1024
        // Someone may have to say whether to save their own work first.
        const val SEND_TIMEOUT_MS = 120_000

        private fun op(name: String, more: kotlinx.serialization.json.JsonObjectBuilder.() -> Unit = {}) =
            buildJsonObject {
                put("op", name)
                more()
            }
    }
}
