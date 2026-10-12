#ifndef LAYER_ONE_H
#define LAYER_ONE_H
#include "layerTwo.h"
#include "trie.h"
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>

#define MAX_APP_PATHS 256

#ifdef _WIN32
#include <windows.h>
typedef struct {
  char paths[MAX_APP_PATHS][PATH_MAX];
  int count;
} AppPathRegistry;
#endif

typedef struct {
  char name[256];
  int parentID;
  int folderID;
} FolderNode;

typedef struct {
  FolderNode *folders;
  size_t count;
  size_t capacity;
} FolderRegistry;

// Global pointers used by the layer (declared as extern)
extern trie *n;
extern trie *examples;
extern trie *registryTrie;
extern FolderRegistry folderRegistry;
extern int nextID;
extern int *folderIndexByID;
extern char rootPath[PATH_MAX];

// Function Prototypes
int layerOne(char *path, int parentID, uint8_t depth);
void buildFolderIndex(FolderRegistry *reg, int maxID);
void folderRegistryAdd(FolderRegistry *registry, FolderNode folder);
void addFileIdentity(FileIdentityArrayList *array, FileIdentity id);

#ifdef _WIN32
void loadInstalledAppPaths(AppPathRegistry *reg);
void buildRegistryTrie(AppPathRegistry *reg);
int isRegProtected(char *dirPath);
#endif

int fileStatus(char *filePath, struct stat *buffer);
void trimRegistries();
void replaceAll(char *str, char oldChar, char newChar);

void buildFolderPath(int folderID, char *buffer, size_t bufferSize);
void loadSeedFolders(const char *seedFile);
int isSeedFolder(int folderID);
extern bool *seedFolderFlags;
#endif
