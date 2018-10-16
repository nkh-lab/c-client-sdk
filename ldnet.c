#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "curl/curl.h"

#include "ldapi.h"
#include "ldinternal.h"

#define LD_STREAMTIMEOUT 300

struct MemoryStruct {
    char *memory;
    size_t size;
};

struct streamdata {
    int (*callback)(const char *);
    struct MemoryStruct mem;
    time_t lastdatatime;
    double lastdataamt;
};

typedef size_t (*WriteCB)(void*, size_t, size_t, void*);

static size_t
WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    mem->memory = realloc(mem->memory, mem->size + realsize + 1);
    if (mem->memory == NULL) {
        /* out of memory! */
        LDi_log(2, "not enough memory (realloc returned NULL)\n");
        return 0;
    }

    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

static size_t
StreamWriteCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    struct streamdata *streamdata = userp;
    struct MemoryStruct *mem = &streamdata->mem;

    mem->memory = realloc(mem->memory, mem->size + realsize + 1);
    if (mem->memory == NULL) {
        /* out of memory! */
        LDi_log(2, "not enough memory (realloc returned NULL)\n");
        return 0;
    }

    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    char *nl;
    nl = memchr(mem->memory, '\n', mem->size);
    if (nl) {
        size_t eaten = 0;
        while (nl) {
            *nl = 0;
            streamdata->callback(mem->memory + eaten);
            eaten = nl - mem->memory + 1;
            nl = memchr(mem->memory + eaten, '\n', mem->size - eaten);
        }
        mem->size -= eaten;
        memmove(mem->memory, mem->memory + eaten, mem->size);
    }
    return realsize;
}

static curl_socket_t
SocketCallback(void cbhandle(int), curlsocktype type, struct curl_sockaddr *addr)
{
    curl_socket_t fd = socket(addr->family, addr->socktype, addr->protocol);
    if (cbhandle) {
        LDi_log(25, "about to call connection handle callback \n");
        cbhandle(fd);
        LDi_log(25, "finished calling connection handle callback\n");
    }
    return fd;
};

/* returns false on failure, results left in clean state */
static bool
prepareShared(const char *const url, const char *const authkey, CURL **r_curl, struct curl_slist **r_headers,
    WriteCB headercb, void *const headerdata, WriteCB datacb, void *const data)
{
    CURL *const curl = curl_easy_init();

    if (!curl) {
        LDi_log(5, "curl_easy_init returned NULL\n"); goto error;
    }

    if (curl_easy_setopt(curl, CURLOPT_URL, url) != CURLE_OK) {
        LDi_log(5, "curl_easy_setopt CURLOPT_URL failed\n"); goto error;
    }

    char headerauth[256];
    if (snprintf(headerauth, sizeof(headerauth), "Authorization: %s", authkey) < 0) {
        LDi_log(5, "snprintf during Authorization header creation failed\n"); goto error;
    }

    struct curl_slist *headers = NULL;
    if (!(headers = curl_slist_append(headers, headerauth))) {
        LDi_log(5, "curl_slist_append failed for headerauth\n"); goto error;
    }

    const char *const headeragent = "User-Agent: CClient/0.1";
    if (!(headers = curl_slist_append(headers, headeragent))) {
        LDi_log(5, "curl_slist_append failed for headeragent\n"); goto error;
    }

    if (curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headercb) != CURLE_OK) {
        LDi_log(5, "curl_easy_setopt CURLOPT_HEADERFUNCTION failed\n"); goto error;
    }

    if (curl_easy_setopt(curl, CURLOPT_HEADERDATA, headerdata) != CURLE_OK) {
        LDi_log(5, "curl_easy_setopt CURLOPT_HEADERDATA failed\n"); goto error;
    }

    if (curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, datacb) != CURLE_OK) {
        LDi_log(5, "curl_easy_setopt CURLOPT_WRITEFUNCTION failed\n"); goto error;
    }

    if (curl_easy_setopt(curl, CURLOPT_WRITEDATA, data) != CURLE_OK) {
        LDi_log(5, "curl_easy_setopt CURLOPT_WRITEDATA failed\n"); goto error;
    }

    *r_curl = curl; *r_headers = headers;

    return true;

  error:
    if (curl) { curl_easy_cleanup(curl); }

    return false;
};

/*
 * record the timestamp of the last received data. if nothing has been
 * seen for a while, disconnect. this shouldn't normally happen.
 */
