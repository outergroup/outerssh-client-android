package com.example.outersshclient

import android.app.Activity
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.text.TextUtils
import android.view.View
import android.view.WindowInsets
import android.view.inputmethod.EditorInfo
import android.webkit.HttpAuthHandler
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.Button
import android.widget.EditText
import androidx.webkit.ProxyConfig
import androidx.webkit.ProxyController
import androidx.webkit.WebViewCompat
import androidx.webkit.WebViewFeature

class MainActivity : Activity(), OuterSSHClient.Listener {
    private val client by lazy { OuterSSHClient(assets, this) }
    private lateinit var webView: WebView
    private var proxyCredentials: Pair<String, String>? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        findViewById<View>(R.id.root).setOnApplyWindowInsetsListener { view, insets ->
            val bars = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.ime())
            view.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            WindowInsets.CONSUMED
        }

        webView = findViewById(R.id.web_view)
        webView.settings.javaScriptEnabled = true
        webView.settings.domStorageEnabled = true
        // Pages served from a Unix socket see their address as a long encoded hostname.
        // OuterSSH's script shows the socket path in its place wherever the page displays it.
        if (WebViewFeature.isFeatureSupported(WebViewFeature.DOCUMENT_START_SCRIPT)) {
            val script = assets.open("SocketHostDisplay.js").bufferedReader().use { it.readText() }
            WebViewCompat.addDocumentStartJavaScript(webView, script, setOf("*"))
        }
        webView.webViewClient = object : WebViewClient() {
            override fun onReceivedHttpAuthRequest(view: WebView, handler: HttpAuthHandler, host: String, realm: String) {
                val credentials = proxyCredentials
                if (host == "127.0.0.1" && realm == "OuterSSH" && credentials != null) {
                    handler.proceed(credentials.first, credentials.second)
                } else handler.cancel()
            }
        }

        val portField = findViewById<EditText>(R.id.port)
        portField.setOnEditorActionListener { _, actionId, _ ->
            if (actionId != EditorInfo.IME_ACTION_GO) return@setOnEditorActionListener false
            val text = portField.text.toString().trim()
            val port = text.toIntOrNull()
            when {
                text.isEmpty() -> client.home()
                port != null && port in 1..65535 -> webView.loadUrl("http://localhost.tcp.outerssh.localhost:$port/")
                else -> onMessage("Enter a port number from 1 to 65535.")
            }
            true
        }
        findViewById<Button>(R.id.home).setOnClickListener { client.home() }

        if (!WebViewFeature.isFeatureSupported(WebViewFeature.PROXY_OVERRIDE)) {
            onMessage("Update Android System WebView to use its proxy override.")
        } else if (Build.VERSION.SDK_INT >= 37 && checkSelfPermission(LOCAL_NETWORK) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(LOCAL_NETWORK), 0)
        } else client.start()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (grantResults.firstOrNull() == PackageManager.PERMISSION_GRANTED) client.start()
        else onMessage("This app needs local network access to reach SSH servers on your network.")
    }

    override fun onDestroy() {
        client.close()
        if (proxyCredentials != null) ProxyController.getInstance().clearProxyOverride(mainExecutor) {}
        webView.destroy()
        super.onDestroy()
    }

    override fun onProxyStarted(port: Int, username: String, password: String) {
        proxyCredentials = username to password
        // Without removeImplicitRules, WebView would bypass the proxy for *.localhost hosts.
        val config = ProxyConfig.Builder().addProxyRule("http://127.0.0.1:$port").removeImplicitRules().build()
        ProxyController.getInstance().setProxyOverride(config, mainExecutor) { client.home() }
    }

    override fun onLoad(url: String) = webView.loadUrl(url)

    override fun onMessage(message: String) {
        val html = "<!doctype html><meta name=\"viewport\" content=\"width=device-width\">" +
            "<body style=\"font-family: sans-serif; padding: 16px\">${TextUtils.htmlEncode(message)}</body>"
        webView.loadDataWithBaseURL(null, html, "text/html", "utf-8", null)
    }

    private companion object {
        const val LOCAL_NETWORK = "android.permission.ACCESS_LOCAL_NETWORK"
    }
}
