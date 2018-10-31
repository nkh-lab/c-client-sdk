#include <stdlib.h>
#include <stdio.h>
#ifndef _WINDOWS
#include <unistd.h>
#else
#endif
#include <math.h>

#include "ldapi.h"
#include "ldinternal.h"

/*
 * all the code that runs in the background here.
 * plus the server event parser and streaming update handler.
 */

ld_thread_t LDi_eventthread;
ld_thread_t LDi_pollingthread;
ld_thread_t LDi_streamingthread;
ld_cond_t LDi_bgeventcond = LD_COND_INIT;
ld_cond_t LDi_bgpollcond = LD_COND_INIT;
ld_cond_t LDi_bgstreamcond = LD_COND_INIT;
ld_mutex_t LDi_condmtx;

#ifdef _WINDOWS
#define THREAD_RETURN DWORD WINAPI
#define THREAD_RETURN_DEFAULT 0
#else
#define THREAD_RETURN void *
#define THREAD_RETURN_DEFAULT NULL
#endif

static THREAD_RETURN
bgeventsender(void *v)
{
    LDClient *const client = v;

    while (true) {
        LDi_rdlock(&LDi_clientlock);
        int ms = 30000;
        if (client->config) {
            ms = client->config->eventsFlushIntervalMillis;
        }
        LDi_rdunlock(&LDi_clientlock);

        LDi_log(LD_LOG_TRACE, "bg sender sleeping\n");
        LDi_mtxenter(&LDi_condmtx);
        LDi_condwait(&LDi_bgeventcond, &LDi_condmtx, ms);
        LDi_mtxleave(&LDi_condmtx);
        LDi_log(LD_LOG_TRACE, "bgsender running\n");

        char *const eventdata = LDi_geteventdata();
        if (!eventdata) { continue; }

        LDi_rdlock(&LDi_clientlock);
        if (client->dead || client->offline) {
            LDi_rdunlock(&LDi_clientlock);
            continue;
        }

        bool sent = false;
        int retries = 0;
        while (!sent) {
            char url[4096];
            if (snprintf(url, sizeof(url), "%s/mobile", client->config->eventsURI) < 0) {
                LDi_log(LD_LOG_CRITICAL, "snprintf config->eventsURI failed\n");
                client->dead = true;
                LDi_updatestatus(client, 0);
                LDi_rdunlock(&LDi_clientlock);
                return THREAD_RETURN_DEFAULT;
            }

            char authkey[256];
            if (snprintf(authkey, sizeof(authkey), "%s", client->config->mobileKey) < 0) {
                LDi_log(LD_LOG_CRITICAL, "snprintf config->mobileKey failed\n");
                client->dead = true;
                LDi_updatestatus(client, 0);
                LDi_rdunlock(&LDi_clientlock);
                return THREAD_RETURN_DEFAULT;
            }

            LDi_rdunlock(&LDi_clientlock);
            /* unlocked while sending; will relock if retry needed */
            int response = 0;
            LDi_sendevents(url, authkey, eventdata, &response);
            if (response == 401 || response == 403) {
                sent = true; /* consider it done */
                retries = 0;
                LDi_wrlock(&LDi_clientlock);
                client->dead = true;
                LDi_updatestatus(client, 0);
                LDi_wrunlock(&LDi_clientlock);
            } else if (response == -1) {
                retries++;
            } else {
                sent = true;
                retries = 0;
            }
            if (retries) {
                unsigned int rng = 0;
                if (!LDi_random(&rng)) {
                    LDi_log(LD_LOG_CRITICAL, "rng failed in bgeventsender\n");
                }

                int backoff = 1000 * pow(2, retries - 2);
                backoff += rng % backoff;
                if (backoff > 3600 * 1000) {
                    backoff = 3600 * 1000;
                    retries--; /* avoid excessive incrementing */
                }

                LDi_millisleep(backoff);
                LDi_rdlock(&LDi_clientlock);
            }
        }
        free(eventdata);
    }
}

/*
 * this thread always runs, even when using streaming, but then it just sleeps
 */
