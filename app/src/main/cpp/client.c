#include <OuterSSH/OuterSSH.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <android/looper.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define LOG_TAG "OuterSSHClient"

enum {
    TAG_PROXY_STARTED = 1,
    TAG_HOMEPAGE,
    TAG_FIND_OUTER_SHELL,
    TAG_DESTROY,
};

enum { URL_CAPACITY = 4096 };

static const char find_outer_shell[] =
    "if [ \"$(id -u)\" = 0 ]; then d=/run; else d=\"${XDG_RUNTIME_DIR:-/run/user/$(id -u)}\"; fi; "
    "p=\"$d/org.outershell.OuterShell\"; [ -S \"$p\" ] && printf '%s' \"$p\"";

typedef struct {
    JavaVM *vm;
    jobject listener, assets;
    AAssetManager *asset_manager;
    jmethodID proxy_started, load, show;
    OuterSSHEventQueue *event_queue;
    OuterSSHSessionPool *pool;
    OuterSSHHTTPProxyServer *proxy;
    int pending_destroys;
    bool pinned_fingerprint;
    bool closing;
} Client;

static jbyteArray byte_array(JNIEnv *env, const void *bytes, size_t length)
{
    jbyteArray array = (*env)->NewByteArray(env, (jsize)length);
    if (array && length) (*env)->SetByteArrayRegion(env, array, 0, (jsize)length, bytes);
    return array;
}