static int
progressinspector(void *v, double dltotal, double dlnow, double ultotal, double ulnow)
{
    struct streamdata *const streamdata = v;
    const time_t now = time(NULL);

    if (streamdata->lastdataamt != dlnow) {
        streamdata->lastdataamt = dlnow;
        streamdata->lastdatatime = now;
    }

    if (now - streamdata->lastdatatime > LD_STREAMTIMEOUT) {
        LDi_log(5, "giving up stream, too slow\n");
        return 1;
    }

    const LDClient *const client = LDClientGet();
    if (client->dead || client->offline) {
        return 1;
    }

    return 0;
}

void
LDi_cancelread(int handle)
{
    shutdown(handle, SHUT_RDWR);
}

/*
 * this function reads data and passes it to the stream callback.
 * it doesn't return except after a disconnect. (or some other failure.)
 */
void
LDi_readstream(const char *urlprefix, const char *authkey, int *response, int cbdata(const char *),
    void cbhandle(int), const char *const userjson, bool usereport)
{
    struct MemoryStruct headers; struct streamdata streamdata;
    CURL *curl = NULL; struct curl_slist *headerlist = NULL;

    memset(&headers, 0, sizeof(headers)); memset(&streamdata, 0, sizeof(streamdata));

    streamdata.callback = cbdata; streamdata.lastdatatime = time(NULL);

    char url[4096];
    if (usereport) {
        if (snprintf(url, sizeof(url), "%s/meval", urlprefix) < 0) {
            LDi_log(2, "snprintf usereport failed\n"); return;
        }
    }
    else {
        size_t b64len;
        char *const b64text = LDi_base64_encode(userjson, strlen(userjson), &b64len);

        if (!b64text) {
            LDi_log(2, "LDi_base64_encode == NULL in LDi_readstream\n"); return;
        }

        const int status = snprintf(url, sizeof(url), "%s/meval/%s", urlprefix, b64text);
        free(b64text);

        if (status < 0) {
            LDi_log(2, "snprintf !usereport failed\n"); return;
        }
    }

    if (!prepareShared(url, authkey, &curl, &headerlist, &WriteMemoryCallback, &headers, &StreamWriteCallback, &streamdata)) {
        LDi_log(10, "LDi_readstream prepareShared failed\n"); return;
    }

    if (usereport) {
        if (curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "REPORT") != CURLE_OK) {
            LDi_log(5, "curl_easy_setopt CURLOPT_CUSTOMREQUEST failed\n");
            curl_easy_cleanup(curl); return;
        }

        const char* const headermime = "Content-Type: application/json";
        if (!(headerlist = curl_slist_append(headerlist, headermime))) {
            LDi_log(5, "curl_slist_append failed for headermime\n");
            curl_easy_cleanup(curl); return;
        }

        if (curl_easy_setopt(curl, CURLOPT_POSTFIELDS, userjson) != CURLE_OK) {
            LDi_log(5, "curl_easy_setopt CURLOPT_POSTFIELDS failed\n");
            curl_easy_cleanup(curl); return;
        }
    }

    if (curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, SocketCallback) != CURLE_OK) {
        LDi_log(5, "curl_easy_setopt CURLOPT_OPENSOCKETFUNCTION failed\n");
        curl_easy_cleanup(curl); return;
    }

    if (curl_easy_setopt(curl, CURLOPT_OPENSOCKETDATA, cbhandle) != CURLE_OK) {
        LDi_log(5, "curl_easy_setopt CURLOPT_OPENSOCKETDATA failed\n");
        curl_easy_cleanup(curl); return;
    }

    if (curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerlist) != CURLE_OK) {
        LDi_log(5, "curl_easy_setopt CURLOPT_HTTPHEADER failed\n");
        curl_easy_cleanup(curl); return;
    }

    if (curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0) != CURLE_OK) {
        LDi_log(5, "curl_easy_setopt CURLOPT_NOPROGRESS failed\n");
        curl_easy_cleanup(curl); return;
    }

    if (curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, progressinspector) != CURLE_OK) {
        LDi_log(5, "curl_easy_setopt CURLOPT_PROGRESSFUNCTION failed\n");
        curl_easy_cleanup(curl); return;
    }

    if (curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, (void *)&streamdata) != CURLE_OK) {
        LDi_log(5, "curl_easy_setopt CURLOPT_PROGRESSDATA failed\n");
        curl_easy_cleanup(curl); return;
    }

    LDi_log(10, "connecting to stream %s\n", url);
    const CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        long response_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        LDi_log(10, "curl response code %d\n", (int)response_code);
        *response = response_code;
    }
    else if (res == CURLE_PARTIAL_FILE) {
        *response = -2;
    }
    else {
        *response = -1;
    }

    free(streamdata.mem.memory); free(headers.memory);

    curl_easy_cleanup(curl);
}

