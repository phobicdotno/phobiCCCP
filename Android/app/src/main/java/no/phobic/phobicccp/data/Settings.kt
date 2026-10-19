package no.phobic.phobicccp.data

import android.content.Context
import no.phobic.phobicccp.model.PadJson
import no.phobic.phobicccp.model.ToolLibrary
import java.io.File

/** What the app remembers between runs besides the designs. */
class Settings(context: Context) {
    private val prefs = context.getSharedPreferences("phobicccp", Context.MODE_PRIVATE)
    private val toolsFile = File(context.filesDir, "tools.json")

    /** Computers added by name or address (Tailscale, another subnet). */
    var manualHosts: Set<String>
        get() = prefs.getStringSet("manualHosts", emptySet()).orEmpty()
        set(v) = prefs.edit().putStringSet("manualHosts", v).apply()

    /** The tool library as the last phobiCCCP sent it, so it works offline. */
    var tools: ToolLibrary
        get() = runCatching { PadJson.decodeFromString(ToolLibrary.serializer(), toolsFile.readText()) }
            .getOrDefault(ToolLibrary())
        set(v) = toolsFile.writeText(PadJson.encodeToString(ToolLibrary.serializer(), v))

    /** The computer and machine last sent to, picked first next time. */
    var lastTarget: String
        get() = prefs.getString("lastTarget", "").orEmpty()
        set(v) = prefs.edit().putString("lastTarget", v).apply()
}
