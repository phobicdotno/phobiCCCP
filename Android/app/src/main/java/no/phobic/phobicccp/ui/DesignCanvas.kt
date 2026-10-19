package no.phobic.phobicccp.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.input.pointer.pointerInput
import no.phobic.phobicccp.model.Design
import no.phobic.phobicccp.model.Shape
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.min
import kotlin.math.sin

/** Millimetres on the stock (Y up) to pixels on the view (Y down), fitted with a margin. */
class StockView(val design: Design, size: Size) {
    private val margin = 24f
    val scale: Float = min(
        (size.width - 2 * margin) / design.stock.width.toFloat(),
        (size.height - 2 * margin) / design.stock.height.toFloat(),
    ).coerceAtLeast(0.01f)
    private val ox = (size.width - design.stock.width.toFloat() * scale) / 2
    private val oy = (size.height - design.stock.height.toFloat() * scale) / 2

    fun toScreen(x: Double, y: Double) = Offset(ox + x.toFloat() * scale, oy + (design.stock.height - y).toFloat() * scale)
    fun toMm(p: Offset) = Pair(((p.x - ox) / scale).toDouble(), design.stock.height - ((p.y - oy) / scale).toDouble())
}

/**
 * The design drawn on its stock. Tap a shape to select it, drag the selected
 * shape to move it; in path mode ([onAddPoint] set) every tap adds a point.
 */
@Composable
fun DesignCanvas(
    design: Design,
    selected: String?,
    highlighted: Set<String>,
    pathDraft: List<List<Double>>,
    onSelect: (String?) -> Unit,
    onMove: (String, Double, Double) -> Unit,
    onMoveDone: () -> Unit,
    onAddPoint: ((Double, Double) -> Unit)?,
    modifier: Modifier = Modifier,
) {
    var size by remember { mutableStateOf(Size(1f, 1f)) }
    val d by rememberUpdatedState(design)
    val sel by rememberUpdatedState(selected)
    val add by rememberUpdatedState(onAddPoint)
    val select by rememberUpdatedState(onSelect)
    val move by rememberUpdatedState(onMove)
    val moveDone by rememberUpdatedState(onMoveDone)

    Canvas(
        modifier
            .pointerInput(Unit) {
                detectTapGestures { p ->
                    val view = StockView(d, size)
                    val (x, y) = view.toMm(p)
                    val a = add
                    if (a != null) a(x, y) else select(hit(d, x, y, 12 / view.scale.toDouble()))
                }
            }
            .pointerInput(Unit) {
                var dragging: String? = null
                detectDragGestures(
                    onDragStart = { p ->
                        val view = StockView(d, size)
                        val (x, y) = view.toMm(p)
                        val h = hit(d, x, y, 16 / view.scale.toDouble())
                        dragging = if (add == null && h != null && h == sel) h else null
                    },
                    onDragEnd = { if (dragging != null) moveDone(); dragging = null },
                    onDragCancel = { if (dragging != null) moveDone(); dragging = null },
                ) { change, amount ->
                    val id = dragging ?: return@detectDragGestures
                    change.consume()
                    val s = StockView(d, size).scale
                    move(id, amount.x / s.toDouble(), -amount.y / s.toDouble())
                }
            },
    ) {
        size = this.size
        val view = StockView(design, this.size)
        drawStock(view)
        for (s in design.shapes) {
            val color = when {
                s.id == selected -> Brand.Gold
                s.id in highlighted -> Brand.Amber
                else -> Brand.Text
            }
            drawShape(view, s, color, if (s.id == selected) 3f else 1.6f)
        }
        if (pathDraft.isNotEmpty()) {
            val pts = pathDraft.map { view.toScreen(it[0], it[1]) }
            for (i in 1 until pts.size) drawLine(Brand.Blue, pts[i - 1], pts[i], 2.5f)
            for (p in pts) drawCircle(Brand.Blue, 5f, p)
        }
    }
}