char *
LDi_fetchfeaturemap(const char *urlprefix, const char *authkey, int *response,
    const char *const userjson, bool usereport)
{
    struct MemoryStruct headers, data;
    CURL *curl = NULL; struct curl_slist *headerlist = NULL;

    memset(&headers, 0, sizeof(headers)); memset(&data, 0, sizeof(data));

    char url[4096];
    if (usereport) {
        if (snprintf(url, sizeof(url), "%s/msdk/evalx/user", urlprefix) < 0) {
            LDi_log(2, "snprintf usereport failed\n"); return NULL;
        }
    }
    else {
        size_t b64len;
        char *const b64text = LDi_base64_encode(userjson, strlen(userjson), &b64len);

        if (!b64text) {
            LDi_log(2, "LDi_base64_encode == NULL in LDi_fetchfeaturemap\n"); return NULL;
        }

        const int status = snprintf(url, sizeof(url), "%s/msdk/evalx/users/%s", urlprefix, b64text);
        free(b64text);

        if (status < 0) {
            LDi_log(2, "snprintf !usereport failed\n"); return NULL;
        }
    }

    if (!prepareShared(url, authkey, &curl, &headerlist, &WriteMemoryCallback, &headers, &WriteMemoryCallback, &data)) {
        LDi_log(10, "fetch_url prepareShared failed\n"); return NULL;
    }

    if (usereport) {
        if (curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "REPORT") != CURLE_OK) {
            LDi_log(5, "curl_easy_setopt CURLOPT_CUSTOMREQUEST failed\n");
            curl_easy_cleanup(curl); return NULL;
        }

        const char* const headermime = "Content-Type: application/json";
        if (!(headerlist = curl_slist_append(headerlist, headermime))) {
            LDi_log(5, "curl_slist_append failed for headermime\n");
            curl_easy_cleanup(curl); return NULL;
        }

        if (curl_easy_setopt(curl, CURLOPT_POSTFIELDS, userjson) != CURLE_OK) {
            LDi_log(5, "curl_easy_setopt CURLOPT_POSTFIELDS failed\n");
            curl_easy_cleanup(curl); return NULL;
        }
    }

    if (curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerlist) != CURLE_OK) {
        LDi_log(5, "curl_easy_setopt CURLOPT_HTTPHEADER failed\n");
        curl_easy_cleanup(curl); return NULL;
    }

    if (curl_easy_perform(curl) == CURLE_OK) {
        long response_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        *response = response_code;
    } else {
        *response = -1;
    }

    free(headers.memory);

    curl_easy_cleanup(curl);

    return data.memory;
}

void
LDi_sendevents(const char *url, const char *authkey, const char *eventdata, int *response)
{
    struct MemoryStruct headers, data;
    CURL *curl = NULL; struct curl_slist *headerlist = NULL;

    memset(&headers, 0, sizeof(headers)); memset(&data, 0, sizeof(data));

    if (!prepareShared(url, authkey, &curl, &headerlist, &WriteMemoryCallback, &headers, &WriteMemoryCallback, &data)) {
        LDi_log(10, "post_data prepareShared failed\n"); return;
    }

    const char* const headermime = "Content-Type: application/json";
    if (!(headerlist = curl_slist_append(headerlist, headermime))) {
        LDi_log(5, "curl_slist_append failed for headermime\n");
        curl_easy_cleanup(curl); return;
    }

    const char* const headerschema = "X-LaunchDarkly-Event-Schema: 3";
    if (!(headerlist = curl_slist_append(headerlist, headerschema))) {
        LDi_log(5, "curl_slist_append failed for headerschema\n");
        curl_easy_cleanup(curl); return;
    }

    if (curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerlist) != CURLE_OK) {
        LDi_log(5, "curl_easy_setopt CURLOPT_HTTPHEADER failed\n");
        curl_easy_cleanup(curl); return;
    }

    if (curl_easy_setopt(curl, CURLOPT_POSTFIELDS, eventdata) != CURLE_OK) {
        LDi_log(5, "curl_easy_setopt CURLOPT_POSTFIELDS failed\n");
        curl_easy_cleanup(curl); return;
    }

    if (curl_easy_perform(curl) == CURLE_OK) {
        long response_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        *response = response_code;
    } else {
        *response = -1;
    }

    free(data.memory); free(headers.memory);

    curl_easy_cleanup(curl);
}