static THREAD_RETURN
bgfeaturepoller(void *v)
{
    LDClient *const client = v;

    while (true) {
        LDi_rdlock(&LDi_clientlock);
        /*
         * the logic here is a bit tangled. we start with a default ms poll interval.
         * we skip polling if the client is dead or offline.
         * if we have a config, we revise those values.
         */
        int ms = 3000000;
        bool skippolling = client->dead || client->offline;
        if (client->config) {
            ms = client->config->pollingIntervalMillis;
            if (client->background) {
                ms = client->config->backgroundPollingIntervalMillis;
                skippolling = skippolling || client->config->disableBackgroundUpdating;
            } else {
                skippolling = skippolling || client->config->streaming;
            }
        }
        /* this triggers the first time the thread runs, so we don't have to wait */
        if (!skippolling && !client->isinit) { ms = 0; }
        LDi_rdunlock(&LDi_clientlock);

        if (ms > 0) {
            LDi_mtxenter(&LDi_condmtx);
            LDi_condwait(&LDi_bgpollcond, &LDi_condmtx, ms);
            LDi_mtxleave(&LDi_condmtx);
        }
        if (skippolling) { continue; }

        LDi_rdlock(&LDi_clientlock);
        if (client->dead) {
            LDi_rdunlock(&LDi_clientlock);
            continue;
        }

        const bool usereport = client->config->useReport;

        char url[4096];
        if (snprintf(url, sizeof(url), "%s", client->config->appURI) < 0) {
            LDi_log(LD_LOG_CRITICAL, "snprintf config->appURI failed\n");
            client->dead = true;
            LDi_updatestatus(client, 0);
            LDi_rdunlock(&LDi_clientlock);
            return THREAD_RETURN_DEFAULT;
        }

        char authkey[256];
        if (snprintf(authkey, sizeof(authkey), "%s", client->config->mobileKey) < 0) {
            LDi_log(LD_LOG_CRITICAL, "snprintf config->mobileKey failed\n");
            client->dead = true;
            LDi_updatestatus(client, 0);
            LDi_rdunlock(&LDi_clientlock);
            return THREAD_RETURN_DEFAULT;
        }

        char *const jsonuser = LDi_usertojsontext(client, client->user, false);
        if (!jsonuser) {
            LDi_log(LD_LOG_CRITICAL, "cJSON_PrintUnformatted == NULL in onstreameventping failed\n");
            client->dead = true;
            LDi_updatestatus(client, 0);
            LDi_rdunlock(&LDi_clientlock);
            return THREAD_RETURN_DEFAULT;
        }

        LDi_rdunlock(&LDi_clientlock);

        int response = 0;
        char *const data = LDi_fetchfeaturemap(url, authkey, &response, jsonuser, usereport);
        free(jsonuser);

        if (response == 401 || response == 403) {
            LDi_wrlock(&LDi_clientlock);
            client->dead = true;
            LDi_updatestatus(client, 0);
            LDi_wrunlock(&LDi_clientlock);
        }
        if (!data) { continue; }
        if (LDi_clientsetflags(client, true, data, 1)) {
            LDi_savehash(client);
        }
        free(data);
    }
}

/* exposed for testing */
void
LDi_onstreameventput(const char *data)
{
    LDClient *const client = LDClientGet();
    if (LDi_clientsetflags(client, true, data, 1)) {
        LDi_savedata("features", client->user->key, data);
    }
}

static void
applypatch(cJSON *payload, bool isdelete)
{
    LDNode *patch = NULL;
    if (payload->type == cJSON_Object) {
        patch = LDi_jsontohash(payload, 2);
    }
    cJSON_Delete(payload);

    LDClient *const client = LDClientGet();
    LDi_wrlock(&LDi_clientlock);
    LDNode *hash = client->allFlags;
    LDNode *node, *tmp;
    HASH_ITER(hh, patch, node, tmp) {
        LDNode *res = NULL;
        HASH_FIND_STR(hash, node->key, res);
        if (res && res->version > node->version) {
            /* stale patch, skip */
            continue;
        }
        if (res) {
            HASH_DEL(hash, res);
            LDi_freenode(res);
        }
        if (!isdelete) {
            HASH_DEL(patch, node);
            HASH_ADD_KEYPTR(hh, hash, node->key, strlen(node->key), node);
        }
        for (struct listener *list = client->listeners; list; list = list->next) {
            if (strcmp(list->key, node->key) == 0) {
                list->fn(node->key, isdelete ? 1 : 0);
            }
        }
    }

    client->allFlags = hash;
    LDi_wrunlock(&LDi_clientlock);

    LDi_freehash(patch);
}

void
LDi_onstreameventpatch(const char *data)
{
    cJSON *const payload = cJSON_Parse(data);

    if (!payload) {
        LDi_log(LD_LOG_ERROR, "parsing patch failed\n");
        return;
    }
    applypatch(payload, false);
    LDi_savehash(LDClientGet());
}

