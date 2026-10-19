package no.phobic.phobicccp.data

import no.phobic.phobicccp.model.Design
import no.phobic.phobicccp.model.PadJson
import java.io.File

/**
 * Designs on the phone: one <id>.json each in [dir]. A deleted design stays
 * as a small "deleted" record so the removal reaches every phobiCCCP too.
 */
class DesignStore(private val dir: File) {

    init {
        dir.mkdirs()
    }

    @Synchronized
    fun all(): List<Design> = dir.listFiles { f -> f.name.endsWith(".json") }.orEmpty()
        .mapNotNull { f -> runCatching { PadJson.decodeFromString(Design.serializer(), f.readText()) }.getOrNull() }
        .sortedByDescending { it.modified }

    /** The designs to show: everything not deleted, newest first. */
    fun live(): List<Design> = all().filter { !it.deleted }

    @Synchronized
    fun get(id: String): Design? = file(id)?.takeIf { it.exists() }?.let {
        runCatching { PadJson.decodeFromString(Design.serializer(), it.readText()) }.getOrNull()
    }

    /** Write [design] exactly as given (sync keeps the sender's modified time). */
    @Synchronized
    fun put(design: Design) {
        val f = file(design.id) ?: return
        val tmp = File(dir, "${design.id}.tmp")
        tmp.writeText(PadJson.encodeToString(Design.serializer(), design))
        if (!tmp.renameTo(f)) {
            f.delete()
            tmp.renameTo(f)
        }
    }

    /** An edit made here: stamped with the time, so it wins the next sync. */
    fun save(design: Design, now: Long = System.currentTimeMillis()): Design =
        design.copy(modified = maxOf(now, design.modified + 1)).also { put(it) }

    fun delete(id: String, now: Long = System.currentTimeMillis()) {
        val d = get(id) ?: return
        put(Design(id = d.id, name = d.name, deleted = true, modified = maxOf(now, d.modified + 1)))
    }

    private fun file(id: String): File? =
        if (ID.matches(id)) File(dir, "$id.json") else null

    companion object {
        // As phobiCCCP accepts them (src/padlink.cpp).
        val ID = Regex("^[A-Za-z0-9-]{1,64}$")
    }
}
