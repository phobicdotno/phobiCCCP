package no.phobic.phobicccp.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.automirrored.filled.List
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.automirrored.filled.Send
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.launch
import no.phobic.phobicccp.model.Design
import no.phobic.phobicccp.model.Stock
import java.text.DateFormat
import java.util.Date

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HomeScreen(vm: AppViewModel, onOpen: (String) -> Unit) {
    val state by vm.state.collectAsState()
    var tab by rememberSaveable { mutableStateOf(0) }
    var creating by remember { mutableStateOf(false) }
    var sending by remember { mutableStateOf<Design?>(null) }
    val snack = remember { SnackbarHostState() }
    val scope = rememberCoroutineScope()

    Scaffold(
        topBar = {
            Column {
                TopAppBar(
                    title = { Text("PhobiCCCP", fontWeight = FontWeight.Bold) },
                    actions = {
                        IconButton(onClick = { vm.syncNow() }) { Icon(Icons.Filled.Refresh, "Sync now") }
                    },
                    colors = TopAppBarDefaults.topAppBarColors(containerColor = Brand.Background),
                )
                if (state.syncing) LinearProgressIndicator(Modifier.fillMaxWidth())
            }
        },
        bottomBar = {
            NavigationBar(containerColor = Brand.Surface) {
                NavigationBarItem(selected = tab == 0, onClick = { tab = 0 },
                    icon = { Icon(Icons.AutoMirrored.Filled.List, null) }, label = { Text("Designs") })
                NavigationBarItem(selected = tab == 1, onClick = { tab = 1 },
                    icon = { Icon(Icons.Filled.Home, null) }, label = { Text("Machines") })
            }
        },
        floatingActionButton = {
            if (tab == 0) FloatingActionButton(onClick = { creating = true }, containerColor = Brand.Red) {
                Icon(Icons.Filled.Add, "New design")
            }
        },
        snackbarHost = { SnackbarHost(snack) },
    ) { pad ->
        Box(Modifier.padding(pad).fillMaxSize()) {
            if (tab == 0) DesignsTab(state, onOpen, onSend = { sending = it }, vm)
            else ComputersTab(state, vm) { msg -> scope.launch { snack.showSnackbar(msg) } }
        }
    }

    if (creating) NewDesignDialog(state, onDismiss = { creating = false }) { name, stock, material ->
        creating = false
        onOpen(vm.newDesign(name, stock, material).id)
    }
    sending?.let { d ->
        SendDialog(vm, d, onDismiss = { sending = null }) { msg ->
            sending = null
            scope.launch { snack.showSnackbar(msg) }
        }
    }
}

@Composable
private fun DesignsTab(state: UiState, onOpen: (String) -> Unit, onSend: (Design) -> Unit, vm: AppViewModel) {
    if (state.designs.isEmpty()) {
        Column(Modifier.fillMaxSize().padding(24.dp), verticalArrangement = Arrangement.Center) {
            Text("No designs yet.", style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.size(8.dp))
            Text("Make one with +. Designs on this phone are kept in step with every PhobiCCCP " +
                "on the same network, so one made on another phone shows up here too.",
                color = Brand.TextDim)
        }
        return
    }
    var deleting by remember { mutableStateOf<Design?>(null) }
    LazyColumn(Modifier.fillMaxSize(), contentPadding = androidx.compose.foundation.layout.PaddingValues(12.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp)) {
        items(state.designs, key = { it.id }) { d ->
            var menu by remember { mutableStateOf(false) }
            Card(Modifier.fillMaxWidth().clickable { onOpen(d.id) },
                colors = CardDefaults.cardColors(containerColor = Brand.Surface)) {
                Row(Modifier.padding(start = 16.dp, top = 12.dp, bottom = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(d.name.ifEmpty { "Untitled" }, style = MaterialTheme.typography.titleMedium)
                        Text("${fmt(d.stock.width)} × ${fmt(d.stock.height)} × ${fmt(d.stock.thickness)} mm · " +
                            "${d.shapes.size} shapes · ${d.toolpaths.size} toolpaths", color = Brand.TextDim,
                            style = MaterialTheme.typography.bodySmall)
                        Text(DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT).format(Date(d.modified)),
                            color = Brand.TextDim, style = MaterialTheme.typography.bodySmall)
                    }
                    IconButton(onClick = { onSend(d) }) { Icon(Icons.AutoMirrored.Filled.Send, "Send", tint = Brand.Gold) }
                    Box {
                        IconButton(onClick = { menu = true }) { Icon(Icons.Filled.MoreVert, "More") }
                        DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                            DropdownMenuItem(text = { Text("Duplicate") }, onClick = { menu = false; vm.duplicateDesign(d.id) })
                            DropdownMenuItem(text = { Text("Delete") }, onClick = { menu = false; deleting = d },
                                leadingIcon = { Icon(Icons.Filled.Delete, null) })
                        }
                    }
                }
            }
        }
    }
    deleting?.let { d ->
        AlertDialog(
            onDismissRequest = { deleting = null },
            title = { Text("Delete “${d.name}”?") },
            text = { Text("It is removed from this phone and from every PhobiCCCP it syncs with.") },
            confirmButton = { TextButton(onClick = { vm.deleteDesign(d.id); deleting = null }) { Text("Delete") } },
            dismissButton = { TextButton(onClick = { deleting = null }) { Text("Cancel") } },
        )
    }
}

