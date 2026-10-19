package no.phobic.phobicccp.ui

import android.app.Application
import android.os.Build
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import no.phobic.phobicccp.data.DesignStore
import no.phobic.phobicccp.data.Settings
import no.phobic.phobicccp.model.Design
import no.phobic.phobicccp.model.Desktop
import no.phobic.phobicccp.model.DesktopInfo
import no.phobic.phobicccp.model.SendResult
import no.phobic.phobicccp.model.Stock
import no.phobic.phobicccp.model.ToolLibrary
import no.phobic.phobicccp.net.CccpConnection
import no.phobic.phobicccp.net.Discovery
import no.phobic.phobicccp.net.Protocol
import no.phobic.phobicccp.sync.Syncer
import java.io.File

/** A phobiCCCP the phone knows, with what it said last. */
data class Computer(
    val desktop: Desktop,
    val info: DesktopInfo? = null,
    val error: String? = null,
    val lastSync: Long = 0,
    val toolsAt: Long = 0,
) {
    val name get() = desktop.host.ifEmpty { desktop.address }
}

data class UiState(
    val computers: List<Computer> = emptyList(),
    val designs: List<Design> = emptyList(),
    val tools: ToolLibrary = ToolLibrary(),
    val syncing: Boolean = false,
    val manualHosts: Set<String> = emptySet(),
    val now: Long = System.currentTimeMillis(),
)

/**
 * Keeps the phone in step with every phobiCCCP on the network while the app
 * is open: looks for them every few seconds, and syncs designs, tool library
 * and machine status with each one it can reach, straight away after an edit.
 */
class AppViewModel(app: Application) : AndroidViewModel(app) {
    private val store = DesignStore(File(app.filesDir, "designs"))
    private val settings = Settings(app)
    private val syncLock = Mutex()
    private var syncSoon: Job? = null

    private val _state = MutableStateFlow(
        UiState(designs = store.live(), tools = settings.tools, manualHosts = settings.manualHosts),
    )
    val state: StateFlow<UiState> = _state.asStateFlow()

    val phoneName: String = Build.MODEL ?: "phone"

    init {
        viewModelScope.launch(Dispatchers.IO) {
            while (isActive) {
                discover()
                syncAll()
                delay(ROUND_MS)
            }
        }
    }

    private fun discover() {
        val found = runCatching { Discovery.probe(settings.manualHosts) }.getOrDefault(emptyList())
        val now = System.currentTimeMillis()
        _state.update { s ->
            val byId = s.computers.associateBy { it.desktop.id }.toMutableMap()
            for (d in found)
                byId[d.id] = (byId[d.id] ?: Computer(d)).copy(desktop = d)
            // A computer typed in but not answering still shows, as offline.
            byId.keys.removeAll { it.startsWith("manual:") }
            for (h in settings.manualHosts)
                if (byId.values.none { it.desktop.typed == h })
                    byId["manual:$h"] = Computer(Desktop(id = "manual:$h", host = h, address = h,
                        port = Protocol.PORT, typed = h))
            s.copy(computers = byId.values.sortedBy { it.name.lowercase() }, now = now)
        }
    }

    private suspend fun syncAll(): Unit = syncLock.withLock {
        val now = System.currentTimeMillis()
        // Typed-in ones are tried too: discovery datagrams may not get through where TCP does.
        val online = _state.value.computers.filter { it.desktop.online(now) || it.desktop.manual }
        if (online.isEmpty()) return@withLock
        _state.update { it.copy(syncing = true) }
        for (c in online) {
            val withTools = now - c.toolsAt > TOOLS_MS || _state.value.tools.tools.isEmpty()
            val r = runCatching { Syncer.syncWith(c.desktop, store, withTools) }
            _state.update { s ->
                s.copy(computers = s.computers.map {
                    if (it.desktop.id != c.desktop.id) it
                    else r.fold(
                        onSuccess = { res -> it.copy(info = res.info, error = null, lastSync = System.currentTimeMillis(),
                            toolsAt = if (res.tools != null) System.currentTimeMillis() else it.toolsAt,
                            desktop = it.desktop.copy(lastSeen = System.currentTimeMillis())) },
                        onFailure = { e -> it.copy(error = e.message ?: e.javaClass.simpleName) },
                    )
                })
            }
            r.getOrNull()?.tools?.takeIf { it.tools.isNotEmpty() }?.let { t ->
                if (t != _state.value.tools) {
                    settings.tools = t
                    _state.update { it.copy(tools = t) }
                }
            }
        }
        _state.update { it.copy(syncing = false, designs = store.live(), now = System.currentTimeMillis()) }
    }

    /** Sync shortly, once the edits stop coming. */
    private fun requestSync() {
        syncSoon?.cancel()
        syncSoon = viewModelScope.launch(Dispatchers.IO) {
            delay(1500)
            syncAll()
        }
    }

    fun syncNow() {
        viewModelScope.launch(Dispatchers.IO) {
            discover()
            syncAll()
        }
    }

    fun design(id: String): Design? = _state.value.designs.firstOrNull { it.id == id } ?: store.get(id)

    fun newDesign(name: String, stock: Stock, material: String): Design {
        val d = store.save(Design(name = name.ifBlank { "Design" }, stock = stock, material = material))
        _state.update { it.copy(designs = store.live()) }
        requestSync()
        return d
    }

    fun saveDesign(design: Design) {
        store.save(design)
        _state.update { it.copy(designs = store.live()) }
        requestSync()
    }

    fun deleteDesign(id: String) {
        store.delete(id)
        _state.update { it.copy(designs = store.live()) }
        requestSync()
    }

    fun duplicateDesign(id: String) {
        val d = store.get(id) ?: return
        store.save(d.copy(id = Design.newId(), name = "${d.name} copy"))
        _state.update { it.copy(designs = store.live()) }
        requestSync()
    }

    fun addHost(host: String) {
        val h = host.trim()
        if (h.isEmpty()) return
        settings.manualHosts = settings.manualHosts + h
        _state.update { it.copy(manualHosts = settings.manualHosts) }
        syncNow()
    }

    fun removeHost(host: String) {
        settings.manualHosts = settings.manualHosts - host
        _state.update { s ->
            s.copy(manualHosts = settings.manualHosts,
                computers = s.computers.filterNot { it.desktop.manual && (it.desktop.address == host || it.desktop.host == host) })
        }
    }

    var lastTarget: String
        get() = settings.lastTarget
        set(v) {
            settings.lastTarget = v
        }

    /** Make [machineId] the machine in use on that computer. */
    suspend fun selectMachine(computer: Computer, machineId: String): Result<DesktopInfo> = withContext(Dispatchers.IO) {
        runCatching {
            CccpConnection(computer.desktop.address, computer.desktop.port).use { it.selectMachine(machineId) }
        }.onSuccess { info ->
            _state.update { s ->
                s.copy(computers = s.computers.map { if (it.desktop.id == computer.desktop.id) it.copy(info = info) else it })
            }
        }
    }

    /** Send [design] to open on that computer (and machine), and start it when asked. */
    suspend fun send(design: Design, computer: Computer, machineId: String, start: Boolean): Result<SendResult> =
        withContext(Dispatchers.IO) {
            runCatching {
                CccpConnection(computer.desktop.address, computer.desktop.port).use {
                    it.send(design, machineId, start, phoneName)
                }
            }.also { syncNow() }
        }

    companion object {
        const val ROUND_MS = 5_000L
        const val TOOLS_MS = 120_000L
    }
}