void
LDi_onstreameventdelete(const char *data)
{
    cJSON *const payload = cJSON_Parse(data);

    if (!payload) {
        LDi_log(LD_LOG_ERROR, "parsing delete patch failed\n");
        return;
    }
    applypatch(payload, 1);
    LDi_savehash(LDClientGet());
}

static void
onstreameventping(void)
{
    LDClient *const client = LDClientGet();

    LDi_rdlock(&LDi_clientlock);
    if (client->dead) {
        LDi_rdunlock(&LDi_clientlock);
        return;
    }

    const bool usereport = client->config->useReport;

    char url[4096];
    if (snprintf(url, sizeof(url), "%s", client->config->appURI) < 0) {
        LDi_log(LD_LOG_CRITICAL, "snprintf config->appURI failed\n");
        client->dead = true;
        LDi_updatestatus(client, 0);
        LDi_rdunlock(&LDi_clientlock);
        return;
    }

    char authkey[256];
    if (snprintf(authkey, sizeof(authkey), "%s", client->config->mobileKey) < 0) {
        LDi_log(LD_LOG_CRITICAL, "snprintf config->mobileKey failed\n");
        client->dead = true;
        LDi_updatestatus(client, 0);
        LDi_rdunlock(&LDi_clientlock);
        return;
    }

    char *const jsonuser = LDi_usertojsontext(client, client->user, false);
    if (!jsonuser) {
        LDi_log(LD_LOG_CRITICAL, "cJSON_PrintUnformatted == NULL in onstreameventping failed\n");
        client->dead = true;
        LDi_updatestatus(client, 0);
        LDi_rdunlock(&LDi_clientlock);
        return;
    }

    LDi_rdunlock(&LDi_clientlock);

    int response = 0;
    char *const data = LDi_fetchfeaturemap(url, authkey, &response, jsonuser, usereport);
    free(jsonuser);

    if (response == 401 || response == 403) {
        LDi_wrlock(&LDi_clientlock);
        client->dead = true;
        LDi_updatestatus(client, 0);
        LDi_wrunlock(&LDi_clientlock);
    }
    if (!data) { return; }
    if (LDi_clientsetflags(client, true, data, 1)) {
        LDi_savedata("features", client->user->key, data);
    }
    free(data);
}

static int streamhandle = 0;
static bool shouldstopstreaming;

void
LDi_startstopstreaming(bool stopstreaming)
{
    shouldstopstreaming = stopstreaming;
    LDi_condsignal(&LDi_bgpollcond);
    LDi_condsignal(&LDi_bgstreamcond);
}

static void
LDi_updatehandle(int handle)
{
    LDi_wrlock(&LDi_clientlock);
    streamhandle = handle;
    LDi_wrunlock(&LDi_clientlock);
}

void
LDi_reinitializeconnection()
{
    if (streamhandle) {
        LDi_cancelread(streamhandle);
        streamhandle = 0;
    }
    LDi_condsignal(&LDi_bgpollcond);
    LDi_condsignal(&LDi_bgstreamcond);
}

/*
 * as far as event stream parsers go, this is pretty basic.
 * : -> comment gets eaten
 * event:type -> type is remembered for the next data lines
 * data:line -> line is processed according to last seen event type
 */
