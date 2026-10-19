package no.phobic.phobicccp

import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import no.phobic.phobicccp.model.Design
import no.phobic.phobicccp.model.PadJson
import no.phobic.phobicccp.net.CccpConnection
import no.phobic.phobicccp.net.CccpException
import no.phobic.phobicccp.net.Discovery
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.net.ServerSocket
import kotlin.concurrent.thread

/** The phone's side of the link against a stand-in for phobiCCCP on loopback. */
class ClientTest {

    private fun fakeDesktop(answer: (JsonObject) -> String): Pair<ServerSocket, MutableList<JsonObject>> {
        val server = ServerSocket(0)
        val seen = mutableListOf<JsonObject>()
        thread(isDaemon = true) {
            server.accept().use { s ->
                val r = s.getInputStream().bufferedReader()
                val w = s.getOutputStream()
                if (r.readLine() != "PHOBICCCP-API 1") return@thread
                while (true) {
                    val line = r.readLine() ?: break
                    val req = PadJson.parseToJsonElement(line).jsonObject
                    synchronized(seen) { seen += req }
                    w.write((answer(req) + "\n").toByteArray())
                    w.flush()
                }
            }
        }
        return server to seen
    }

    @Test
    fun requestsAndAnswers() {
        val (server, seen) = fakeDesktop { req ->
            when (req["op"]!!.jsonPrimitive.content) {
                "info" -> """{"ok":true,"host":"shop","id":"i1","activeMachine":"m1","allowStart":true,
                    "machines":[{"id":"m1","name":"Shapeoko","active":true},{"id":"m2","name":"Router"}],
                    "status":{"connected":true,"state":"Idle","homed":true,"busy":false}}""".replace("\n", "")
                "designs" -> """{"ok":true,"designs":[{"id":"a","name":"A","modified":5,"deleted":false}]}"""
                "put" -> """{"ok":true,"stored":true}"""
                "send" -> """{"ok":true,"started":true,"message":"opened A.c2d on shop, cutting started","notes":[]}"""
                else -> """{"ok":false,"error":"unknown request"}"""
            }
        }
        server.use {
            CccpConnection("127.0.0.1", server.localPort).use { c ->
                val info = c.info()
                assertEquals("shop", info.host)
                assertEquals(2, info.machines.size)
                assertTrue(info.status.readyToCut())
                assertEquals(5L, c.designs().single().modified)
                assertTrue(c.put(Design(id = "a", name = "A", modified = 6)))
                val r = c.send(Design(id = "a", name = "A", modified = 6), "m2", start = true, from = "Pixel")
                assertTrue(r.started)
                val err = runCatching { c.tools() }.exceptionOrNull()
                assertTrue(err is CccpException && err.message == "unknown request")
            }
        }
        val send = synchronized(seen) { seen.first { it["op"]!!.jsonPrimitive.content == "send" } }
        assertEquals("m2", send["machine"]!!.jsonPrimitive.content)
        assertTrue(send["start"]!!.jsonPrimitive.boolean)
        assertEquals("a", send["design"]!!.jsonObject["id"]!!.jsonPrimitive.content)
    }

    @Test
    fun discoveryAnswers() {
        val d = Discovery.parseAnswer("""{"app":"phobiCCCP","host":"shop","port":47652,"id":"i1","machine":"Shapeoko","version":"1.1.21"}""",
            "192.168.1.5", now = 1000)!!
        assertEquals("i1", d.id)
        assertEquals("Shapeoko", d.machine)
        assertTrue(d.online(2000))
        assertFalse(d.online(1000 + 60_000))
        // An older phobiCCCP without an id is still found, keyed by name and address.
        assertEquals("pc@10.0.0.2", Discovery.parseAnswer("""{"app":"phobiCCCP","host":"pc","port":47652}""", "10.0.0.2")!!.id)
        assertNull(Discovery.parseAnswer("""{"app":"other"}""", "10.0.0.2"))
        assertNull(Discovery.parseAnswer("not json", "10.0.0.2"))
    }
}
