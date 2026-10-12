#ifndef _WIN
#define _DEFAULT_SOURCE // exposes d_type, lstat() under strict -std=c11
#endif

#include "layerOne.h"
#include "layerTwo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_APP_PATHS 256
#ifdef _WIN32
#include <io.h>
#include <windows.h> // For GetFileAttributes
#include <winreg.h>
#define F_OK 0
#define access _access // Windows uses _access instead of access
#else
#include <unistd.h> // For getuid on Linux/Mac
#endif

char *sep = "/";
size_t len = PATH_MAX;

FolderRegistry folderRegistry = {NULL, 0, 0};
FileIdentityArrayList fileIdentityList = {NULL, 0, 0};

int nextID = 1;

static const unsigned long long MAX_FILE_SIZE = 53687091200ULL;

// ── Timing accumulators ─────────────────────────────────────────────────────
// layerOne() recurses once per directory, so these are *global, running
// totals* across the whole traversal, not per-call. Call printLayerOneTimings()
// once after the top-level layerOne() call returns to see the breakdown.
static double timeReaddirLoop = 0.0; // building names[]/types[] from readdir()
static double timeSentinelCheck = 0.0; // isSentinel()/search() scanning
static double timeLstat = 0.0;         // the lstat()/stat() syscall itself
static double timeFileStatus = 0.0;    // fileStatus() permission/size checks
static double timeFprintfWrite = 0.0;  // writing a line to statusFile
                                       // read from wherever you print it)

static double elapsedSeconds(struct timespec start, struct timespec end) {
  return (double)(end.tv_sec - start.tv_sec) +
         (double)(end.tv_nsec - start.tv_nsec) / 1e9;
}

#ifdef _WIN32
static int fileStatusWin(const WIN32_FIND_DATAA *attrs);
static long long fileTimeToUnixSeconds(const FILETIME *ft);
#endif

void printLayerOneTimings(void) {
  fprintf(stderr, "\n--- layerOne() timing breakdown ---\n");
  fprintf(stderr, "readdir loop (build names/types): %.3fs\n", timeReaddirLoop);
  fprintf(stderr, "isSentinel/search checks:         %.3fs\n",
          timeSentinelCheck);
  fprintf(stderr, "lstat()/stat() calls:              %.3fs\n", timeLstat);
  fprintf(stderr, "fileStatus() checks:               %.3fs\n", timeFileStatus);
  fprintf(stderr, "fprintf(statusFile, ...) writes:   %.3fs\n",
          timeFprintfWrite);
  fprintf(stderr, "------------------------------------\n");
}

// ── Neighborhood sentinel table ─────────────────────────────────────────────
static const char *PROJECT_SENTINELS[] = {
    ".git",         "Makefile",         "CMakeLists.txt",
    "package.json", "requirements.txt", "Cargo.toml",
    "pom.xml",      "build.gradle",     "go.mod",
    ".sln",         "Dockerfile",       NULL};

// Blacklist of directory names that should NEVER be traversed
static const char *SKIP_DIR_NAMES[] = {
    ".git", "node_modules", "target",  ".venv", "venv", "bin",
    "obj",  ".idea",        ".vscode", "build", "dist", NULL};

static int isIgnoredDir(const char *dirName) {
  for (int i = 0; SKIP_DIR_NAMES[i] != NULL; i++) {
    if (strcmp(dirName, SKIP_DIR_NAMES[i]) == 0)
      return 1;
  }
  return 0;
}

