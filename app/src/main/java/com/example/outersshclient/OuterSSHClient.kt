package com.example.outersshclient

import android.content.res.AssetManager

/** Connects to the server in [Config] and runs an HTTP proxy through it. Callbacks arrive on the main thread. */
class OuterSSHClient(private val assets: AssetManager, private val listener: Listener) : AutoCloseable {
    interface Listener {
        fun onProxyStarted(port: Int, username: String, password: String)
        fun onLoad(url: String)
        fun onMessage(message: String)
    }

    private var handle = 0L

    fun start() {
        handle = start(assets, Config.HOST.toByteArray(), Config.PORT, Config.USERNAME.toByteArray(),
            Config.PRIVATE_KEY.toByteArray(), Config.PASSPHRASE.toByteArray(), Config.PASSWORD.toByteArray(),
            Config.HOST_KEY_FINGERPRINT.toByteArray())
    }

    /** Loads the server's homepage, or Outer Shell when none is configured. */
    fun home() {
        if (handle != 0L) home(handle)
    }

    override fun close() {
        if (handle != 0L) close(handle)
        handle = 0L
    }

    private fun proxyStarted(port: Int, username: ByteArray, password: ByteArray) =
        listener.onProxyStarted(port, String(username), String(password))
    private fun load(url: ByteArray) = listener.onLoad(String(url))
    private fun show(message: ByteArray) = listener.onMessage(String(message))

    private external fun start(assets: AssetManager, host: ByteArray, port: Int, username: ByteArray, privateKey: ByteArray,
        passphrase: ByteArray, password: ByteArray, fingerprint: ByteArray): Long
    private external fun home(handle: Long)
    private external fun close(handle: Long)

    companion object {
        init { System.loadLibrary("client") }
    }
}
