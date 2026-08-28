#include "ui/net.h"

#include <curl/curl.h>
#include <inkview.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/abs_api.h"
#include "ui/log.h"

#define ABS_HTTP_TIMEOUT_S      30L
#define ABS_HTTP_CONNECT_TIME_S 15L

typedef struct {
    abs_http_response *res;
    size_t max_bytes;
    int    overflowed;
} write_ctx;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    write_ctx *ctx = (write_ctx *)userdata;
    size_t incoming = size * nmemb;

    if (ctx->res->len + incoming > ctx->max_bytes) {
        ctx->overflowed = 1;
        return 0;  /* aborts the transfer */
    }

    char *grown = realloc(ctx->res->data, ctx->res->len + incoming + 1);
    if (grown == NULL) return 0;

    memcpy(grown + ctx->res->len, ptr, incoming);
    ctx->res->data = grown;
    ctx->res->len += incoming;
    ctx->res->data[ctx->res->len] = '\0';

    return incoming;
}

int abs_net_connect(char *err, size_t err_size)
{
    if (QueryNetwork() & NET_CONNECTED) {
        abs_log("network already up");
        return 1;
    }

    abs_log("dialling network");
    int rc = NetConnect(NULL);
    if (rc != NET_OK) {
        const char *msg = NetError(rc);
        abs_log("NetConnect failed: %d (%s)", rc, msg ? msg : "?");
        if (err != NULL && err_size > 0) {
            snprintf(err, err_size, "%s", msg ? msg : "Could not connect to Wi-Fi.");
        }
        return 0;
    }

    abs_log("network up");
    return 1;
}

/*
 * One request path for every verb, so timeouts, TLS policy, redirect limits
 * and the response cap cannot drift apart between GET and POST.
 */
static int http_request(const abs_config *cfg, const char *url, const char *body,
                        const char *bearer, abs_http_response *res, size_t max_bytes)
{
    memset(res, 0, sizeof *res);

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        snprintf(res->error, sizeof res->error, "Could not initialise HTTP client.");
        return 0;
    }

    char auth[ABS_MAX_TOKEN + 64];
    struct curl_slist *headers = NULL;
    if (bearer != NULL && bearer[0] != '\0' &&
        abs_auth_header_token(bearer, auth, sizeof auth) > 0) {
        headers = curl_slist_append(headers, auth);
    }
    headers = curl_slist_append(headers, "Accept: application/json");
    if (body != NULL) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
    }

    write_ctx ctx = { res, max_bytes, 0 };

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, ABS_HTTP_TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, ABS_HTTP_CONNECT_TIME_S);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ABSClient-PocketBook/0.1");

    if (body != NULL) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    }

    if (cfg->insecure) {
        /* Opt-in, for a self-signed certificate on a server the user controls. */
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    abs_log("%s %s", body ? "POST" : "GET", url);   /* body is never logged */
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res->status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        if (ctx.overflowed) {
            snprintf(res->error, sizeof res->error, "Response too large.");
        } else if (rc == CURLE_SSL_CACERT || rc == CURLE_PEER_FAILED_VERIFICATION) {
            /* Overwhelmingly the self-hosted case: name the fix, not the errno. */
            snprintf(res->error, sizeof res->error,
                     "Certificate not trusted. For a self-signed server, set "
                     "insecure=1 in abs_client.cfg.");
        } else {
            snprintf(res->error, sizeof res->error, "%s", curl_easy_strerror(rc));
        }
        abs_log("curl failed: %d (%s)", rc, res->error);
        abs_http_free(res);
        return 0;
    }

    abs_log("HTTP %ld, %lu bytes", res->status, (unsigned long)res->len);
    return 1;
}

int abs_http_get(const abs_config *cfg, const char *url,
                 abs_http_response *res, size_t max_bytes)
{
    return http_request(cfg, url, NULL, cfg->token, res, max_bytes);
}

int abs_http_get_auth(const abs_config *cfg, const char *url, const char *bearer,
                      abs_http_response *res, size_t max_bytes)
{
    return http_request(cfg, url, NULL, bearer, res, max_bytes);
}

int abs_http_post_json(const abs_config *cfg, const char *url, const char *body,
                       const char *bearer, abs_http_response *res, size_t max_bytes)
{
    return http_request(cfg, url, body, bearer, res, max_bytes);
}

void abs_http_free(abs_http_response *res)
{
    free(res->data);
    res->data = NULL;
    res->len = 0;
}

const char *abs_http_status_message(long status)
{
    if (status == 401 || status == 403) {
        return "Server rejected the API key.";
    }
    if (status == 404) {
        return "Not found. Check the server address.";
    }
    if (status >= 500) {
        return "The server reported an error.";
    }
    if (status >= 400) {
        return "The server refused the request.";
    }
    return NULL;
}
