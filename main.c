#include "layerOne.h"
#include "layerThree.h"
#include "layerTwo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
// Define the globals declared in the header
trie *n = NULL;
trie *examples = NULL;
ExampleRegistry *exampleID;
trie *registryTrie = NULL;
char largeBuffer[BUF_SIZE];
char rootPath[PATH_MAX];

#ifdef _WIN32
AppPathRegistry appRegistry;
#endif

void printLayerOneTimings(void);
void exampleIdAdd(ExampleRegistry *registry, int id);

int main(int argc, char *argv[]) {
  const char *inputPath = (argc > 1) ? argv[1] : "C:/Users/XPS 15";
  char *outputFilePath = "fileData.tsv";

  strncpy(rootPath, inputPath, sizeof(rootPath) - 1);
  rootPath[sizeof(rootPath) - 1] = '\0';
  replaceAll(rootPath, '\\', '/');

  size_t rl = strlen(rootPath);
  if (rl > 0 && rootPath[rl - 1] == '/')
    rootPath[rl - 1] = '\0';

#ifdef _WIN32
  loadInstalledAppPaths(&appRegistry);
  buildRegistryTrie(&appRegistry);
#endif

  n = createNode();
  initializeData(n, "EXCLUDE.txt");

  exampleID = calloc(1, sizeof(ExampleRegistry));
  if (exampleID == NULL) {
    printf("Memory allocation failed!\n");
    return -1;
  }

  clock_t t0 = clock();
  char startPoint[PATH_MAX];
  snprintf(startPoint, sizeof(startPoint), "%.*s/",
           (int)(sizeof(startPoint) - 2), rootPath);
  layerOne(startPoint, 0, 0);
  buildFolderIndex(&folderRegistry, nextID);
  trimRegistries();
  loadSeedFolders("seedFolders.txt");
  clock_t t1 = clock();

  destroyTrie(n);
  if (registryTrie) {
    destroyTrie(registryTrie);
  }

  printLayerOneTimings();
  layerTwo(outputFilePath, 1);
  for (size_t i = 0; i < fileIdentityList.count; i++) {
    if (isSeedFolder(fileIdentityList.fileIdentities[i].folderID))
      exampleIdAdd(exampleID, i);
  }

  /*if (exampleID->count < exampleSize){
      fprintf(stderr, "[main] WARNING: only %d of %d example paths were found in
  the scan\n", exampleID->count, exampleSize);
      // Re-read exampleFiles.txt directly and check each path's existence on
  disk,
      // independent of the trie, to pinpoint which one(s) are missing.
      FILE *f = fopen("exampleFiles.txt", "r");
      if (f){
          char line[PATH_MAX];
          while (fgets(line, sizeof(line), f)){
              line[strcspn(line, "\r\n")] = '\0'; // strip trailing newline/CR
              if (line[0] == '\0') continue;
              FILE *check = fopen(line, "rb");
              if (!check){
                  fprintf(stderr, "[main]   NOT FOUND ON DISK: %s\n", line);
              } else {
                  fclose(check);
              }
          }
          fclose(f);
      }
  }*/
  clock_t t2 = clock();
  layerThree("move.txt");
  clock_t t3 = clock();
  printf("layerOne: %.2fs\nlayerTwo: %.2fs\nlayerThree: %.2fs\n",
         (double)(t1 - t0) / CLOCKS_PER_SEC, (double)(t2 - t1) / CLOCKS_PER_SEC,
         (double)(t3 - t2) / CLOCKS_PER_SEC);
  free(seedFolderFlags);
  free(exampleID->id);
  free(exampleID);
  free(fileIdentityList.fileIdentities);
  free(folderIndexByID);
  free(folderRegistry.folders);
  return 0;
}

void exampleIdAdd(ExampleRegistry *registry, int id) {
  size_t newCapacity = registry->capacity == 0 ? 64 : registry->capacity * 2;

  if (registry->capacity <= registry->count) {
    int *temp = realloc(registry->id, newCapacity * sizeof(int));
    if (!temp)
      return;
    registry->id = temp;
    registry->capacity = newCapacity;
  }

  registry->id[registry->count] = id;
  registry->count++;
}
