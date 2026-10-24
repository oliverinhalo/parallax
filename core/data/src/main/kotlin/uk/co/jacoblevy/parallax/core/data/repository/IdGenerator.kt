package uk.co.jacoblevy.parallax.core.data.repository

import java.util.UUID
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Supplies new project, folder and job ids.
 *
 * An interface rather than a call to [UUID.randomUUID] at each site so that tests can assert on
 * what was created without having to read the id back out first.
 */
fun interface IdGenerator {
    fun newId(): String
}

@Singleton
class UuidIdGenerator @Inject constructor() : IdGenerator {
    override fun newId(): String = UUID.randomUUID().toString()
}
