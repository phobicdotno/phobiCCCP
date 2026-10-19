package no.phobic.phobicccp.sync

import no.phobic.phobicccp.data.DesignStore
import no.phobic.phobicccp.model.Desktop
import no.phobic.phobicccp.model.DesktopInfo
import no.phobic.phobicccp.model.ToolLibrary
import no.phobic.phobicccp.net.CccpConnection

/** One round with one phobiCCCP: its status, its tool library, and the designs both ways. */
data class SyncResult(val info: DesktopInfo, val tools: ToolLibrary?, val pulled: Int, val pushed: Int)

object Syncer {
    /** [withTools]: fetch the tool library too (it seldom changes, and it is the big answer). */
    fun syncWith(desktop: Desktop, store: DesignStore, withTools: Boolean = true): SyncResult =
        CccpConnection(desktop.address, desktop.port).use { c ->
            val info = c.info()
            val tools = if (withTools) c.tools() else null
            val plan = SyncPlan.of(store.all().associate { it.id to it.modified }, c.designs())
            var pulled = 0
            for (id in plan.pull) {
                val d = c.get(id)
                // It may have changed here meanwhile; the newer one still wins.
                if (d.modified > (store.get(id)?.modified ?: Long.MIN_VALUE)) {
                    store.put(d)
                    pulled++
                }
            }
            var pushed = 0
            for (id in plan.push) {
                val d = store.get(id) ?: continue
                if (c.put(d)) pushed++
            }
            SyncResult(info, tools, pulled, pushed)
        }
}