static int
streamcallback(const char *line)
{
    static char eventname[256] = { 0 };
    static char *databuffer = NULL;

    if (!line) {
        //should not happen from the networking side but is not fatal
        LDi_log(LD_LOG_ERROR, "streamcallback got NULL line\n");
    } else if (line[0] == ':') {
        //skip comment
    } else if (line[0] == 0) {
        if (eventname[0] == 0) {
            LDi_log(LD_LOG_WARNING, "streamcallback got dispatch but type was never set\n");
        } else if (strcmp(eventname, "ping") == 0) {
            onstreameventping();
        } else if (databuffer == NULL) {
            LDi_log(LD_LOG_WARNING, "streamcallback got dispatch but data was never set\n");
        } else if (strcmp(eventname, "put") == 0) {
            LDi_onstreameventput(databuffer);
        } else if (strcmp(eventname, "patch") == 0) {
            LDi_onstreameventpatch(databuffer);
        } else if (strcmp(eventname, "delete") == 0) {
            LDi_onstreameventdelete(databuffer);
        } else {
            LDi_log(LD_LOG_WARNING, "streamcallback unknown event name: %s\n", eventname);
        }

        free(databuffer); databuffer = NULL; eventname[0] = 0;
    } else if (strncmp(line, "data:", 5) == 0) {
        line += 5; line += line[0] == ' ';

        const bool nempty = databuffer != NULL;

        const size_t linesize = strlen(line);

        size_t currentsize = 0;
        if (nempty) { currentsize = strlen(databuffer); }

        databuffer = realloc(databuffer, linesize + currentsize + nempty + 1);

        if (nempty) { databuffer[currentsize] = '\n'; }

        memcpy(databuffer + currentsize + nempty, line, linesize);

        databuffer[currentsize + nempty + linesize] = 0;
    } else if (strncmp(line, "event:", 6) == 0) {
        line += 6; line += line[0] == ' ';

        if (snprintf(eventname, sizeof(eventname), "%s", line) < 0) {
            LDi_log(LD_LOG_CRITICAL, "snprintf failed in streamcallback type processing\n");
            return 1;
        }
    }

    if (shouldstopstreaming) {
        free(databuffer); databuffer = NULL; eventname[0] = 0;
        return 1;
    }

    return 0;
}

static THREAD_RETURN
bgfeaturestreamer(void *v)
{
    LDClient *const client = v;

    int retries = 0;
    while (true) {
        LDi_rdlock(&LDi_clientlock);

        if (client->dead || !client->config->streaming || client->offline || client->background) {
            LDi_rdunlock(&LDi_clientlock);
            int ms = 30000;
            LDi_mtxenter(&LDi_condmtx);
            LDi_condwait(&LDi_bgstreamcond, &LDi_condmtx, ms);
            LDi_mtxleave(&LDi_condmtx);
            continue;
        }

        const bool usereport = client->config->useReport;

        char url[4096];
        if (snprintf(url, sizeof(url), "%s", client->config->streamURI) < 0) {
            LDi_log(LD_LOG_CRITICAL, "snprintf config->streamURI failed\n");
            client->dead = true;
            LDi_updatestatus(client, 0);
            LDi_rdunlock(&LDi_clientlock);
            return THREAD_RETURN_DEFAULT;
        }

        char authkey[256];
        if (snprintf(authkey, sizeof(authkey), "%s", client->config->mobileKey) < 0) {
            LDi_log(LD_LOG_CRITICAL, "snprintf config->mobileKey failed\n");
            client->dead = true;
            LDi_updatestatus(client, 0);
            LDi_rdunlock(&LDi_clientlock);
            return THREAD_RETURN_DEFAULT;
        }

        char *const jsonuser = LDi_usertojsontext(client, client->user, false);
        if (!jsonuser) {
            LDi_log(LD_LOG_CRITICAL, "cJSON_PrintUnformatted == NULL in bgfeaturestreamer failed\n");
            client->dead = true;
            LDi_updatestatus(client, 0);
            LDi_rdunlock(&LDi_clientlock);
            return THREAD_RETURN_DEFAULT;
        }

        LDi_rdunlock(&LDi_clientlock);

        int response;
        /* this won't return until it disconnects */
        LDi_readstream(url, authkey, &response, streamcallback, LDi_updatehandle, jsonuser, usereport);
        free(jsonuser);

        if (response == 401 || response == 403) {
            retries = 0;
            LDi_wrlock(&LDi_clientlock);
            client->dead = true;
            LDi_updatestatus(client, 0);
            LDi_wrunlock(&LDi_clientlock);
        } else if (response == -1) {
            retries++;
        } else {
            retries = 0;
        }
        if (retries) {
            unsigned int rng = 0;
            if (!LDi_random(&rng)) {
                LDi_log(LD_LOG_CRITICAL, "rng failed in bgeventsender\n");
            }

            int backoff = 1000 * pow(2, retries - 2);
            backoff += rng % backoff;
            if (backoff > 3600 * 1000) {
                backoff = 3600 * 1000;
                retries--; /* avoid excessive incrementing */
            }

            LDi_millisleep(backoff);
        }
    }
}

void
LDi_startthreads(LDClient *client)
{
    LDi_mtxinit(&LDi_condmtx);
    LDi_createthread(&LDi_eventthread, bgeventsender, client);
    LDi_createthread(&LDi_pollingthread, bgfeaturepoller, client);
    LDi_createthread(&LDi_streamingthread, bgfeaturestreamer, client);
}
