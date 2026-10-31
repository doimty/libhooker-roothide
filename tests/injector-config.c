// Exercise the actual loader selection with inert files and a dlopen recorder.
// No plugin code is executed, and preferences are confined to a private temp tree.
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

static unsigned checks;
#define REQUIRE(c) do { checks++; if (!(c)) { fprintf(stderr,"injector-config:%d: %s\n",__LINE__,#c); abort(); } } while (0)
static char loaded[16][PATH_MAX];
static unsigned loadCount;
static char preferencePath[PATH_MAX];
static bool rewriteOnFirstLoad;

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path,"w");
    REQUIRE(file != NULL);
    size_t length = strlen(text);
    REQUIRE(fwrite(text,1,length,file) == length);
    REQUIRE(fclose(file) == 0);
}

static void *record_dlopen(const char *path, int flags) {
    REQUIRE(loadCount < 16);
    REQUIRE(flags == RTLD_NOW);
    REQUIRE(strlcpy(loaded[loadCount++],path,PATH_MAX) < PATH_MAX);
    if (rewriteOnFirstLoad && loadCount == 1)
        write_text(preferencePath,"<plist version=\"1.0\"><dict><key>tweaksEnabled</key><false/></dict></plist>");
    return (void *)1;
}

#define LH_TESTING 1
#define LH_LOADER_UNIT_TEST 1
#define dlopen record_dlopen
#include "../injector/injector.c"
#undef dlopen

static const char *header = "<plist version=\"1.0\"><dict>";
static const char *footer = "</dict></plist>";
static void prefs(const char *body) {
    char text[16384];
    REQUIRE(snprintf(text,sizeof(text),"%s%s%s",header,body,footer) < sizeof(text));
    write_text(preferencePath,text);
}
static void reset_loads(void) {
    loadCount=0;
    memset(loaded,0,sizeof(loaded));
    rewriteOnFirstLoad=false;
}
static void check_name(unsigned index,const char *name) {
    REQUIRE(index < loadCount);
    const char *last = strrchr(loaded[index],'/');
    REQUIRE(last && !strcmp(last+1,name));
}
static void all_plugins(const ProcessContext *context) {
    reset_loads();
    tweaks_iterate(context);
    REQUIRE(loadCount == 3);
    check_name(0,"Manager.dylib");
    check_name(1,"PluginA.dylib");
    check_name(2,"PluginB.dylib");
}
static void no_plugins(const ProcessContext *context) {
    reset_loads();
    tweaks_iterate(context);
    REQUIRE(loadCount == 0);
}

