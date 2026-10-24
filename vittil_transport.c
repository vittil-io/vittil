/*
 * vittil_transport.c: the transport. The device talks to the server;
 * everything the ping and the upload send goes through here, and this file
 * knows nothing about what it carries: the URL and the client, the network
 * guard, the small body, the project key header, the refused key and the
 * status handed back for the retry, and the streamed body (begin, write,
 * end; post_small is the three in a row). The caller's loop waits for the
 * network; nothing here blocks on it.
 * Every entry point checks its arguments and refuses with one log line.
 */
#include <string.h>
#include <stdio.h>
#include <limits.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#if CONFIG_VITTIL_TLS_CERT_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "vittil_priv.h"

static const char *TAG = "vittil";

/* The file's state, one struct. Written by the task that sends and by
 * vittil_transport_set_key before it starts. */
static struct {
    /* The project key as "Bearer <key>", sent on every request once set;
     * the server answers 401 without it. Copied, so the caller's string
     * need not outlive the call. */
    char auth[80];
    /* Set by the first 401 or 403, never cleared. A wrong key
     * is fixed only by new firmware, so from then on every send returns at
     * once with ESP_ERR_NOT_ALLOWED, nothing sent, nothing retried, until
     * the next boot. One log line when it is set, silence after. */
    bool key_refused;
    /* the request's URL, built per request */
    char url[160];
} vittil_transport;

void vittil_transport_set_key(const char *key)
{
    if (!key || !key[0]) {
        vittil_transport.auth[0] = 0;
        return;
    }
    int n = snprintf(vittil_transport.auth, sizeof(vittil_transport.auth), "Bearer %s", key);
    if (n < 0 || n >= (int)sizeof(vittil_transport.auth)) {
        ESP_LOGE(TAG, "project key too long (%d), not sent", n);
        vittil_transport.auth[0] = 0;
    }
}

bool vittil_transport_has_key(void)
{
    return vittil_transport.auth[0] != 0;
}

/* One client handle per request, POST, 15 s, JSON. ESP_ERR_INVALID_ARG
 * for a bad path or URL (our own bug, not retried); ESP_ERR_NO_MEM when the
 * client or a header could not be allocated (retried by the caller). A
 * request is never sent with a header missing: without the Authorization
 * header the server answers 401 and the key would be marked refused. */
static esp_err_t client_for(const char *path, esp_http_client_handle_t *out)
{
    char *url = vittil_transport.url;
    *out = NULL;
    if (!path || path[0] != '/') {
        ESP_LOGE(TAG, "bad path (NULL or not starting with /)");
        return ESP_ERR_INVALID_ARG;
    }
    int n = snprintf(url, sizeof(vittil_transport.url), "%s%s", CONFIG_VITTIL_SERVER_URL, path);
    if (n < 0 || n >= (int)sizeof(vittil_transport.url)) {
        ESP_LOGE(TAG, "URL too long (%d), not sent", n);
        return ESP_ERR_INVALID_ARG;
    }
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
#if CONFIG_VITTIL_TLS_CERT_BUNDLE
        /* https: verify the server against ESP-IDF's root certificate bundle.
         * Ignored for an http URL. */
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
#if !CONFIG_VITTIL_TLS_CERT_BUNDLE
    if (strncmp(url, "https:", 6) == 0) {
        ESP_LOGE(TAG, "https server URL needs CONFIG_VITTIL_TLS_CERT_BUNDLE=y");
        return ESP_ERR_INVALID_ARG;
    }
#endif
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        ESP_LOGW(TAG, "no memory for the http client, not sent");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_set_header(c, "Content-Type", "application/json");
    if (err == ESP_OK && vittil_transport.auth[0]) {
        err = esp_http_client_set_header(c, "Authorization", vittil_transport.auth);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no memory for the headers (%s), not sent", esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return ESP_ERR_NO_MEM;
    }
    *out = c;
    return ESP_OK;
}

