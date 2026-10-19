package no.phobic.phobicccp.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.launch
import no.phobic.phobicccp.model.Design

/**
 * Send a design to any PhobiCCCP that is open on the network, on any of its
 * machines, and optionally start cutting it straight away. Starting is asked
 * for every send (off by default) and confirmed once more before it goes.
 */
@Composable
fun SendDialog(vm: AppViewModel, design: Design, onDismiss: () -> Unit, onDone: (String) -> Unit) {
    val state by vm.state.collectAsState()
    val reachable = state.computers.filter { it.info != null && it.error == null && it.desktop.online(state.now) }
    val last = vm.lastTarget.split('/')
    var computerId by remember { mutableStateOf(reachable.firstOrNull { it.desktop.id == last.getOrNull(0) }?.desktop?.id
        ?: reachable.firstOrNull()?.desktop?.id) }
    val computer = reachable.firstOrNull { it.desktop.id == computerId }
    var machineId by remember(computerId) {
        mutableStateOf(computer?.info?.machines?.firstOrNull { it.id == last.getOrNull(1) }?.id ?: computer?.info?.activeMachine)
    }
    var start by remember { mutableStateOf(false) }
    var confirming by remember { mutableStateOf(false) }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    val scope = rememberCoroutineScope()

    fun go() {
        val c = computer ?: return
        busy = true
        error = null
        vm.lastTarget = "${c.desktop.id}/${machineId.orEmpty()}"
        scope.launch {
            vm.send(design, c, machineId.orEmpty(), start).fold(
                onSuccess = { r ->
                    busy = false
                    val text = (if (start && r.started) "Cutting started on ${c.name}." else r.message.ifEmpty { "Sent to ${c.name}." }) +
                        (if (r.notes.isNotEmpty()) " " + r.notes.joinToString("; ") else "")
                    onDone(text)
                },
                onFailure = {
                    busy = false
                    error = it.message ?: "Could not send"
                },
            )
        }
    }

    if (confirming) {
        val machine = computer?.info?.machines?.firstOrNull { it.id == machineId }?.name ?: "the machine"
        AlertDialog(
            onDismissRequest = { confirming = false },
            title = { Text("Start cutting now?") },
            text = {
                Text("“${design.name}” will start on $machine at ${computer?.name} as soon as it arrives, " +
                    "and the spindle will spin up. Make sure the stock is clamped, the right bit is in, " +
                    "the zero is set and nobody is near the machine.")
            },
            confirmButton = { TextButton(onClick = { confirming = false; go() }) { Text("Send and start", color = Brand.Red, fontWeight = FontWeight.Bold) } },
            dismissButton = { TextButton(onClick = { confirming = false }) { Text("Cancel") } },
        )
        return
    }

    AlertDialog(
        onDismissRequest = { if (!busy) onDismiss() },
        title = { Text("Send “${design.name}”") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                if (reachable.isEmpty()) {
                    Text("No PhobiCCCP is answering right now. Open it on a computer on this network " +
                        "(or add it under Machines) and try again.", color = Brand.TextDim)
                    return@Column
                }
                Text("To", style = MaterialTheme.typography.labelLarge, color = Brand.TextDim)
                for (c in reachable) Row(Modifier.fillMaxWidth().clickable { computerId = c.desktop.id },
                    verticalAlignment = Alignment.CenterVertically) {
                    RadioButton(selected = c.desktop.id == computerId, onClick = null)
                    Spacer(Modifier.width(8.dp))
                    Column {
                        Text(c.name)
                        Text(c.info?.status?.words().orEmpty(), color = Brand.TextDim, style = MaterialTheme.typography.bodySmall)
                    }
                }
                val machines = computer?.info?.machines.orEmpty()
                if (machines.size > 1) {
                    Spacer(Modifier.size(4.dp))
                    Text("Machine", style = MaterialTheme.typography.labelLarge, color = Brand.TextDim)
                    for (m in machines) Row(Modifier.fillMaxWidth().clickable { machineId = m.id },
                        verticalAlignment = Alignment.CenterVertically) {
                        RadioButton(selected = m.id == machineId, onClick = null)
                        Spacer(Modifier.width(8.dp))
                        Text(m.name + if (m.active) "  (in use)" else "")
                    }
                }
                Spacer(Modifier.size(4.dp))
                Row(Modifier.fillMaxWidth().clickable { start = !start }, verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(checked = start, onCheckedChange = { start = it })
                    Text("Start cutting right away")
                }
                val info = computer?.info
                if (start && info != null) {
                    val usesOther = machineId != null && machineId != info.activeMachine
                    val warn = when {
                        !info.allowStart -> "This computer does not allow starting from a phone (Machine › Phones may start cutting). " +
                            "The design is opened there, not started."
                        usesOther -> "That machine is not the one in use; phobiCCCP switches to it and reconnects, " +
                            "and it will most likely need Initialize before it can cut."
                        !info.status.readyToCut() -> "The machine is not ready (${info.status.words()}); it will be opened, not started."
                        design.toolpaths.isEmpty() -> "This design has no toolpaths, so there is nothing to cut."
                        else -> null
                    }
                    warn?.let { Text(it, color = Brand.Amber, style = MaterialTheme.typography.bodySmall) }
                }
                if (busy) Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(top = 8.dp)) {
                    CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                    Spacer(Modifier.width(8.dp))
                    Text("Sending… (phobiCCCP may ask about unsaved work first)", color = Brand.TextDim)
                }
                error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
            }
        },
        confirmButton = {
            TextButton(enabled = computer != null && !busy, onClick = { if (start) confirming = true else go() }) {
                Text(if (start) "Send and start…" else "Send")
            }
        },
        dismissButton = { TextButton(enabled = !busy, onClick = onDismiss) { Text("Cancel") } },
    )
}