static int isSentinel(const char *name) {
  for (int i = 0; PROJECT_SENTINELS[i] != NULL; i++) {
    if (PROJECT_SENTINELS[i][0] == '.' &&
        strcmp(PROJECT_SENTINELS[i], ".git") != 0) {
      size_t namelen = strlen(name);
      size_t projectSentinelLen = strlen(PROJECT_SENTINELS[i]);
      if (namelen >= projectSentinelLen &&
          strcmp(name + namelen - projectSentinelLen, PROJECT_SENTINELS[i]) ==
              0) {
        return 1;
      }
    } else if (strcmp(name, PROJECT_SENTINELS[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

int layerOne(char *path, int parentID, uint8_t depth) {
  int status;
  struct timespec t0, t1;

  size_t nameCount = 0;
  size_t capacity = 64;
  char (*names)[256] = malloc(capacity * sizeof(*names));
  if (!names)
    return 0;

#ifdef _WIN32
  // NEW: FindFirstFile/FindNextFile already hand back full attributes,
  // size, and mtime per entry -- capture that here instead of throwing it
  // away and re-asking with a second GetFileAttributesExA() call per file.
  WIN32_FIND_DATAA *attrs = malloc(capacity * sizeof(WIN32_FIND_DATAA));
  if (!attrs) {
    free(names);
    return 0;
  }

  char pattern[PATH_MAX];
  snprintf(pattern, sizeof(pattern), "%s/*", path);

  WIN32_FIND_DATAA findData;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  HANDLE hFind = FindFirstFileA(pattern, &findData);
  if (hFind == INVALID_HANDLE_VALUE) {
    free(names);
    free(attrs);
    return 0;
  }

  while (1) {
    if (strcmp(findData.cFileName, ".") != 0 &&
        strcmp(findData.cFileName, "..") != 0) {
      if (capacity <= nameCount) {
        capacity *= 2;
        char (*tempNames)[256] = realloc(names, capacity * sizeof(*names));
        if (!tempNames) {
          free(names);
          free(attrs);
          FindClose(hFind);
          return 0;
        }
        names = tempNames;

        WIN32_FIND_DATAA *tempAttrs =
            realloc(attrs, capacity * sizeof(WIN32_FIND_DATAA));
        if (!tempAttrs) {
          free(names);
          free(attrs);
          FindClose(hFind);
          return 0;
        }
        attrs = tempAttrs;
      }
      strncpy(names[nameCount], findData.cFileName, sizeof(names[0]) - 1);
      names[nameCount][sizeof(names[0]) - 1] = '\0';
      attrs[nameCount] = findData;
      nameCount++;
    }
    if (!FindNextFileA(hFind, &findData))
      break;
  }
  FindClose(hFind);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  timeReaddirLoop += elapsedSeconds(t0, t1);

#else
  DIR *dir = opendir(path);
  if (!dir) {
    free(names);
    return 0;
  }

  // d_type comes free out of readdir() on POSIX. Capturing it here lets the
  // loop below skip an lstat() syscall for entries it can already classify
  // (directories, symlinks) -- same outcome as before, fewer syscalls.
  unsigned char *types = malloc(capacity * sizeof(*types));
  if (!types) {
    free(names);
    closedir(dir);
    return 0;
  }

  struct dirent *entry;

  clock_gettime(CLOCK_MONOTONIC, &t0);
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    if (capacity <= nameCount) {
      capacity *= 2;
      char (*temp)[256] = realloc(names, capacity * sizeof(*names));
      if (!temp) {
        free(names);
        free(types);
        closedir(dir);
        return 0;
      }
      names = temp;

      unsigned char *typesTemp = realloc(types, capacity * sizeof(*types));
      if (!typesTemp) {
        free(names);
        free(types);
        closedir(dir);
        return 0;
      }
      types = typesTemp;
    }

    strncpy(names[nameCount], entry->d_name, sizeof(names[0]) - 1);
    names[nameCount][sizeof(names[0]) - 1] = '\0';
    types[nameCount] = entry->d_type;
    nameCount++;
  }

  closedir(dir);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  timeReaddirLoop += elapsedSeconds(t0, t1);
#endif

  clock_gettime(CLOCK_MONOTONIC, &t0);
  if (parentID != 0) {
    for (size_t i = 0; i < nameCount; i++) {
      if (isSentinel(names[i])) {
        fprintf(stderr,
                "[layerOne] SKIPPING whole directory '%s' -- sentinel '%s' "
                "found inside it\n",
                path, names[i]);
        free(names);
#ifdef _WIN32
        free(attrs);
#else
        free(types);
#endif
        clock_gettime(CLOCK_MONOTONIC, &t1);
        timeSentinelCheck += elapsedSeconds(t0, t1);
        return 0;
      }
    }
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  timeSentinelCheck += elapsedSeconds(t0, t1);

  // size_t pathLen = strlen(path);
  // int hasSep = (pathLen > 0 && (path[pathLen-1] == '/' || path[pathLen-1] ==
  // '\\'));

  for (size_t i = 0; i < nameCount; i++) {
    char *file = names[i];

    clock_gettime(CLOCK_MONOTONIC, &t0);
    int skip = (*file == 0x2E || search(n, file) == 1);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    timeSentinelCheck += elapsedSeconds(t0, t1);
    if (skip) {
      continue;
    }

#ifndef _WIN32
    if (types[i] == DT_LNK) {
      // fileStatus() only ever accepted S_ISREG, and a symlink is never
      // S_ISDIR either -- lstat() would end up rejecting this anyway.
      continue;
    }
#endif

    // size_t len = pathLen + (hasSep ? 0:strlen(sep)) + strlen(file) + 1;
    char filePath[len];
    // if (hasSep)
    //     snprintf(filePath, len, "%s%s", path, file);
    // else
    snprintf(filePath, len, "%s%s%s", path, sep, file);

#ifndef _WIN32
    if (types[i] == DT_DIR) {
      // Known-good directory straight from readdir() -- exactly what
      // S_ISDIR(lstat(...)) below would have told us, no syscall needed.
      if (isIgnoredDir(file))
        continue;

      FolderNode folderNode = {0};
      strncpy(folderNode.name, file, sizeof(folderNode.name) - 1);
      folderNode.parentID = parentID;
      folderNode.folderID = nextID++;
      folderRegistryAdd(&folderRegistry, folderNode);
      layerOne(filePath, folderNode.folderID, depth + 1);
      continue;
    }
#endif

#ifdef _WIN32
    // attrs[i] was already captured by FindFirstFile/FindNextFile above --
    // no per-entry syscall needed here anymore.
    WIN32_FIND_DATAA *winAttrs = &attrs[i];

    if ((winAttrs->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        (winAttrs->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
      continue;

    if (winAttrs->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (isIgnoredDir(file))
        continue;

      if (isRegProtected(filePath) != 0)
        continue;

      FolderNode folderNode = {0};
      strncpy(folderNode.name, file, sizeof(folderNode.name) - 1);
      folderNode.parentID = parentID;
      folderNode.folderID = nextID++;
      folderRegistryAdd(&folderRegistry, folderNode);
      layerOne(filePath, folderNode.folderID, depth + 1);
    } else {
      clock_gettime(CLOCK_MONOTONIC, &t0);
      status = fileStatusWin(winAttrs);
      clock_gettime(CLOCK_MONOTONIC, &t1);
      timeFileStatus += elapsedSeconds(t0, t1);

      if (status == 1) {
        FileIdentity id;
        memset(&id, 0, sizeof(id));
        // strncpy(id.path, filePath, sizeof(id.path)-1);
        strncpy(id.name, file, sizeof(id.name) - 1);
        id.fileID = nextID++;
        id.folderID = parentID;
        id.depth = depth;

        ULARGE_INTEGER fileSize;
        fileSize.LowPart = winAttrs->nFileSizeLow;
        fileSize.HighPart = winAttrs->nFileSizeHigh;
        long long mtime = fileTimeToUnixSeconds(&winAttrs->ftLastWriteTime);
        long long ctime = fileTimeToUnixSeconds(&winAttrs->ftCreationTime);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        id.fileSize = fileSize.QuadPart;
        id.mtime = (long)mtime;
        id.ctime = (long)ctime;
        id.cTomDeltaSeconds = id.ctime - id.mtime;
        addFileIdentity(&fileIdentityList, id);
        // fprintf(statusFile, "%s\t%lld\t%lld\n", filePath, (long long)
        // fileSize.QuadPart, mtime);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        timeFprintfWrite += elapsedSeconds(t0, t1);
      }
    }
#else
    struct stat gbuffer;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int lstatFailed = (lstat(filePath, &gbuffer) == -1);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    timeLstat += elapsedSeconds(t0, t1);
    if (lstatFailed)
      continue;

    if (S_ISDIR(gbuffer.st_mode)) {
      // printf("%s || [DIR]\n", filePath);

      if (isIgnoredDir(file))
        continue;

      FolderNode folderNode = {0};
      strncpy(folderNode.name, file, sizeof(folderNode.name) - 1);
      folderNode.parentID = parentID;
      folderNode.folderID = nextID++;
      folderRegistryAdd(&folderRegistry, folderNode);
      layerOne(filePath, folderNode.folderID, depth + 1);
    }

    else {
      clock_gettime(CLOCK_MONOTONIC, &t0);
      status = fileStatus(filePath, &gbuffer);
      clock_gettime(CLOCK_MONOTONIC, &t1);
      timeFileStatus += elapsedSeconds(t0, t1);

      if (status == 1) {
        FileIdentity id;
        memset(&id, 0, sizeof(id));
        strncpy(id.name, file, sizeof(id.name) - 1);
        id.fileID = nextID++;
        id.folderID = parentID;
        id.depth = depth;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        id.fileSize = gbuffer.st_size;
        id.mtime = (long)gbuffer.st_mtime;
#ifdef __APPLE__
        // macOS's struct stat carries a REAL birth/creation time,
        // unlike Linux -- exposed via the st_birthtime accessor. This
        // is the one POSIX-ish platform where "creation time" means
        // creation time, matching Windows' ftCreationTime semantics.
        id.ctime = (long)gbuffer.st_birthtime;
#else
        id.ctime =
            (long)gbuffer
                .st_ctime; // metadata-change time, NOT creation time on Linux
#endif
        id.cTomDeltaSeconds = id.ctime - id.mtime;
        addFileIdentity(&fileIdentityList, id);
        // fprintf(statusFile, "%s\t%lld\t%lld\n",filePath, (long long)
        // gbuffer.st_size, (long long) gbuffer.st_mtime);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        timeFprintfWrite += elapsedSeconds(t0, t1);
      }
    }
#endif
  }
  free(names);
#ifdef _WIN32
  free(attrs);
#else
  free(types);
#endif
  return 0;
}

bool *seedFolderFlags =
    NULL; // indexed by folderID; true if this folder is a seed

// Same parent-chain walk buildPath()/buildFilePathToFolder() do in layerTwo.c,
// factored out to run from a bare folderID -- seed matching has no FileIdentity
// to hang the walk off of.
void buildFolderPath(int folderID, char *buffer, size_t bufferSize) {
  int idx = 0, depth = 0;
  char *pathParts[64];
  while (folderID != 0 && depth < 64) {
    idx = folderIndexByID[folderID];
    if (idx < 0)
      break;
    pathParts[depth++] = folderRegistry.folders[idx].name;
    folderID = folderRegistry.folders[idx].parentID;
  }
  char *ptr = buffer;
  char *end = buffer + bufferSize - 1;
  size_t rootLen = strlen(rootPath);
  if (ptr + rootLen >= end) {
    *ptr = '\0';
    return;
  }
  memcpy(ptr, rootPath, rootLen);
  ptr += rootLen;
  for (int i = depth - 1; i >= 0; i--) {
    char *folderName = pathParts[i];
    size_t len = strlen(folderName);
    if (ptr + len + 1 >= end)
      break;
    *ptr++ = '/';
    memcpy(ptr, folderName, len);
    ptr += len;
  }
  *ptr = '\0';
}

// Call AFTER buildFolderIndex() -- needs folderIndexByID populated. One
// folder path per line in seedFile; every scanned folder whose reconstructed
// path matches gets flagged as a seed.
void loadSeedFolders(const char *seedFile) {
  seedFolderFlags = calloc((size_t)nextID, sizeof(bool));
  if (!seedFolderFlags) {
    fprintf(stderr, "[loadSeedFolders] calloc failed, skipping\n");
    return;
  }
  FILE *f = fopen(seedFile, "r");
  if (!f) {
    fprintf(stderr, "[loadSeedFolders] no seed file '%s' -- skipping\n",
            seedFile);
    return;
  }
  char line[PATH_MAX];
  int matched = 0, total = 0;
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = '\0';
    if (line[0] == '\0')
      continue;
    total++;
    replaceAll(line, '\\', '/');
    size_t ll = strlen(line);
    if (ll > 0 && line[ll - 1] == '/')
      line[ll - 1] = '\0';

    for (size_t i = 0; i < folderRegistry.count; i++) {
      char folderPath[PATH_MAX];
      buildFolderPath(folderRegistry.folders[i].folderID, folderPath,
                      sizeof(folderPath));
      if (strcmp(folderPath, line) == 0) {
        seedFolderFlags[folderRegistry.folders[i].folderID] = true;
        matched++;
        break;
      }
    }
  }
  fclose(f);
  fprintf(stderr, "[loadSeedFolders] matched %d/%d seed folders\n", matched,
          total);
}

int isSeedFolder(int folderID) {
  if (!seedFolderFlags || folderID <= 0 || folderID >= nextID)
    return 0;
  return seedFolderFlags[folderID];
}

int *folderIndexByID; // sized nextID+1; -1 = "no folder with this ID" (i.e.
                      // root)
void buildFolderIndex(FolderRegistry *reg, int maxID) {
  folderIndexByID = malloc((maxID + 1) * sizeof(int));
  for (int i = 0; i <= maxID; i++)
    folderIndexByID[i] = -1;
  for (size_t i = 0; i < reg->count; i++)
    folderIndexByID[reg->folders[i].folderID] = (int)i;
}

void addFileIdentity(FileIdentityArrayList *array, FileIdentity id) {
  size_t newCapacity = array->capacity == 0 ? 64 : array->capacity * 2;

  if (array->capacity <= array->count) {
    FileIdentity *temp =
        realloc(array->fileIdentities, newCapacity * sizeof(FileIdentity));
    if (!temp)
      return;

    array->fileIdentities = temp;
    array->capacity = newCapacity;
  }

  array->fileIdentities[array->count] = id;
  array->count++;
}

void folderRegistryAdd(FolderRegistry *registry, FolderNode folder) {
  size_t newCapacity = registry->capacity == 0 ? 64 : registry->capacity * 2;

  if (registry->capacity <= registry->count) {
    FolderNode *temp =
        realloc(registry->folders, newCapacity * sizeof(FolderNode));
    if (!temp) {
      return;
    }
    registry->folders = temp;
    registry->capacity = newCapacity;
  }

  registry->folders[registry->count] = folder;
  registry->count++;
}

void fileRegistryAdd(FileIdentityArrayList *registry, FileIdentity file) {
  size_t newCapacity = registry->capacity == 0 ? 64 : registry->capacity * 2;

  if (registry->capacity <= registry->count) {
    FileIdentity *temp =
        realloc(registry->fileIdentities, newCapacity * sizeof(FileIdentity));
    if (temp == NULL) {
      return;
    }
    registry->fileIdentities = temp;
    registry->capacity = newCapacity;
  }

  registry->fileIdentities[registry->count] = file;
  registry->count++;
}

#ifdef _WIN32
void loadInstalledAppPaths(AppPathRegistry *reg) {
  reg->count = 0;
  HKEY hUninstall;

  // Check both machine-wide and per-user installs
  HKEY roots[] = {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER};
  const char *subkey =
      "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

  for (int r = 0; r < 2; r++) {
    if (RegOpenKeyExA(roots[r], subkey, 0, KEY_READ, &hUninstall) !=
        ERROR_SUCCESS)
      continue;

    char appKeyName[256];
    DWORD index = 0;
    while (RegEnumKeyA(hUninstall, index++, appKeyName, sizeof(appKeyName)) ==
           ERROR_SUCCESS) {
      HKEY hApp;
      if (RegOpenKeyExA(hUninstall, appKeyName, 0, KEY_READ, &hApp) !=
          ERROR_SUCCESS)
        continue;

      char installPath[PATH_MAX];
      DWORD size = sizeof(installPath);
      DWORD type;
      if (RegQueryValueExA(hApp, "InstallLocation", NULL, &type,
                           (LPBYTE)installPath, &size) == ERROR_SUCCESS) {

        // FORCE null termination based on the 'size' Windows reported
        if (size < PATH_MAX)
          installPath[size] = '\0';
        else
          installPath[PATH_MAX - 1] = '\0';

        if (strlen(installPath) > 0 && reg->count < MAX_APP_PATHS) {
          // Normalize slashes
          strncpy(reg->paths[reg->count], installPath, PATH_MAX);
          reg->paths[reg->count][PATH_MAX - 1] = '\0';
          replaceAll(reg->paths[reg->count], '\\', '/');
          reg->count++;
        }
      }
      RegCloseKey(hApp);
    }
    RegCloseKey(hUninstall);
  }
}

void buildRegistryTrie(AppPathRegistry *reg) {
  registryTrie = createNode();
  for (int i = 0; i < reg->count; i++) {
    insert(registryTrie, reg->paths[i]);
  }
}

int isRegProtected(char *dirPath) {
  return searchPrefix(registryTrie, dirPath);
}

// Same accept/reject rules as fileStatus() above, kept as a separate function
// (rather than changing fileStatus()'s signature) since layerOne() now already
// has a WIN32_FILE_ATTRIBUTE_DATA in hand from GetFileAttributesExA() and
// doesn't need a second GetFileAttributesA() syscall to get the same bits.
static int fileStatusWin(const WIN32_FIND_DATAA *attrs) {
  ULARGE_INTEGER size;
  size.LowPart = attrs->nFileSizeLow;
  size.HighPart = attrs->nFileSizeHigh;
  if (size.QuadPart >= MAX_FILE_SIZE)
    return 0;

  if (attrs->dwFileAttributes & (FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN))
    return 0;

  if (attrs->dwFileAttributes & FILE_ATTRIBUTE_READONLY)
    return 0;

  return 1;
}

// Windows FILETIME is 100ns ticks since 1601-01-01. statusFile's format
// expects a Unix mtime (seconds since 1970-01-01), same as st_mtime used to
// give -- 116444736000000000 is the tick offset between those two epochs.
static long long fileTimeToUnixSeconds(const FILETIME *ft) {
  ULARGE_INTEGER t;
  t.LowPart = ft->dwLowDateTime;
  t.HighPart = ft->dwHighDateTime;
  return (long long)((t.QuadPart - 116444736000000000ULL) / 10000000ULL);
}
#endif

void dirContent(const char *path) {
  DIR *dir = opendir(path);
  struct dirent *entry;

  if (dir == NULL) {
    printf("Read failed!\n");
    return;
  }
  while ((entry = readdir(dir)) != NULL) {
    printf("%s\n", entry->d_name);
  }
  closedir(dir);
}

int fileStatus(char *filePath, struct stat *buffer) {
  if ((unsigned long long)buffer->st_size >= MAX_FILE_SIZE)
    return 0;

#ifndef _WIN32
  if (!S_ISREG(buffer->st_mode))
    return 0;
#endif

#ifdef _WIN32
  DWORD attribute = GetFileAttributesA(filePath);
  if (attribute & (FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN))
    return 0;

  if (attribute & FILE_ATTRIBUTE_READONLY)
    return 0;
#else
  if (buffer->st_uid != getuid())
    return 0;
  if (!(buffer->st_mode & S_IRUSR) || !(buffer->st_mode & S_IWUSR))
    return 0;
#endif
  return 1;
}

void trimRegistries() {
  if (folderRegistry.count < folderRegistry.capacity) {
    FolderNode *temp = realloc(folderRegistry.folders,
                               folderRegistry.count * sizeof(FolderNode));
    if (temp) {
      folderRegistry.folders = temp;
      folderRegistry.capacity = folderRegistry.count;
    }
  }
  if (fileIdentityList.count < fileIdentityList.capacity) {
    FileIdentity *temp = realloc(fileIdentityList.fileIdentities,
                                 fileIdentityList.count * sizeof(FileIdentity));
    if (temp) {
      fileIdentityList.fileIdentities = temp;
      fileIdentityList.capacity = fileIdentityList.count;
    }
  }
  if (fileIdentityList.count < fileIdentityList.capacity) {
    FileIdentity *temp = realloc(fileIdentityList.fileIdentities,
                                 fileIdentityList.count * sizeof(FileIdentity));
    if (temp) {
      fileIdentityList.fileIdentities = temp;
      fileIdentityList.capacity = fileIdentityList.count;
    }
  }
}

void replaceAll(char *str, char oldChar, char newChar) {
  for (int i = 0; str[i] != '\0'; i++) {
    if (str[i] == oldChar)
      str[i] = newChar;
  }
}