/* Finish a request: headers, status, a short response for the log.
 * -1 when no answer came. A 2xx at debug: the caller logs what it meant
 * (the upload, the summary, the no-dump event, the ping's first of the
 * boot), so the ping every interval is not a line each time; any other
 * answer a warning with the server's words. */
static int finish(esp_http_client_handle_t c, const char *what)
{
    int status = -1;
    if (esp_http_client_fetch_headers(c) >= 0) {
        status = esp_http_client_get_status_code(c);
        char resp[128];
        int n = esp_http_client_read_response(c, resp, sizeof(resp) - 1);
        if (n < 0) {
            n = 0;
        }
        resp[n] = 0;
        if (status >= 200 && status < 300) {
            ESP_LOGD(TAG, "%s: HTTP %d %s", what, status, resp);
        } else {
            ESP_LOGW(TAG, "%s: HTTP %d %s", what, status, resp);
        }
    } else {
        ESP_LOGW(TAG, "%s: no response from %s", what, CONFIG_VITTIL_SERVER_URL);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return status;
}

/* A body of known length, written in pieces. begin opens the
 * connection with len as the Content-Length; write sends one piece; end
 * reads the answer and frees the handle. Every begin that returns ESP_OK is
 * matched by exactly one end, which frees on every path (a failed write
 * included), so the caller never frees. The refused key and the status
 * handed back are end's. */
esp_err_t vittil_post_begin(const char *path, size_t len, const char *what, vittil_post_t *p)
{
    if (!p) {
        ESP_LOGE(TAG, "post_begin: NULL post");
        return ESP_ERR_INVALID_ARG;
    }
    p->c = NULL;
    p->left = 0;
    if (!what) {
        ESP_LOGE(TAG, "post_begin: NULL name");
        return ESP_ERR_INVALID_ARG;
    }
    if (len > (size_t)INT_MAX) {
        /* the client takes the Content-Length as an int; a length past it
         * would be sent wrong */
        ESP_LOGE(TAG, "%s: body of %u bytes over the client's length, not sent", what, (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }
    if (vittil_transport.key_refused) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (!vittil_net_ready()) {
        /* no socket before the network; the caller decides when to
         * try again (the round). */
        return ESP_ERR_INVALID_STATE;
    }
    esp_http_client_handle_t c = NULL;
    esp_err_t err = client_for(path, &c);
    if (err != ESP_OK) {
        return err; /* INVALID_ARG (logged, our bug) or NO_MEM (retried) */
    }
    err = esp_http_client_open(c, (int)len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: connect failed: %s", what, esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return err;
    }
    p->c = c;
    p->left = len;
    return ESP_OK;
}

esp_err_t vittil_post_write(vittil_post_t *p, const void *data, size_t len)
{
    if (!p || !p->c || (!data && len)) {
        ESP_LOGE(TAG, "post_write: NULL post or data");
        return ESP_ERR_INVALID_ARG;
    }
    if (len > p->left) {
        ESP_LOGE(TAG, "post_write: %u bytes past the body's length", (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }
    if (len == 0) {
        return ESP_OK;
    }
    if (esp_http_client_write(p->c, data, (int)len) != (int)len) {
        return ESP_FAIL; /* the caller's end sees the body short and frees */
    }
    p->left -= len;
    return ESP_OK;
}

esp_err_t vittil_post_end(vittil_post_t *p, const char *what, int *status)
{
    if (status) {
        *status = -1;
    }
    if (!p || !p->c || !what) {
        ESP_LOGE(TAG, "post_end: NULL post or name");
        return ESP_ERR_INVALID_ARG;
    }
    esp_http_client_handle_t c = p->c;
    p->c = NULL;
    if (p->left) {
        /* a body short of its Content-Length: the server would wait for
         * the rest; closed without an answer */
        ESP_LOGW(TAG, "%s: body short by %u bytes, not finished", what, (unsigned)p->left);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }
    int st = finish(c, what);
    if (status) {
        *status = st;
    }
    if (st == 401 || st == 403) {
        vittil_transport.key_refused = true;
        ESP_LOGE(TAG, "project key refused (HTTP %d): nothing more is sent until the next boot", st);
        return ESP_ERR_NOT_ALLOWED;
    }
    return (st >= 200 && st < 300) ? ESP_OK : ESP_FAIL;
}

/* A string body, its length as Content-Length, one write: begin, write
 * and end in a row. */
esp_err_t vittil_post_small(const char *path, const char *body, const char *what, int *status)
{
    if (status) {
        *status = -1;
    }
    if (!body || !what) {
        ESP_LOGE(TAG, "post_small: NULL body or name");
        return ESP_ERR_INVALID_ARG;
    }
    vittil_post_t p;
    size_t len = strlen(body);
    esp_err_t err = vittil_post_begin(path, len, what, &p);
    if (err != ESP_OK) {
        return err;
    }
    err = vittil_post_write(&p, body, len);
    int st = -1;
    esp_err_t end = vittil_post_end(&p, what, &st); /* frees on every path */
    if (status) {
        *status = st;
    }
    return err != ESP_OK ? err : end;
}

/* The application owns the network. Until it has initialised the TCP/IP
 * stack and an interface is up, vittil must not open a socket: a lookup on a
 * stack that was never initialised asserts inside lwIP and takes the device
 * down. An interface that is up is not yet a network: Wi-Fi is up once it
 * joins, before DHCP, and a send then fails its lookup and spends a retry.
 * So an address is needed too: an IPv4 one, or an IPv6 one that is not
 * link-local; and, when the server URL names its host, a DNS server to look
 * it up (an IPv6 address can come from the router with no DNS server).
 * Plain reads, safe before esp_netif_init(); the one function that says
 * "no network", returning at once. post_small
 * checks it itself; who waits, and for how long, is the caller's loop. */
static bool has_address(esp_netif_t *netif)
{
    esp_netif_ip_info_t ip4;
    if (esp_netif_get_ip_info(netif, &ip4) == ESP_OK && ip4.ip.addr != 0) {
        return true;
    }
#if CONFIG_LWIP_IPV6
    esp_ip6_addr_t ip6[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
    int n = esp_netif_get_all_ip6(netif, ip6);
    for (int i = 0; i < n && i < CONFIG_LWIP_IPV6_NUM_ADDRESSES; i++) {
        if (esp_netif_ip6_get_addr_type(&ip6[i]) != ESP_IP6_ADDR_IS_LINK_LOCAL) {
            return true;
        }
    }
#endif
    return false;
}

/* True when the server URL's host is a name, not an IPv4 or [IPv6] literal. */
static bool host_is_name(void)
{
    const char *h = strstr(CONFIG_VITTIL_SERVER_URL, "://");
    h = h ? h + 3 : CONFIG_VITTIL_SERVER_URL;
    if (*h == '[') {
        return false;
    }
    for (; *h != '\0' && *h != ':' && *h != '/'; h++) {
        if ((*h >= 'a' && *h <= 'z') || (*h >= 'A' && *h <= 'Z')) {
            return true;
        }
    }
    return false;
}

static bool has_dns(esp_netif_t *netif)
{
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK) {
        return false;
    }
    if (dns.ip.type == ESP_IPADDR_TYPE_V4) {
        return dns.ip.u_addr.ip4.addr != 0;
    }
    const uint32_t *a = dns.ip.u_addr.ip6.addr;
    return (a[0] | a[1] | a[2] | a[3]) != 0;
}

bool vittil_net_ready(void)
{
    esp_netif_t *netif = esp_netif_get_default_netif();
    if (netif == NULL || !esp_netif_is_netif_up(netif) || !has_address(netif)) {
        return false;
    }
    return !host_is_name() || has_dns(netif);
}
