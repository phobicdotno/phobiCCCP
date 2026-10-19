package no.phobic.phobicccp.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.KeyboardArrowUp
import androidx.compose.material.icons.automirrored.filled.Send
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Surface
import androidx.compose.material3.Tab
import androidx.compose.material3.TabRow
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.launch
import no.phobic.phobicccp.model.Design
import no.phobic.phobicccp.model.Shape
import no.phobic.phobicccp.model.ToolLibrary
import no.phobic.phobicccp.model.ToolpathSpec

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun EditorScreen(vm: AppViewModel, id: String, onClose: () -> Unit) {
    val state by vm.state.collectAsState()
    var design by remember(id) { mutableStateOf(vm.design(id)) }
    val d = design
    LaunchedEffect(d) { if (d == null) onClose() }
    if (d == null) return

    var selected by remember { mutableStateOf<String?>(null) }
    var tab by rememberSaveable { mutableStateOf(0) }
    var editingShape by remember { mutableStateOf<Shape?>(null) }
    var editingToolpath by remember { mutableStateOf<Int?>(null) }   // -1: a new one
    var editingStock by remember { mutableStateOf(false) }
    var sending by remember { mutableStateOf(false) }
    var pathDraft by remember { mutableStateOf<List<List<Double>>?>(null) }
    var pathClosed by remember { mutableStateOf(true) }
    val snack = remember { SnackbarHostState() }
    val scope = rememberCoroutineScope()

    fun commit(next: Design) {
        design = next
        vm.saveDesign(next)
    }

    fun addShape(s: Shape) {
        commit(d.copy(shapes = d.shapes + s))
        selected = s.id
        editingShape = s
    }

    val cx = d.stock.width / 2
    val cy = d.stock.height / 2
    val unit = minOf(d.stock.width, d.stock.height) / 4

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(d.name.ifEmpty { "Untitled" }, maxLines = 1) },
                navigationIcon = { IconButton(onClick = onClose) { Icon(Icons.AutoMirrored.Filled.ArrowBack, "Back") } },
                actions = {
                    IconButton(onClick = { editingStock = true }) { Icon(Icons.Filled.Settings, "Stock and name") }
                    IconButton(onClick = { sending = true }) { Icon(Icons.AutoMirrored.Filled.Send, "Send", tint = Brand.Gold) }
                },
                colors = TopAppBarDefaults.topAppBarColors(containerColor = Brand.Background),
            )
        },
        snackbarHost = { SnackbarHost(snack) },
    ) { pad ->
        Column(Modifier.padding(pad).fillMaxSize()) {
            val highlight = editingToolpath?.let { d.toolpaths.getOrNull(it)?.shapes?.toSet() }.orEmpty()
            DesignCanvas(
                design = d,
                selected = selected,
                highlighted = highlight,
                pathDraft = pathDraft.orEmpty(),
                onSelect = { selected = it },
                onMove = { sid, dx, dy ->
                    design?.let { cur -> design = cur.copy(shapes = cur.shapes.map { if (it.id == sid) it.moved(dx, dy) else it }) }
                },
                onMoveDone = { design?.let { vm.saveDesign(it) } },
                onAddPoint = pathDraft?.let { draft -> { x, y -> pathDraft = draft + listOf(listOf(round(x), round(y))) } },
                modifier = Modifier.fillMaxWidth().weight(1f),
            )
            val draft = pathDraft
            if (draft != null) {
                Surface(color = Brand.Surface) {
                    Row(Modifier.fillMaxWidth().padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text("Tap to add points (${draft.size})", Modifier.weight(1f))
                        Checkbox(pathClosed, { pathClosed = it })
                        Text("Closed")
                        TextButton(onClick = { pathDraft = null }) { Text("Cancel") }
                        TextButton(enabled = draft.size >= 2, onClick = {
                            addShape(Shape(d.nextShapeId(), Shape.PATH, points = draft, closed = pathClosed && draft.size >= 3))
                            pathDraft = null
                        }) { Text("Done") }
                    }
                }
            }
            selected?.let { sid -> d.shape(sid) }?.let { s ->
                Surface(color = Brand.SurfaceHigh) {
                    Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text(s.label(), Modifier.weight(1f), maxLines = 1)
                        IconButton(onClick = { editingShape = s }) { Icon(Icons.Filled.Edit, "Edit") }
                        IconButton(onClick = {
                            commit(d.copy(shapes = d.shapes - s,
                                toolpaths = d.toolpaths.map { t -> t.copy(shapes = t.shapes - s.id) }))
                            selected = null
                        }) { Icon(Icons.Filled.Delete, "Delete") }
                    }
                }
            }
            TabRow(selectedTabIndex = tab, containerColor = Brand.Background) {
                Tab(tab == 0, { tab = 0 }, text = { Text("Shapes (${d.shapes.size})") })
                Tab(tab == 1, { tab = 1 }, text = { Text("Toolpaths (${d.toolpaths.size})") })
            }
            Column(Modifier.fillMaxWidth().weight(0.8f)) {
                if (tab == 0) {
                    Row(Modifier.horizontalScroll(rememberScrollState()).padding(8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        AssistChip(onClick = { addShape(Shape(d.nextShapeId(), Shape.RECT, x = round(cx - unit), y = round(cy - unit / 2), w = round(unit * 2), h = round(unit))) },
                            label = { Text("Rectangle") })
                        AssistChip(onClick = { addShape(Shape(d.nextShapeId(), Shape.CIRCLE, x = round(cx), y = round(cy), r = round(unit / 2))) },
                            label = { Text("Circle") })
                        AssistChip(onClick = { addShape(Shape(d.nextShapeId(), Shape.POLYGON, x = round(cx), y = round(cy), r = round(unit / 2), sides = 6)) },
                            label = { Text("Polygon") })
                        AssistChip(onClick = { addShape(Shape(d.nextShapeId(), Shape.TEXT, x = round(cx - unit), y = round(cy), text = "Text", size = round(unit / 2))) },
                            label = { Text("Text") })
                        AssistChip(onClick = { selected = null; pathDraft = emptyList() }, label = { Text("Path") })
                        AssistChip(onClick = { addShape(Shape(d.nextShapeId(), Shape.POINT, x = round(cx), y = round(cy))) },
                            label = { Text("Drill point") })
                    }
                    LazyColumn(Modifier.fillMaxSize()) {
                        itemsIndexed(d.shapes, key = { _, s -> s.id }) { _, s ->
                            Row(Modifier.fillMaxWidth().clickable { selected = s.id }.padding(horizontal = 16.dp, vertical = 10.dp)) {
                                Text(s.label(), color = if (s.id == selected) Brand.Gold else Brand.Text)
                            }
                            HorizontalDivider(color = Brand.Surface)
                        }
                    }
                } else {
                    Row(Modifier.padding(8.dp)) {
                        OutlinedButton(onClick = { editingToolpath = -1 }, enabled = d.shapes.isNotEmpty()) { Text("Add toolpath") }
                    }
                    LazyColumn(Modifier.fillMaxSize()) {
                        itemsIndexed(d.toolpaths) { i, t ->
                            Row(Modifier.fillMaxWidth().clickable { editingToolpath = i }.padding(start = 16.dp),
                                verticalAlignment = Alignment.CenterVertically) {
                                Column(Modifier.weight(1f).padding(vertical = 8.dp)) {
                                    Text("${i + 1}. ${t.name}")
                                    val tool = state.tools.tool(t.tool)?.name ?: "tool #${t.tool}"
                                    Text("${ToolpathSpec.typeLabel(t.type)}${if (t.type == "profile") " ${t.side}" else ""}, " +
                                        "${fmt(t.depth)} mm deep, $tool, ${t.shapes.size} shape(s)",
                                        color = Brand.TextDim, style = MaterialTheme.typography.bodySmall)
                                }
                                IconButton(enabled = i > 0, onClick = {
                                    commit(d.copy(toolpaths = d.toolpaths.toMutableList().apply { add(i - 1, removeAt(i)) }))
                                }) { Icon(Icons.Filled.KeyboardArrowUp, "Earlier") }
                                IconButton(enabled = i < d.toolpaths.size - 1, onClick = {
                                    commit(d.copy(toolpaths = d.toolpaths.toMutableList().apply { add(i + 1, removeAt(i)) }))
                                }) { Icon(Icons.Filled.KeyboardArrowDown, "Later") }
                            }
                            HorizontalDivider(color = Brand.Surface)
                        }
                    }
                }
            }
        }
    }

    editingShape?.let { s ->
        ShapeDialog(s, onDismiss = { editingShape = null }, onSave = { n ->
            commit(d.copy(shapes = d.shapes.map { if (it.id == n.id) n else it }))
            editingShape = null
        })
    }
    editingToolpath?.let { i ->
        val existing = d.toolpaths.getOrNull(i)
        ToolpathDialog(
            spec = existing,
            design = d,
            tools = state.tools,
            preselect = listOfNotNull(selected),
            onDismiss = { editingToolpath = null },
            onSave = { t ->
                commit(d.copy(toolpaths = if (existing == null) d.toolpaths + t else d.toolpaths.mapIndexed { j, o -> if (j == i) t else o }))
                editingToolpath = null
            },
            onDelete = if (existing == null) null else {
                {
                    commit(d.copy(toolpaths = d.toolpaths.filterIndexed { j, _ -> j != i }))
                    editingToolpath = null
                }
            },
        )
    }
    if (editingStock) {
        var name by remember { mutableStateOf(d.name) }
        var stock by remember { mutableStateOf(d.stock) }
        var material by remember { mutableStateOf(d.material) }
        AlertDialog(
            onDismissRequest = { editingStock = false },
            title = { Text("Design") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(name, { name = it }, label = { Text("Name") }, singleLine = true, modifier = Modifier.fillMaxWidth())
                    StockFields(stock, material, state.tools, { stock = it }, { material = it })
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    commit(d.copy(name = name, stock = stock, material = material))
                    editingStock = false
                }) { Text("OK") }
            },
            dismissButton = { TextButton(onClick = { editingStock = false }) { Text("Cancel") } },
        )
    }
    if (sending) SendDialog(vm, d, onDismiss = { sending = false }) { msg ->
        sending = false
        scope.launch { snack.showSnackbar(msg) }
    }
}