@Composable
private fun ComputersTab(state: UiState, vm: AppViewModel, toast: (String) -> Unit) {
    var adding by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    LazyColumn(Modifier.fillMaxSize(), contentPadding = androidx.compose.foundation.layout.PaddingValues(12.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp)) {
        if (state.computers.isEmpty()) item {
            Text("Looking for PhobiCCCP on this network…", style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.size(4.dp))
            Text("Start phobiCCCP on a computer on the same Wi-Fi, with File › Receive from Syntexophobic " +
                "and phones turned on. Over Tailscale or across networks, add it by name or address below.",
                color = Brand.TextDim)
        }
        items(state.computers, key = { it.desktop.id }) { c ->
            val online = c.desktop.online(state.now) && c.error == null
            Card(Modifier.fillMaxWidth(), colors = CardDefaults.cardColors(containerColor = Brand.Surface)) {
                Column(Modifier.padding(16.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Box(Modifier.size(10.dp).background(if (online) Brand.Good else Brand.TextDim, CircleShape))
                        Spacer(Modifier.width(8.dp))
                        Column(Modifier.weight(1f)) {
                            Text(c.name, style = MaterialTheme.typography.titleMedium)
                            Text(listOfNotNull(c.desktop.address.takeIf { it != c.name },
                                c.info?.version?.takeIf { it.isNotEmpty() }?.let { "phobiCCCP $it" })
                                .joinToString(" · "), color = Brand.TextDim, style = MaterialTheme.typography.bodySmall)
                        }
                        if (c.desktop.manual) IconButton(onClick = { vm.removeHost(c.desktop.typed) }) {
                            Icon(Icons.Filled.Delete, "Forget")
                        }
                    }
                    c.error?.let { Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }
                    val info = c.info ?: return@Column
                    Spacer(Modifier.size(8.dp))
                    Text(info.status.words() + (info.document.takeIf { it.isNotEmpty() }?.let { " · open: $it" } ?: ""),
                        color = if (info.status.readyToCut()) Brand.Good else Brand.Amber)
                    Spacer(Modifier.size(4.dp))
                    Text("Machines", style = MaterialTheme.typography.labelLarge, color = Brand.TextDim)
                    for (m in info.machines) Row(
                        Modifier.fillMaxWidth().clickable(enabled = !m.active && online) {
                            scope.launch {
                                vm.selectMachine(c, m.id)
                                    .onSuccess { toast("${c.name} now uses ${m.name}") }
                                    .onFailure { toast("Could not switch: ${it.message}") }
                            }
                        },
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        RadioButton(selected = m.active, onClick = null, enabled = online)
                        Spacer(Modifier.width(8.dp))
                        Column {
                            Text(m.name)
                            if (m.size.isNotEmpty()) Text(m.size, color = Brand.TextDim, style = MaterialTheme.typography.bodySmall)
                        }
                    }
                    Text(if (info.allowStart) "Phones may start cutting here."
                         else "Starting from a phone is off here (Machine › Phones may start cutting).",
                        color = Brand.TextDim, style = MaterialTheme.typography.bodySmall)
                }
            }
        }
        item {
            OutlinedButton(onClick = { adding = true }, modifier = Modifier.fillMaxWidth()) {
                Text("Add a computer by name or address")
            }
        }
    }
    if (adding) {
        var host by remember { mutableStateOf("") }
        AlertDialog(
            onDismissRequest = { adding = false },
            title = { Text("Add a computer") },
            text = {
                Column {
                    Text("Its name or address, for example a Tailscale name, or 192.168.1.20.", color = Brand.TextDim)
                    Spacer(Modifier.size(8.dp))
                    OutlinedTextField(host, { host = it }, singleLine = true, label = { Text("Name or address") })
                }
            },
            confirmButton = { TextButton(onClick = { vm.addHost(host); adding = false }) { Text("Add") } },
            dismissButton = { TextButton(onClick = { adding = false }) { Text("Cancel") } },
        )
    }
}

@Composable
fun NewDesignDialog(state: UiState, onDismiss: () -> Unit, onCreate: (String, Stock, String) -> Unit) {
    var name by remember { mutableStateOf("") }
    var stock by remember { mutableStateOf(Stock()) }
    var material by remember { mutableStateOf("softwood") }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("New design") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(name, { name = it }, label = { Text("Name") }, singleLine = true, modifier = Modifier.fillMaxWidth())
                StockFields(stock, material, state.tools, { stock = it }, { material = it })
            }
        },
        confirmButton = { TextButton(onClick = { onCreate(name, stock, material) }) { Text("Create") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}
