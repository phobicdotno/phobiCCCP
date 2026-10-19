package no.phobic.phobicccp

import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonObject
import no.phobic.phobicccp.model.Design
import no.phobic.phobicccp.model.PadJson
import no.phobic.phobicccp.model.Shape
import no.phobic.phobicccp.model.ToolpathSpec
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

class DesignJsonTest {
    // tests/data/pad-design.json, which the desktop's test_padlink builds into a document.
    private val fixture = File("../../tests/data/pad-design.json")

    @Test
    fun fixtureRoundTripsUnchanged() {
        val text = fixture.readText()
        val design = PadJson.decodeFromString(Design.serializer(), text)
        assertEquals("Door sign", design.name)
        assertEquals(6, design.shapes.size)
        assertEquals(listOf("engrave", "pocket", "drill", "profile"), design.toolpaths.map { it.type })
        val again = PadJson.parseToJsonElement(PadJson.encodeToString(Design.serializer(), design))
        assertEquals(PadJson.parseToJsonElement(text), again)
    }

    @Test
    fun writesOnlyWhatAShapeUses() {
        val d = Design(id = "x", name = "n", modified = 1,
            shapes = listOf(Shape("s1", Shape.CIRCLE, x = 1.0, y = 2.0, r = 3.0)),
            toolpaths = listOf(ToolpathSpec("p", "pocket", listOf("s1"), 201, 2.0)))
        val o = PadJson.parseToJsonElement(PadJson.encodeToString(Design.serializer(), d)).jsonObject
        assertEquals("phobicccp-pad", o["format"].toString().trim('"'))
        val circle = (o["shapes"] as kotlinx.serialization.json.JsonArray)[0] as JsonObject
        assertEquals(setOf("id", "type", "x", "y", "r"), circle.keys)
    }

    @Test
    fun readsWhatTheDesktopSendsBack() {
        // Qt writes whole numbers without a fraction and may add keys we do not know.
        val text = """{"format":"phobicccp-pad","version":1,"id":"a","name":"A","modified":1760850000000,
            |"stock":{"width":300,"height":200,"thickness":19},"shapes":[{"id":"s1","type":"rect","x":0,"y":0,"w":10,"h":10,"extra":1}],
            |"toolpaths":[],"future":true}""".trimMargin()
        val d = PadJson.decodeFromString(Design.serializer(), text)
        assertEquals(1760850000000, d.modified)
        assertEquals(10.0, d.shapes[0].w!!, 0.0)
        assertFalse(d.deleted)
    }

    @Test
    fun movingAndBounds() {
        val p = Shape("p", Shape.PATH, points = listOf(listOf(0.0, 0.0), listOf(10.0, 5.0)))
        val m = p.moved(1.0, 2.0)
        assertEquals(listOf(listOf(1.0, 2.0), listOf(11.0, 7.0)), m.points)
        assertTrue(m.bounds().contentEquals(doubleArrayOf(1.0, 2.0, 11.0, 7.0)))
        val r = Shape("r", Shape.RECT, x = 1.0, y = 1.0, w = 4.0, h = 2.0).moved(1.0, 0.0)
        assertTrue(r.bounds().contentEquals(doubleArrayOf(2.0, 1.0, 6.0, 3.0)))
    }

    @Test
    fun nextShapeIdSkipsTaken() {
        val d = Design(shapes = listOf(Shape("s2", Shape.POINT)))
        assertEquals("s3", d.nextShapeId())
    }
}
