package no.phobic.phobicccp.model

import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import java.util.UUID

/**
 * A design made on the phone, in the JSON phobiCCCP reads (src/padlink.h in
 * the desktop app documents the same thing). Millimetres, X right, Y up, with
 * X0 Y0 at the stock's front-left corner, as on the desktop.
 */
@Serializable
data class Design(
    val format: String = FORMAT,
    val version: Int = 1,
    val id: String = newId(),
    val name: String = "",
    /** Milliseconds since 1970, set by whoever edited it last. The newer copy wins a sync. */
    val modified: Long = 0,
    /** A removal, kept so it reaches every phobiCCCP too. */
    val deleted: Boolean = false,
    val stock: Stock = Stock(),
    val material: String = "softwood",
    val shapes: List<Shape> = emptyList(),
    val toolpaths: List<ToolpathSpec> = emptyList(),
) {
    fun shape(id: String) = shapes.firstOrNull { it.id == id }

    fun nextShapeId(): String {
        var n = shapes.size + 1
        while (shapes.any { it.id == "s$n" }) n++
        return "s$n"
    }

    companion object {
        const val FORMAT = "phobicccp-pad"
        fun newId(): String = UUID.randomUUID().toString()
    }
}

@Serializable
data class Stock(
    val width: Double = 300.0,
    val height: Double = 200.0,
    val thickness: Double = 19.0,
)

/**
 * One drawn shape. Which fields mean something depends on [type]:
 * rect x/y (lower-left) w/h; circle x/y (centre) r; polygon x/y r sides;
 * text x/y (baseline-left) text size; path points closed; point x/y.
 */
@Serializable
data class Shape(
    val id: String,
    val type: String,
    val x: Double = 0.0,
    val y: Double = 0.0,
    val w: Double? = null,
    val h: Double? = null,
    val r: Double? = null,
    val sides: Int? = null,
    val text: String? = null,
    val size: Double? = null,
    val points: List<List<Double>>? = null,
    val closed: Boolean? = null,
) {
    /** Moved by (dx, dy) mm. */
    fun moved(dx: Double, dy: Double): Shape =
        if (type == PATH) copy(points = points?.map { listOf(it[0] + dx, it[1] + dy) })
        else copy(x = x + dx, y = y + dy)

    /** Lower-left and upper-right corner, roughly (text is guessed from its size). */
    fun bounds(): DoubleArray = when (type) {
        RECT -> doubleArrayOf(x, y, x + (w ?: 0.0), y + (h ?: 0.0))
        CIRCLE, POLYGON -> (r ?: 0.0).let { doubleArrayOf(x - it, y - it, x + it, y + it) }
        TEXT -> (size ?: 10.0).let { doubleArrayOf(x, y, x + it * 0.6 * (text?.length ?: 1), y + it) }
        PATH -> {
            val p = points.orEmpty()
            if (p.isEmpty()) doubleArrayOf(0.0, 0.0, 0.0, 0.0)
            else doubleArrayOf(p.minOf { it[0] }, p.minOf { it[1] }, p.maxOf { it[0] }, p.maxOf { it[1] })
        }
        else -> doubleArrayOf(x - 1, y - 1, x + 1, y + 1)
    }

    fun label(): String = when (type) {
        RECT -> "Rectangle %.1f × %.1f".format(w ?: 0.0, h ?: 0.0)
        CIRCLE -> "Circle Ø%.1f".format((r ?: 0.0) * 2)
        POLYGON -> "${sides ?: 6}-sided polygon"
        TEXT -> "Text “${text.orEmpty()}”"
        PATH -> if (closed == true) "Closed path, ${points?.size ?: 0} points" else "Path, ${points?.size ?: 0} points"
        POINT -> "Point"
        else -> type
    }

    companion object {
        const val RECT = "rect"
        const val CIRCLE = "circle"
        const val POLYGON = "polygon"
        const val TEXT = "text"
        const val PATH = "path"
        const val POINT = "point"
    }
}

@Serializable
data class ToolpathSpec(
    val name: String,
    /** profile | pocket | drill | engrave */
    val type: String,
    val shapes: List<String>,
    /** Tool library number on the desktop (#201 and so on). */
    val tool: Int,
    /** Below the top of the stock, mm. */
    val depth: Double,
    /** profile only: outside | inside | on */
    val side: String = "outside",
) {
    companion object {
        val TYPES = listOf("profile", "pocket", "drill", "engrave")
        val SIDES = listOf("outside", "inside", "on")
        fun typeLabel(type: String) = when (type) {
            "profile" -> "Profile"
            "pocket" -> "Pocket"
            "drill" -> "Drill"
            "engrave" -> "Engrave"
            else -> type
        }
    }
}

/** One JSON setup for everything sent and stored. */
val PadJson = Json {
    ignoreUnknownKeys = true
    encodeDefaults = true
    explicitNulls = false
}