private fun DrawScope.drawStock(view: StockView) {
    val a = view.toScreen(0.0, view.design.stock.height)
    val b = view.toScreen(view.design.stock.width, 0.0)
    drawRect(Color(0xFF3B3226), a, Size(b.x - a.x, b.y - a.y))
    drawRect(Color(0xFF6B5B45), a, Size(b.x - a.x, b.y - a.y), style = Stroke(1.5f))
    // X0 Y0: the corner phobiCCCP zeroes on.
    val o = view.toScreen(0.0, 0.0)
    drawLine(Brand.Red, o, o + Offset(28f, 0f), 3f)
    drawLine(Brand.Good, o, o + Offset(0f, -28f), 3f)
}

private fun DrawScope.drawShape(view: StockView, s: Shape, color: Color, width: Float) {
    val stroke = Stroke(width)
    when (s.type) {
        Shape.RECT -> {
            val a = view.toScreen(s.x, s.y + (s.h ?: 0.0))
            val b = view.toScreen(s.x + (s.w ?: 0.0), s.y)
            drawRect(color, a, Size(b.x - a.x, b.y - a.y), style = stroke)
        }
        Shape.CIRCLE -> drawCircle(color, (s.r ?: 0.0).toFloat() * view.scale, view.toScreen(s.x, s.y), style = stroke)
        Shape.POLYGON -> {
            val n = (s.sides ?: 6).coerceIn(3, 100)
            val r = s.r ?: 0.0
            val path = Path()
            for (i in 0..n) {
                // As phobiCCCP makes it: vertex 0 on +X, counter-clockwise.
                val a = 2 * Math.PI * i / n
                val p = view.toScreen(s.x + r * cos(a), s.y + r * sin(a))
                if (i == 0) path.moveTo(p.x, p.y) else path.lineTo(p.x, p.y)
            }
            drawPath(path, color, style = stroke)
        }
        Shape.TEXT -> {
            val p = view.toScreen(s.x, s.y)
            val paint = android.graphics.Paint().apply {
                this.color = color.toArgb()
                textSize = ((s.size ?: 10.0) * 1.07).toFloat() * view.scale   // size is the ascent, as on the desktop
                isAntiAlias = true
                style = android.graphics.Paint.Style.STROKE
                strokeWidth = 1.2f
                typeface = android.graphics.Typeface.SANS_SERIF
            }
            drawContext.canvas.nativeCanvas.drawText(s.text.orEmpty(), p.x, p.y, paint)
        }
        Shape.PATH -> {
            val pts = s.points.orEmpty()
            if (pts.size < 2) return
            val path = Path()
            pts.forEachIndexed { i, q ->
                val p = view.toScreen(q[0], q[1])
                if (i == 0) path.moveTo(p.x, p.y) else path.lineTo(p.x, p.y)
            }
            if (s.closed == true) path.close()
            drawPath(path, color, style = stroke)
        }
        Shape.POINT -> {
            val p = view.toScreen(s.x, s.y)
            drawLine(color, p + Offset(-7f, 0f), p + Offset(7f, 0f), width)
            drawLine(color, p + Offset(0f, -7f), p + Offset(0f, 7f), width)
        }
    }
    if (width > 2f) {
        val b = s.bounds()
        val a = view.toScreen(b[0], b[3])
        val c = view.toScreen(b[2], b[1])
        drawRect(color.copy(alpha = 0.5f), a - Offset(4f, 4f), Size(c.x - a.x + 8f, c.y - a.y + 8f),
            style = Stroke(1f, pathEffect = PathEffect.dashPathEffect(floatArrayOf(6f, 6f))))
    }
}

/** The shape under (x, y) mm, within [slop] mm; the smallest one wins. */
fun hit(d: Design, x: Double, y: Double, slop: Double): String? =
    d.shapes.filter { s ->
        val b = s.bounds()
        x >= b[0] - slop && x <= b[2] + slop && y >= b[1] - slop && y <= b[3] + slop
    }.minByOrNull { s ->
        val b = s.bounds()
        hypot(b[2] - b[0], b[3] - b[1])
    }?.id