private fun round(v: Double) = Math.round(v * 100) / 100.0

@Composable
private fun ShapeDialog(shape: Shape, onDismiss: () -> Unit, onSave: (Shape) -> Unit) {
    var s by remember(shape.id) { mutableStateOf(shape) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(s.label()) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                if (s.type != Shape.PATH) Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    val what = when (s.type) { Shape.RECT -> "corner"; Shape.TEXT -> "start"; else -> "centre" }
                    NumberField("X ($what)", s.x, { s = s.copy(x = it) }, Modifier.weight(1f))
                    NumberField("Y ($what)", s.y, { s = s.copy(y = it) }, Modifier.weight(1f))
                }
                when (s.type) {
                    Shape.RECT -> Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        NumberField("Width", s.w ?: 10.0, { s = s.copy(w = it) }, Modifier.weight(1f), 0.01)
                        NumberField("Height", s.h ?: 10.0, { s = s.copy(h = it) }, Modifier.weight(1f), 0.01)
                    }
                    Shape.CIRCLE -> NumberField("Diameter", (s.r ?: 5.0) * 2, { s = s.copy(r = it / 2) }, Modifier.fillMaxWidth(), 0.02)
                    Shape.POLYGON -> Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        NumberField("Radius", s.r ?: 5.0, { s = s.copy(r = it) }, Modifier.weight(1f), 0.01)
                        NumberField("Sides", (s.sides ?: 6).toDouble(), { s = s.copy(sides = it.toInt()) }, Modifier.weight(1f), 3.0, 100.0)
                    }
                    Shape.TEXT -> {
                        OutlinedTextField(s.text.orEmpty(), { s = s.copy(text = it.take(500)) }, label = { Text("Text") }, modifier = Modifier.fillMaxWidth())
                        NumberField("Letter height", s.size ?: 10.0, { s = s.copy(size = it) }, Modifier.fillMaxWidth(), 0.5, 2000.0)
                    }
                    Shape.PATH -> {
                        Text("${s.points?.size ?: 0} points. Drag it on the drawing to move it.", color = Brand.TextDim)
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Checkbox(s.closed == true, { s = s.copy(closed = it && (s.points?.size ?: 0) >= 3) })
                            Text("Closed")
                        }
                    }
                }
            }
        },
        confirmButton = {
            TextButton(enabled = s.type != Shape.TEXT || !s.text.isNullOrBlank(), onClick = { onSave(s) }) { Text("OK") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

/** A sensible first tool for a kind of toolpath, from what the library has for the material. */
fun defaultTool(tools: ToolLibrary, material: String, type: String): Int? {
    val have = tools.toolsFor(material)
    val wanted = if (type == "engrave") listOf("engraver", "vbit", "endmill") else listOf("endmill", "ball")
    for (k in wanted) have.firstOrNull { it.kind == k }?.let { return it.number }
    return have.firstOrNull()?.number
}

@Composable
private fun ToolpathDialog(
    spec: ToolpathSpec?,
    design: Design,
    tools: ToolLibrary,
    preselect: List<String>,
    onDismiss: () -> Unit,
    onSave: (ToolpathSpec) -> Unit,
    onDelete: (() -> Unit)?,
) {
    var t by remember {
        mutableStateOf(spec ?: ToolpathSpec(
            name = "Toolpath ${design.toolpaths.size + 1}",
            type = "profile",
            shapes = preselect.ifEmpty { design.shapes.map { it.id } },
            tool = defaultTool(tools, design.material, "profile") ?: 201,
            depth = minOf(3.0, design.stock.thickness),
        ))
    }
    val available = tools.toolsFor(design.material)
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(if (spec == null) "New toolpath" else "Toolpath") },
        text = {
            LazyColumn(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                item {
                    OutlinedTextField(t.name, { t = t.copy(name = it.take(60)) }, label = { Text("Name") }, singleLine = true, modifier = Modifier.fillMaxWidth())
                }
                item {
                    Row(Modifier.horizontalScroll(rememberScrollState())) {
                        ChipChoice(ToolpathSpec.TYPES, t.type, ToolpathSpec::typeLabel) { ty ->
                            val tool = if (tools.tool(t.tool)?.let { design.material in it.materials } == true && ty != "engrave") t.tool
                                       else defaultTool(tools, design.material, ty) ?: t.tool
                            t = t.copy(type = ty, tool = tool)
                        }
                    }
                }
                if (t.type == "profile") item {
                    ChipChoice(ToolpathSpec.SIDES, t.side, { it.replaceFirstChar(Char::uppercase) }) { t = t.copy(side = it) }
                }
                item {
                    NumberField("Depth below the top (mm)", t.depth, { t = t.copy(depth = it) }, Modifier.fillMaxWidth(), 0.0, 1000.0)
                    if (t.depth > design.stock.thickness)
                        Text("Deeper than the stock (${fmt(design.stock.thickness)} mm): it cuts into the spoilboard.", color = Brand.Amber,
                            style = MaterialTheme.typography.bodySmall)
                }
                item {
                    if (available.isEmpty()) {
                        NumberField("Tool number", t.tool.toDouble(), { t = t.copy(tool = it.toInt()) }, Modifier.fillMaxWidth(), 1.0, 1e9)
                        Text("The tool list comes from phobiCCCP's library once the phone has found it.", color = Brand.TextDim,
                            style = MaterialTheme.typography.bodySmall)
                    } else {
                        Dropdown("Tool (${tools.materialName(design.material)})", available, available.firstOrNull { it.number == t.tool },
                            { "${it.name}${if (it.diameter > 0) " · Ø${fmt(it.diameter)}" else ""}" }) { t = t.copy(tool = it.number) }
                        if (available.none { it.number == t.tool })
                            Text("Tool #${t.tool} has no feeds for this material; pick another.", color = Brand.Amber,
                                style = MaterialTheme.typography.bodySmall)
                    }
                }
                item { Text("Shapes", style = MaterialTheme.typography.labelLarge, color = Brand.TextDim) }
                itemsIndexed(design.shapes) { _, s ->
                    Row(Modifier.fillMaxWidth().clickable {
                        t = t.copy(shapes = if (s.id in t.shapes) t.shapes - s.id else t.shapes + s.id)
                    }, verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(s.id in t.shapes, null)
                        Spacer(Modifier.width(8.dp))
                        Text(s.label())
                    }
                }
                if (onDelete != null) item {
                    Button(onClick = onDelete, colors = ButtonDefaults.buttonColors(containerColor = Brand.SurfaceHigh)) {
                        Text("Delete toolpath")
                    }
                }
            }
        },
        confirmButton = {
            val ok = t.shapes.isNotEmpty() && (available.isEmpty() || available.any { it.number == t.tool })
            TextButton(enabled = ok, onClick = { onSave(t.copy(name = t.name.ifBlank { ToolpathSpec.typeLabel(t.type) })) }) { Text("OK") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}
