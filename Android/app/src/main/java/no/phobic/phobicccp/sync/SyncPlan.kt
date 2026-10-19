package no.phobic.phobicccp.sync

import no.phobic.phobicccp.model.DesignEntry

/**
 * What to fetch from and send to one phobiCCCP so both have the newest copy
 * of every design. Deletions travel the same way (a deleted design is just a
 * newer copy). Equal times mean the same copy: nothing moves.
 */
data class SyncPlan(val pull: List<String>, val push: List<String>) {
    val isEmpty get() = pull.isEmpty() && push.isEmpty()

    companion object {
        fun of(local: Map<String, Long>, remote: List<DesignEntry>): SyncPlan {
            val theirs = remote.associate { it.id to it.modified }
            val pull = theirs.filter { (id, t) -> t > (local[id] ?: Long.MIN_VALUE) }.keys.sorted()
            val push = local.filter { (id, t) -> t > (theirs[id] ?: Long.MIN_VALUE) }.keys.sorted()
            return SyncPlan(pull, push)
        }
    }
}