int main(void) {
    char root[]="/tmp/lh-injector-config-XXXXXX";
    REQUIRE(mkdtemp(root));
    char directory[PATH_MAX];
    REQUIRE(snprintf(directory,sizeof(directory),"%s/tweaks/",root) < sizeof(directory));
    REQUIRE(mkdir(directory,0700) == 0);
    REQUIRE(snprintf(preferencePath,sizeof(preferencePath),"%s/config.plist",root) < sizeof(preferencePath));
    REQUIRE(setenv("LH_TEST_TWEAK_DIRECTORY",directory,1) == 0);
    REQUIRE(setenv("LH_TEST_CONFIGURATION_PATH",preferencePath,1) == 0);
    const char *names[]={"PluginA","PluginB","Manager","Unmatched"};
    for (unsigned i=0;i<4;i++) {
        char path[PATH_MAX];
        REQUIRE(snprintf(path,sizeof(path),"%s%s.dylib",directory,names[i]) < sizeof(path));
        write_text(path,"inert test fixture; never dlopen'd\n");
        REQUIRE(snprintf(path,sizeof(path),"%s%s.plist",directory,names[i]) < sizeof(path));
        char text[1024];
        REQUIRE(snprintf(text,sizeof(text),
            "<plist version=\"1.0\"><dict><key>Filter</key><dict><key>Executables</key><array><string>%s</string></array></dict>%s</dict></plist>",
            i==3?"DifferentExecutable":"PolicyFixture",
            i==2?"<key>IsTweakManager</key><true/>":"") < sizeof(text));
        write_text(path,text);
    }
    ProcessContext context={.executableName=CFSTR("PolicyFixture"),
        .bundleIdentifier=CFSTR("com.example.policyfixture"),.logicalPath=CFSTR("/usr/libexec/PolicyFixture")};

    // No preferences and malformed files preserve the old metadata filter.
    all_plugins(&context);
    write_text(preferencePath,"broken plist");
    all_plugins(&context);
    write_text(preferencePath,"<plist version=\"1.0\"><array/></plist>");
    all_plugins(&context);

    // A global or per-process disable prevents ALL loads, including managers.
    prefs("<key>tweaksEnabled</key><false/>");
    no_plugins(&context);
    prefs("<key>tweakconfigs</key><dict><key>bundles</key><dict><key>com.example.policyfixture</key><dict><key>enableTweaks</key><false/></dict></dict></dict>");
    no_plugins(&context);

    // Deny selection filters the ordinary plugin, not the infrastructure manager.
    prefs("<key>tweakconfigs</key><dict><key>default</key><dict><key>customConfig</key><true/><key>allowDeny</key><integer>1</integer><key>tweakConfigs</key><dict><key>PluginA.dylib</key><true/><key>Manager.dylib</key><true/></dict></dict></dict>");
    reset_loads();tweaks_iterate(&context);
    REQUIRE(loadCount==2);check_name(0,"Manager.dylib");check_name(1,"PluginB.dylib");

    // Allow cannot force-load a candidate rejected by its own Filter.
    prefs("<key>tweakconfigs</key><dict><key>default</key><dict><key>customConfig</key><true/><key>allowDeny</key><integer>0</integer><key>tweakConfigs</key><dict><key>Unmatched.dylib</key><true/></dict></dict></dict>");
    no_plugins(&context);
    prefs("<key>tweakconfigs</key><dict><key>default</key><dict><key>customConfig</key><true/><key>allowDeny</key><integer>0</integer><key>tweakConfigs</key><dict><key>PluginB.dylib</key><true/></dict></dict></dict>");
    reset_loads();tweaks_iterate(&context);
    REQUIRE(loadCount==2);check_name(0,"Manager.dylib");check_name(1,"PluginB.dylib");

    // New app configuration is consulted on the next process/scan, never during loads.
    prefs("");
    reset_loads();rewriteOnFirstLoad=true;tweaks_iterate(&context);
    REQUIRE(loadCount==3);check_name(0,"Manager.dylib");check_name(1,"PluginA.dylib");check_name(2,"PluginB.dylib");
    no_plugins(&context);

    // Existing safe-mode and directory-integrity gates still precede selection.
    prefs("");reset_loads();
    inject_process_tweaks(&context,"/usr/libexec/PolicyFixture",NULL,NULL,true);
    REQUIRE(loadCount==0);
    ProcessContext ui=context;ui.isUIProcess=true;
    char marker[PATH_MAX];REQUIRE(snprintf(marker,sizeof(marker),"%s/safemode",root)<sizeof(marker));
    write_text(marker,"");
    inject_process_tweaks(&ui,"/Applications/Fixture.app/Fixture",marker,NULL,false);
    REQUIRE(loadCount==0);
    REQUIRE(unlink(marker)==0);
    inject_process_tweaks(&ui,"/Applications/Fixture.app/Fixture",NULL,NULL,false);
    REQUIRE(loadCount==0);

    // Reject configuration symlinks and oversized files; do not follow them.
    char other[PATH_MAX];REQUIRE(snprintf(other,sizeof(other),"%s/other.plist",root)<sizeof(other));
    REQUIRE(rename(preferencePath,other)==0);
    write_text(other,"<plist version=\"1.0\"><dict><key>tweaksEnabled</key><false/></dict></plist>");
    REQUIRE(symlink(other,preferencePath)==0);
    all_plugins(&context);
    REQUIRE(unlink(preferencePath)==0);
    int fd=open(preferencePath,O_CREAT|O_WRONLY,0600);REQUIRE(fd>=0);
    REQUIRE(ftruncate(fd,MAX_CONFIGURATION_BYTES+1)==0);REQUIRE(close(fd)==0);
    all_plugins(&context);
    REQUIRE(unlink(preferencePath)==0);REQUIRE(unlink(other)==0);

    for(unsigned i=0;i<4;i++)for(unsigned j=0;j<2;j++){
        char path[PATH_MAX];
        REQUIRE(snprintf(path,sizeof(path),"%s%s.%s",directory,names[i],j?"plist":"dylib")<sizeof(path));
        REQUIRE(unlink(path)==0);
    }
    REQUIRE(rmdir(directory)==0);REQUIRE(rmdir(root)==0);
    printf("injector-config: %u checks passed; no runtime library was loaded\n",checks);
    return 0;
}
