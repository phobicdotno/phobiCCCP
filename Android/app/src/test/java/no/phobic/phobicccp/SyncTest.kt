package no.phobic.phobicccp

import no.phobic.phobicccp.data.DesignStore
import no.phobic.phobicccp.model.Design
import no.phobic.phobicccp.model.DesignEntry
import no.phobic.phobicccp.sync.SyncPlan
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class SyncTest {
    @get:Rule val tmp = TemporaryFolder()

    @Test
    fun newerWinsBothWays() {
        val plan = SyncPlan.of(
            local = mapOf("a" to 5L, "b" to 10L, "c" to 7L, "only-here" to 1L),
            remote = listOf(DesignEntry("a", modified = 9), DesignEntry("b", modified = 3),
                DesignEntry("c", modified = 7), DesignEntry("only-there", modified = 2)),
        )
        assertEquals(listOf("a", "only-there"), plan.pull)
        assertEquals(listOf("b", "only-here"), plan.push)
    }

    @Test
    fun inStepMeansNothingMoves() {
        assertTrue(SyncPlan.of(mapOf("a" to 1L), listOf(DesignEntry("a", modified = 1))).isEmpty)
    }

    @Test
    fun storeStampsEditsAndKeepsDeletions() {
        val store = DesignStore(tmp.root)
        val saved = store.save(Design(id = "d1", name = "One", modified = 0), now = 100)
        assertEquals(100, saved.modified)
        // An edit within the same millisecond still counts as newer.
        assertEquals(101, store.save(saved, now = 100).modified)
        store.delete("d1", now = 200)
        assertTrue(store.get("d1")!!.deleted)
        assertTrue(store.live().isEmpty())
        assertEquals(1, store.all().size)
        // Synced copies keep their own time.
        store.put(Design(id = "d2", name = "Two", modified = 42))
        assertEquals(42, store.get("d2")!!.modified)
    }

    @Test
    fun storeRefusesPathsAsIds() {
        val store = DesignStore(tmp.root)
        store.put(Design(id = "../evil", name = "x", modified = 1))
        assertNull(store.get("../evil"))
        assertTrue(tmp.root.parentFile!!.listFiles()!!.none { it.name == "evil.json" })
    }
}
