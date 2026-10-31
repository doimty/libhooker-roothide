// Standalone lookup diagnostic: no hooks, IOKit connection changes or reboot.
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *iosurface = "/System/Library/Frameworks/IOSurface.framework/IOSurface";
typedef const void *(*GetImage)(const char *);
typedef void *(*FindSymbol)(const void *, const char *);

int main(int argc, char **argv) {
    if (argc != 2 || argv[1][0] != '/') {
        fprintf(stderr, "Usage: %s /absolute/path/to/libjoker.dylib\n", argv[0]);
        return 2;
    }
    // This process owns both handles throughout the lookup. Do not load the
    // injector or MobileSafety: their constructors would change test behavior.
    void *system = dlopen(iosurface, RTLD_NOW | RTLD_LOCAL);
    if (!system) {
        fprintf(stderr, "IOSurface dlopen failed: %s\n", dlerror());
        return 1;
    }
    void *engine = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL | RTLD_FIRST);
    if (!engine) {
        fprintf(stderr, "Engine dlopen failed: %s\n", dlerror());
        dlclose(system);
        return 1;
    }
    const void *forkPrepare = dlsym(RTLD_DEFAULT, "_dyld_dlopen_atfork_prepare");
    const void *forkParent = dlsym(RTLD_DEFAULT, "_dyld_dlopen_atfork_parent");
    printf("dyld fork SPI exports: prepare=%s parent=%s (not called)\n",
        forkPrepare ? "present" : "absent", forkParent ? "present" : "absent");
    const char *(*engineVersion)(void) = (const char *(*)(void))dlsym(engine, "LHVersion");
    if (engineVersion) printf("Engine version: %s\n", engineVersion());
    GetImage getImage = (GetImage)dlsym(engine, "MSGetImageByName");
    FindSymbol findSymbol = (FindSymbol)dlsym(engine, "MSFindSymbol");
    if (!getImage || !findSymbol) {
        fprintf(stderr, "Missing MSGetImageByName/MSFindSymbol exports\n");
        dlclose(engine);
        dlclose(system);
        return 1;
    }
    char resolved[PATH_MAX];
    errno = 0;
    bool onDisk = realpath(iosurface, resolved) != NULL;
    int pathError = errno;
    printf("IOSurface realpath: %s (errno=%d)\n", onDisk ? resolved : "unavailable", pathError);
    const void *loaded = NULL;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *name = _dyld_get_image_name(i);
        if (name && strcmp(name, iosurface) == 0) {
            loaded = _dyld_get_image_header(i);
            break;
        }
    }
    const void *image = getImage(iosurface);
    printf("dyld header: %p\nMSGetImageByName: %p\n", loaded, image);
    if (!image || (loaded && image != loaded)) {
        fprintf(stderr, "FAIL: image lookup rejected or mismatched an explicitly loaded IOSurface\n");
        dlclose(engine);
        dlclose(system);
        return 1;
    }
    void *service = findSymbol(image, "__iosService");
    void *connection = findSymbol(image, "__iosConnect");
    printf("MSFindSymbol __iosService: %p\nMSFindSymbol __iosConnect: %p\n", service, connection);
    // These are storage addresses, not function pointers. Never call them or
    // read/write their contents, and do not invoke IOServiceOpen/Close.
    bool success = service && connection;
    puts(success ? "PASS: image and private symbols resolved" : "FAIL: private symbol lookup remains unavailable");
    dlclose(engine);
    dlclose(system);
    return success ? 0 : 1;
}
