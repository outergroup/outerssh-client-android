package com.example.outersshclient

// Edit these to point the app at your server.
object Config {
    const val HOST = "example.com"
    const val PORT = 22
    const val USERNAME = "you"

    // Paste an OpenSSH private key between the quotes, or leave it empty and set PASSWORD.
    val PRIVATE_KEY = """
    """.trimIndent()
    const val PASSPHRASE = ""
    const val PASSWORD = ""

    // Leave empty to trust the server's key on each connection; the app then logs
    // the fingerprint ("SHA256:...") so you can paste it here to pin it.
    const val HOST_KEY_FINGERPRINT = ""
}