static void call(Client *client, jmethodID method, const char *text, size_t length)
{
    JNIEnv *env;
    if ((*client->vm)->GetEnv(client->vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return;
    jbyteArray array = byte_array(env, text, length);
    if (array) (*env)->CallVoidMethod(env, client->listener, method, array);
    (*env)->DeleteLocalRef(env, array);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

static void load(Client *client, const char *url) { call(client, client->load, url, strlen(url)); }
static void show(Client *client, const char *text) { call(client, client->show, text, strlen(text)); }

static void show_error(Client *client, const char *prefix, OuterSSHError error)
{
    char text[1024];
    snprintf(text, sizeof(text), "%s%.*s", prefix, (int)error.message_length,
        error.message ? error.message : "");
    show(client, text);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool percent_decode(const char *text, size_t length, char *output, size_t capacity, size_t *output_length)
{
    size_t out = 0;
    for (size_t i = 0; i < length; i++) {
        char c = text[i];
        if (c == '%') {
            if (i + 2 >= length) return false;
            int high = hex_value(text[i + 1]), low = hex_value(text[i + 2]);
            if (high < 0 || low < 0) return false;
            c = (char)(high << 4 | low);
            i += 2;
        }
        if (out + 1 >= capacity) return false;
        output[out++] = c;
    }
    output[out] = 0;
    *output_length = out;
    return true;
}

/* Converts the http+unix and http://localhost homepages this starter supports
 * into URLs the OuterSSH proxy understands. */
static bool convert_homepage(const char *url, size_t length, char *output, size_t capacity)
{
    static const char unix_scheme[] = "http+unix://", http_scheme[] = "http://";
    bool socket;
    size_t start;
    if (length > strlen(unix_scheme) && !strncasecmp(url, unix_scheme, strlen(unix_scheme))) {
        socket = true;
        start = strlen(unix_scheme);
    } else if (length > strlen(http_scheme) && !strncasecmp(url, http_scheme, strlen(http_scheme))) {
        socket = false;
        start = strlen(http_scheme);
    } else {
        return false;
    }
    for (size_t i = 0; i < length; i++)
        if ((unsigned char)url[i] <= ' ' || url[i] == 127) return false;
    size_t end = start;
    while (end < length && url[end] != '/' && url[end] != '?' && url[end] != '#') end++;
    const char *authority = url + start, *rest = url + end;
    size_t authority_length = end - start, rest_length = length - end;
    if (authority_length == 0 || memchr(authority, '@', authority_length)) return false;
    const char *slash = rest_length && rest[0] == '/' ? "" : "/";

    char host[OUTERSSH_PROXY_SOCKET_HOST_CAPACITY];
    size_t host_length;
    const char *port = "";
    size_t port_length = 0;
    if (socket) {
        char path[256];
        size_t path_length;
        if (!percent_decode(authority, authority_length, path, sizeof(path), &path_length) ||
            !outerssh_proxy_encode_unix_socket_host(path, path_length, host, sizeof(host), &host_length)) {
            return false;
        }
    } else {
        size_t name_length = authority_length;
        const char *closing = authority[0] == '[' ? memchr(authority, ']', authority_length) : NULL;
        const char *colon = memchr(closing ? closing : authority, ':',
            authority_length - (size_t)((closing ? closing : authority) - authority));
        if (colon) {
            name_length = (size_t)(colon - authority);
            port = colon;
            port_length = authority_length - name_length;
            long number = 0;
            for (size_t i = 1; i < port_length; i++) {
                if (port[i] < '0' || port[i] > '9' || (number = number * 10 + (port[i] - '0')) > 65535) return false;
            }
            if (port_length < 2 || number == 0) return false;
        }
        if (!outerssh_proxy_encode_tcp_host(authority, name_length, host, sizeof(host), &host_length)) return false;
    }
    int written = snprintf(output, capacity, "http://%.*s%.*s%s%.*s", (int)host_length, host,
        (int)port_length, port, slash, (int)rest_length, rest);
    return written > 0 && (size_t)written < capacity;
}

static void homepage_received(Client *client, const OuterSSHEvent *event)
{
    if (event->homepage_received.error.domain != OUTERSSH_ERROR_NONE) {
        show_error(client, "Couldn't read the server's homepage: ", event->homepage_received.error);
    } else if (event->homepage_received.found) {
        char url[URL_CAPACITY];
        if (convert_homepage(event->homepage_received.url, event->homepage_received.url_length, url, sizeof(url))) {
            load(client, url);
        } else {
            char text[URL_CAPACITY + 64];
            snprintf(text, sizeof(text), "This starter doesn't support the homepage %.*s.",
                (int)event->homepage_received.url_length, event->homepage_received.url);
            show(client, text);
        }
    } else if (!outerssh_session_pool_execute_command(client->pool, find_outer_shell,
                   sizeof(find_outer_shell) - 1, NULL, 0, TAG_FIND_OUTER_SHELL)) {
        show(client, "Couldn't look for Outer Shell.");
    }
}

static void outer_shell_found(Client *client, const OuterSSHEvent *event)
{
    static const char suffix[] = "/org.outershell.OuterShell";
    const char *path = (const char *)event->command_completed.standard_output;
    size_t length = event->command_completed.standard_output_length;
    char host[OUTERSSH_PROXY_SOCKET_HOST_CAPACITY];
    size_t host_length;
    if (event->command_completed.error.domain != OUTERSSH_ERROR_NONE) {
        show_error(client, "Couldn't look for Outer Shell: ", event->command_completed.error);
    } else if (event->command_completed.exit_status == 0 && path && length > sizeof(suffix) - 1 && path[0] == '/' &&
               !memcmp(path + length - (sizeof(suffix) - 1), suffix, sizeof(suffix) - 1) &&
               outerssh_proxy_encode_unix_socket_host(path, length, host, sizeof(host), &host_length)) {
        char url[URL_CAPACITY];
        snprintf(url, sizeof(url), "http://%.*s/", (int)host_length, host);
        load(client, url);
    } else {
        show(client, "Outer Shell isn't running on this server.");
    }
}

static void proxy_started(Client *client, const OuterSSHEvent *event)
{
    if (event->proxy_started.error.domain != OUTERSSH_ERROR_NONE) {
        show_error(client, "Couldn't start the HTTP proxy: ", event->proxy_started.error);
        return;
    }
    JNIEnv *env;
    if ((*client->vm)->GetEnv(client->vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return;
    jbyteArray username = byte_array(env, event->proxy_started.username, event->proxy_started.username_length);
    jbyteArray password = byte_array(env, event->proxy_started.password, event->proxy_started.password_length);
    if (username && password) {
        (*env)->CallVoidMethod(env, client->listener, client->proxy_started,
            (jint)event->proxy_started.listen_port, username, password);
    }
    (*env)->DeleteLocalRef(env, username);
    (*env)->DeleteLocalRef(env, password);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

static void validate_host_key(Client *client, const OuterSSHEvent *event, bool pinned)
{
    int length = (int)event->host_key_validation.fingerprint_length;
    const char *fingerprint = event->host_key_validation.fingerprint;
    if (pinned) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
            "Rejecting host key %.*s, which doesn't match HOST_KEY_FINGERPRINT in Config.kt.", length, fingerprint);
    } else {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG,
            "Trusting host key %.*s. Paste it into HOST_KEY_FINGERPRINT in Config.kt to pin it.", length, fingerprint);
    }
    outerssh_session_pool_answer_host_key(client->pool, event->host_key_validation.question, !pinned);
}

/* Answers OuterSSH's request for its server-side helper with an asset built from
 * the repository's helpers/ folder. */
static void provide_helper(Client *client, const OuterSSHEvent *event)
{
    char name[128];
    snprintf(name, sizeof(name), "outerssh-helper-%.*s.gz.bin",
        (int)event->helper_binary_needed.platform_length, event->helper_binary_needed.platform);
    AAsset *asset = AAssetManager_open(client->asset_manager, name, AASSET_MODE_UNKNOWN);
    off64_t start = 0, length = 0;
    int file = asset ? AAsset_openFileDescriptor64(asset, &start, &length) : -1;
    if (asset) AAsset_close(asset);
    if (file >= 0) {
        outerssh_session_pool_provide_helper_binary(client->pool, event->helper_binary_needed.question,
            file, (uint64_t)start, (uint64_t)length);
    } else {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
            "Missing asset %s; this server's sockets need it. See README.md.", name);
        outerssh_session_pool_decline_helper_binary(client->pool, event->helper_binary_needed.question,
            (OuterSSHError){0});
    }
}

static void destroy_client(Client *client)
{
    JNIEnv *env;
    ALooper_removeFd(ALooper_forThread(), outerssh_event_queue_wait_handle(client->event_queue));
    outerssh_event_queue_destroy(client->event_queue);
    if ((*client->vm)->GetEnv(client->vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK) {
        (*env)->DeleteGlobalRef(env, client->listener);
        (*env)->DeleteGlobalRef(env, client->assets);
    }
    free(client);
}

static int drain_events(int fd, int events, void *data)
{
    (void)fd;
    (void)events;
    Client *client = data;
    OuterSSHEvent batch[8];
    size_t count;
    while ((count = outerssh_event_queue_poll(client->event_queue, batch, 8)) != 0) {
        for (size_t i = 0; i < count; i++) {
            const OuterSSHEvent *event = &batch[i];
            if (event->type == OUTERSSH_EVENT_DESTROYED) {
                if (--client->pending_destroys == 0) {
                    destroy_client(client);
                    return 0;
                }
                continue;
            }
            if (client->closing) continue;
            switch (event->type) {
            case OUTERSSH_EVENT_PROXY_STARTED:
                proxy_started(client, event);
                break;
            case OUTERSSH_EVENT_HOMEPAGE_RECEIVED:
                homepage_received(client, event);
                break;
            case OUTERSSH_EVENT_COMMAND_COMPLETED:
                outer_shell_found(client, event);
                break;
            case OUTERSSH_EVENT_HOST_KEY_VALIDATION:
                validate_host_key(client, event, client->pinned_fingerprint);
                break;
            case OUTERSSH_EVENT_SUDO_PASSWORD_NEEDED:
                outerssh_session_pool_decline_sudo_password(client->pool, event->sudo_password_needed.question);
                break;
            case OUTERSSH_EVENT_HELPER_BINARY_NEEDED:
                provide_helper(client, event);
                break;
            case OUTERSSH_EVENT_CONNECTION_FAILED:
                show_error(client, "Connection failed: ", event->connection_failed.error);
                break;
            default:
                break;
            }
        }
    }
    return 1;
}

typedef struct {
    jbyteArray array;
    jbyte *bytes;
    size_t length;
} Bytes;

static Bytes get_bytes(JNIEnv *env, jbyteArray array)
{
    Bytes result = {array, NULL, 0};
    if (!array) return result;
    result.length = (size_t)(*env)->GetArrayLength(env, array);
    if (result.length) result.bytes = (*env)->GetByteArrayElements(env, array, NULL);
    return result;
}

static void release_bytes(JNIEnv *env, Bytes *bytes)
{
    if (!bytes->bytes) return;
    memset(bytes->bytes, 0, bytes->length);
    (*env)->ReleaseByteArrayElements(env, bytes->array, bytes->bytes, 0);
}

JNIEXPORT jlong JNICALL Java_com_example_outersshclient_OuterSSHClient_start(JNIEnv *env, jobject listener,
    jobject assets, jbyteArray host_array, jint port, jbyteArray username_array, jbyteArray private_key_array,
    jbyteArray passphrase_array, jbyteArray password_array, jbyteArray fingerprint_array)
{
    Client *client = calloc(1, sizeof(*client));
    if (!client) return 0;
    jclass class = (*env)->GetObjectClass(env, listener);
    (*env)->GetJavaVM(env, &client->vm);
    client->proxy_started = (*env)->GetMethodID(env, class, "proxyStarted", "(I[B[B)V");
    client->load = (*env)->GetMethodID(env, class, "load", "([B)V");
    client->show = (*env)->GetMethodID(env, class, "show", "([B)V");
    client->listener = (*env)->NewGlobalRef(env, listener);
    client->assets = (*env)->NewGlobalRef(env, assets);
    client->asset_manager = AAssetManager_fromJava(env, client->assets);
    client->event_queue = outerssh_event_queue_create();
    if (!client->event_queue || ALooper_addFd(ALooper_forThread(), outerssh_event_queue_wait_handle(client->event_queue),
            ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, drain_events, client) != 1) {
        show(client, "Couldn't create the OuterSSH event queue.");
        outerssh_event_queue_destroy(client->event_queue);
        (*env)->DeleteGlobalRef(env, client->listener);
        (*env)->DeleteGlobalRef(env, client->assets);
        free(client);
        return 0;
    }

    Bytes host = get_bytes(env, host_array), username = get_bytes(env, username_array);
    Bytes private_key = get_bytes(env, private_key_array), passphrase = get_bytes(env, passphrase_array);
    Bytes password = get_bytes(env, password_array), fingerprint = get_bytes(env, fingerprint_array);
    client->pinned_fingerprint = fingerprint.length > 0;
    OuterSSHSessionPoolConfiguration configuration = {
        .hostname = (const char *)host.bytes, .hostname_length = host.length,
        .username = (const char *)username.bytes, .username_length = username.length,
        .port = (uint16_t)port,
        .authentication = private_key.length ? (OuterSSHSessionAuthentication){
            .kind = OUTERSSH_SESSION_AUTHENTICATION_PRIVATE_KEY,
            .private_key = private_key.bytes, .private_key_length = private_key.length,
            .passphrase = (const char *)passphrase.bytes, .passphrase_length = passphrase.length,
        } : (OuterSSHSessionAuthentication){
            .kind = OUTERSSH_SESSION_AUTHENTICATION_PASSWORD,
            .password = password.bytes ? (const char *)password.bytes : "", .password_length = password.length,
        },
        .trusted_host_key_fingerprint = (const char *)fingerprint.bytes,
        .trusted_host_key_fingerprint_length = fingerprint.length,
    };
    OuterSSHError error = {0};
    client->pool = outerssh_session_pool_create(client->event_queue, &configuration, &error);
    release_bytes(env, &host);
    release_bytes(env, &username);
    release_bytes(env, &private_key);
    release_bytes(env, &passphrase);
    release_bytes(env, &password);
    release_bytes(env, &fingerprint);
    if (client->pool) {
        OuterSSHHTTPProxyServerConfiguration proxy_configuration = {
            .authentication = OUTERSSH_PROXY_AUTHENTICATION_PASSWORD,
            .rewrite_loopback_addresses = true,
        };
        client->proxy = outerssh_http_proxy_server_create(client->pool, &proxy_configuration, &error);
    }
    if (!client->pool || !client->proxy) {
        show_error(client, "Couldn't start OuterSSH: ", error);
    } else if (!outerssh_http_proxy_server_start(client->proxy, 0, TAG_PROXY_STARTED)) {
        show(client, "Couldn't start the HTTP proxy.");
    }
    return (jlong)(intptr_t)client;
}

JNIEXPORT void JNICALL Java_com_example_outersshclient_OuterSSHClient_home(JNIEnv *env, jobject listener, jlong handle)
{
    (void)env;
    (void)listener;
    Client *client = (Client *)(intptr_t)handle;
    if (!client->pool || !outerssh_session_pool_get_homepage(client->pool, TAG_HOMEPAGE)) {
        show(client, "Couldn't ask the server for its homepage.");
    }
}

JNIEXPORT void JNICALL Java_com_example_outersshclient_OuterSSHClient_close(JNIEnv *env, jobject listener, jlong handle)
{
    (void)env;
    (void)listener;
    Client *client = (Client *)(intptr_t)handle;
    client->closing = true;
    if (client->proxy && outerssh_http_proxy_server_destroy(client->proxy, TAG_DESTROY)) client->pending_destroys++;
    if (client->pool && outerssh_session_pool_destroy(client->pool, TAG_DESTROY)) client->pending_destroys++;
    if (client->pending_destroys == 0) destroy_client(client);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;
    return outerssh_android_initialize(vm) ? JNI_VERSION_1_6 : JNI_ERR;
}
