
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "ldapi.h"
#include "ldinternal.h"

void
logger(const char *const s)
{
    printf("LD: %s", s);
}

void *
fake_opener(void *const ctx, const char *const name, const char *const mode, const size_t len)
{
    return "the handle";
}

void
fake_closer(void *const handle)
{
    if (strcmp(handle, "the handle") != 0) {
        printf("ERROR: something bad happened to the handle\n");
    }
}

bool gotcallback = false;

bool
fake_stringwriter(void *const handle, const char *const data)
{
    gotcallback = true;
}

/*
 * Read flags from an external file and check for expected value.
 */
void
test1(void)
{
    LD_store_setfns(NULL, LD_store_fileopen, NULL /* no writer */, LD_store_fileread, LD_store_fileclose);

    LDConfig *const config = LDConfigNew("authkey");
    LDConfigSetOffline(config, true);

    LDUser *const user = LDUserNew("fileuser");

    LDClient *const client = LDClientInit(config, user, 0);

    char buffer[256];
    LDStringVariation(client, "filedata", "incorrect", buffer, sizeof(buffer));
    if (strcmp(buffer, "as expected") != 0) {
        printf("ERROR: didn't load file data\n");
    }

    char *patch = "{ \"key\": \"filedata\", \"value\": \"updated\", \"version\": 2 } }";
    LDi_onstreameventpatch(client, patch);
    LDStringVariation(client, "filedata", "incorrect", buffer, sizeof(buffer));
    if (strcmp(buffer, "as expected") != 0) {
        printf("ERROR: applied stale patch\n");
    }

    patch = "{ \"key\": \"filedata\", \"value\": \"updated\", \"version\": 4 } }";
    LDi_onstreameventpatch(client, patch);
    LDStringVariation(client, "filedata", "incorrect", buffer, sizeof(buffer));
    if (strcmp(buffer, "updated") != 0) {
        printf("ERROR: didn't apply good patch\n");
    }

    LDClientClose(client);

}

/*
 * Test that flags get written out after receiving an update.
 */
void
test2(void)
{
    LD_store_setfns(NULL, fake_opener, fake_stringwriter, NULL, fake_closer);

    LDConfig *const config = LDConfigNew("authkey");
    LDConfigSetOffline(config, true);

    LDUser *const user = LDUserNew("fakeuser");

    LDClient *const client = LDClientInit(config, user, 0);

    const char *const putflags = "{ \"bgcolor\": { \"value\": \"red\", \"version\": 1 } }";

    LDi_onstreameventput(client, putflags);

    if (!gotcallback) {
        printf("ERROR: flag update didn't call writer\n");
    }

    LDClientClose(client);

}

int
main(int argc, char **argv)
{
    printf("Beginning tests\n");

    LDSetLogFunction(1, logger);

    test1();

    test2();

    printf("Completed all tests\n");
    return 0;
}
