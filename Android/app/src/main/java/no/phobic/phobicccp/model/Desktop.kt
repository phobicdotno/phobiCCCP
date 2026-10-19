package no.phobic.phobicccp.model

import kotlinx.serialization.Serializable

/** A phobiCCCP found on the network (or typed in), as its discovery answer says. */
data class Desktop(
    /** This install of phobiCCCP; two computers with the same name still differ. */
    val id: String,
    val host: String,
    val address: String,
    val port: Int,
    val version: String = "",
    /** The active machine's name. */
    val machine: String = "",
    val lastSeen: Long = 0,
    /** The name or address it was added by, when added by hand (Tailscale, another subnet). */
    val typed: String = "",
) {
    val manual get() = typed.isNotEmpty()

    fun online(now: Long) = now - lastSeen < ONLINE_MS

    companion object {
        const val ONLINE_MS = 15_000L
    }
}

/** The answer to "info". */
@Serializable
data class DesktopInfo(
    val ok: Boolean = false,
    val app: String = "",
    val version: String = "",
    val host: String = "",
    val id: String = "",
    val machines: List<MachineInfo> = emptyList(),
    val activeMachine: String = "",
    val status: MachineStatus = MachineStatus(),
    val allowStart: Boolean = false,
    val document: String = "",
    val unsaved: Boolean = false,
)

@Serializable
data class MachineInfo(
    val id: String,
    val name: String,
    val active: Boolean = false,
    val size: String = "",
    val width: Double = 0.0,
    val height: Double = 0.0,
)

@Serializable
data class MachineStatus(
    val connected: Boolean = false,
    val port: String = "",
    val state: String = "",
    val homed: Boolean = false,
    val busy: Boolean = false,
    val streaming: Boolean = false,
    val job: String = "",
) {
    fun words(): String = when {
        !connected -> "Not connected"
        streaming -> "Cutting${if (job.isNotEmpty()) " $job" else ""}"
        state.startsWith("Alarm") -> "Alarm: needs Initialize"
        !homed -> "Connected, not homed"
        busy -> "Busy ($state)"
        state == "Idle" -> "Idle, ready"
        else -> state.ifEmpty { "Connected" }
    }

    /** What the desktop will check before it starts a job from the phone. */
    fun readyToCut() = connected && homed && !busy && state == "Idle"
}

/** The desktop's tool library, as "tools" answers it. */
@Serializable
data class ToolLibrary(
    val materials: List<Material> = emptyList(),
    val tools: List<Tool> = emptyList(),
) {
    fun toolsFor(material: String) = tools.filter { material in it.materials }
    fun tool(number: Int) = tools.firstOrNull { it.number == number }
    fun materialName(id: String) = materials.firstOrNull { it.id == id }?.name ?: id
}

@Serializable
data class Material(val id: String, val name: String)

@Serializable
data class Tool(
    val number: Int,
    val name: String,
    val kind: String = "",
    val diameter: Double = 0.0,
    val angle: Double = 0.0,
    val flutes: Int = 0,
    /** Materials the library has feeds and speeds for. */
    val materials: List<String> = emptyList(),
)

/** One row of "designs". */
@Serializable
data class DesignEntry(
    val id: String,
    val name: String = "",
    val modified: Long = 0,
    val deleted: Boolean = false,
)

/** The answer to "send". */
@Serializable
data class SendResult(
    val ok: Boolean = false,
    val error: String = "",
    val started: Boolean = false,
    val message: String = "",
    val notes: List<String> = emptyList(),
)
