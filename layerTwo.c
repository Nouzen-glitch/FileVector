#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <sys/xattr.h>
#endif
#include "layerOne.h"
#include "layerTwo.h"

static FILE *out;
static char outBuf[BUF_SIZE];
int deepScan = 0;

// ── Timing accumulators (thread-safe) ───────────────────────────────────────
// layerTwo() runs across NUM_THREADS threads, so a plain `static double`
// running total like layerOne.c uses would be a data race -- every thread
// doing `total += x` on the same variable with no lock. Instead, each
// thread keeps PRIVATE running totals in processFile() (no locking needed
// while accumulating -- only that thread ever touches its own copies) and
// folds them into these shared globals exactly once, right before it exits,
// via flushThreadTimings(). printLayerTwoTimings() prints the breakdown.
static double timeWebOriginCheck = 0.0; // checkWebOrigin() ADS/xattr lookup
static double timeFileIO = 0.0; // fopen()+fread()+fclose() of the sample
static double timeContentMath =
    0.0; // printable ratio / entropy / histogram fold
static double timeMagicBytes =
    0.0; // magic-byte signature scan (deep scan only)
static double timeWriteLoop =
    0.0; // single-threaded writeIdentity() loop at the end
static mutex_t timingLock;

static double elapsedSeconds(struct timespec start, struct timespec end) {
  return (double)(end.tv_sec - start.tv_sec) +
         (double)(end.tv_nsec - start.tv_nsec) / 1e9;
}

// Folds one thread's private totals into the shared globals above, under
// timingLock, then zeroes the thread's copies. Each thread calls this
// exactly once -- at the moment it discovers there's no more work left.
void flushThreadTimings(double *webOrigin, double *fileIO, double *contentMath,
                        double *magicBytes) {
  MUTEX_LOCK(&timingLock);
  timeWebOriginCheck += *webOrigin;
  timeFileIO += *fileIO;
  timeContentMath += *contentMath;
  timeMagicBytes += *magicBytes;
  MUTEX_UNLOCK(&timingLock);
  *webOrigin = *fileIO = *contentMath = *magicBytes = 0.0;
}

void printLayerTwoTimings(void) {
  fprintf(stderr, "\n--- layerTwo() timing breakdown ---\n");
  fprintf(stderr, "checkWebOrigin() ADS/xattr lookup: %.3fs\n",
          timeWebOriginCheck);
  fprintf(stderr, "file open+read (fopen/fread):       %.3fs\n", timeFileIO);
  fprintf(stderr, "content math (entropy/histogram):   %.3fs\n",
          timeContentMath);
  fprintf(stderr, "magic-byte signature scan:          %.3fs\n",
          timeMagicBytes);
  fprintf(stderr, "writeIdentity() write loop:          %.3fs\n",
          timeWriteLoop);
  fprintf(stderr, "------------------------------------\n");
}

#define X(ext, tag, trueType) {ext, tag, trueType},
const ExtEntry EXT_TABLE[] = {
    EXT_TABLE_LIST{
        NULL, TAG_NONE,
        "UNKNOWN"} // sentinel -- checkExtention()'s loop relies on this
};
#undef X

#define X(sig, sigLen, trueType, tag) {sig, sigLen, trueType, tag},
const MagicEntry MAGIC_TABLE[] = {
    MAGIC_TABLE_LIST{
        {0},
        0,
        NULL,
        TAG_NONE} // sentinel -- the magic-byte scan loop relies on this
};
#undef X

static thread_t threads[NUM_THREADS];
static mutex_t lock;
static size_t nextIndex = 0;
MinMaxValues minMaxVals = {.maxDepth = 0,
                           .minFileSize = LLONG_MAX,
                           .maxFileSize = LLONG_MIN,
                           .minCToMDeltaSeconds = LONG_MAX,
                           .maxCToMDeltaSeconds = LONG_MIN};

// FileIdentityArrayList fileIdentityList = {NULL, 0, 0};

int layerTwo(char *outputPath, int deepScanFlag) {
  char *oPath = outputPath ? outputPath : "fileData.tsv";

  out = fopen(oPath, "w");
  if (!out) {
    fprintf(stderr, "Error: Could not open output file '%s'\n", oPath);
    return -1;
  }
  setvbuf(out, outBuf, _IOFBF, BUF_SIZE);

  fprintf(out,
          "path\ttags\ttrueType\textension\tfileSize\tctime\tmtime\t"
          "byteEntropy\tprintableRatio\tbyteFreqSketch\tburstSessionBucket\t"
          "downloadedFromWeb\tisGenericDumpPath\tdepth\t"
          "whitespaceRatio\theaderBytesHex\tdigitRatioInName\tnamePatternType\t"
          "cTomDeltaSeconds\thourOfDay\tdayOfWeek\n");

  deepScan = deepScanFlag;
  nextIndex = 0;
  MUTEX_INIT(&lock);
  MUTEX_INIT(&timingLock);

  for (int i = 0; i < NUM_THREADS; i++) {
#ifdef _WIN32
    threads[i] = (HANDLE)_beginthreadex(NULL, 0, processFile, NULL, 0, NULL);
#else
    pthread_create(&threads[i], NULL, processFile, NULL);
#endif
  }

#ifdef _WIN32
  WaitForMultipleObjects(NUM_THREADS, threads, TRUE, INFINITE);
  for (int i = 0; i < NUM_THREADS; i++)
    CloseHandle(threads[i]);
#else
  for (int i = 0; i < NUM_THREADS; i++)
    pthread_join(threads[i], NULL);
#endif

  MUTEX_DESTROY(&lock);
  MUTEX_DESTROY(&timingLock);

  // Single-threaded from here on - safe to write everything in original order.
  struct timespec wt0, wt1;
  clock_gettime(CLOCK_MONOTONIC, &wt0);
  for (size_t i = 0; i < fileIdentityList.count; i++) {
    writeIdentity(out, &fileIdentityList.fileIdentities[i]);
  }
  clock_gettime(CLOCK_MONOTONIC, &wt1);
  timeWriteLoop += elapsedSeconds(wt0, wt1);

  fflush(out);
  fclose(out);
  printLayerTwoTimings();
  return 0;
}

#ifdef _WIN32
unsigned __stdcall processFile(void *arg)
#else
void *processFile(void *arg)
#endif
{
  // Thread-private running totals -- only this thread ever touches these,
  // so no lock is needed while accumulating across the files it processes.
  // Folded into the shared globals exactly once via flushThreadTimings().
  double myWebOrigin = 0.0, myFileIO = 0.0, myContentMath = 0.0,
         myMagicBytes = 0.0;

  // Thread-private min/max -- only this thread touches these while
  // accumulating. Start "inside out" so the first real value always wins.
  MinMaxValues myMinMax = {.maxDepth = 0,
                           .minFileSize = LLONG_MAX,
                           .maxFileSize = LLONG_MIN,
                           .minCToMDeltaSeconds = LONG_MAX,
                           .maxCToMDeltaSeconds = LONG_MIN};

  while (1) {
    size_t currentIndex;
    MUTEX_LOCK(&lock);
    if (nextIndex >= fileIdentityList.count) {
      MUTEX_UNLOCK(&lock);
      flushThreadTimings(&myWebOrigin, &myFileIO, &myContentMath,
                         &myMagicBytes);
      flushMinMax(&myMinMax);
      break;
    }
    currentIndex = nextIndex;
    nextIndex++;
    MUTEX_UNLOCK(&lock);
    FileIdentity *id = &fileIdentityList.fileIdentities[currentIndex];
    checkExtention(id);
    // extractFileFeatures() gathers entropy / printable-ratio / histogram
    // / temporal-bucket / dump-path / web-origin for EVERY file, and
    // internally also runs the magic-byte check (reusing the same read)
    // when the extension lookup came back TAG_UNKNOWN and deepScan is on.
    // NOTE: this replaces the old "if (id->extTableIdx == 0 && deepScan)"
    // check, which was a bug -- extTableIdx == 0 means "matched
    // EXT_TABLE[0]" (a .c file), not "unknown extension". The correct
    // test for "we don't know what this is" is the TAG_UNKNOWN bit.
    extractFileFeatures(id, &myWebOrigin, &myFileIO, &myContentMath,
                        &myMagicBytes);
    myMinMax.maxDepth =
        myMinMax.maxDepth < id->depth ? id->depth : myMinMax.maxDepth;
    myMinMax.minFileSize = myMinMax.minFileSize > id->fileSize
                               ? id->fileSize
                               : myMinMax.minFileSize;
    myMinMax.maxFileSize = myMinMax.maxFileSize < id->fileSize
                               ? id->fileSize
                               : myMinMax.maxFileSize;
    myMinMax.minCToMDeltaSeconds =
        myMinMax.minCToMDeltaSeconds > id->cTomDeltaSeconds
            ? id->cTomDeltaSeconds
            : myMinMax.minCToMDeltaSeconds;
    myMinMax.maxCToMDeltaSeconds =
        myMinMax.maxCToMDeltaSeconds < id->cTomDeltaSeconds
            ? id->cTomDeltaSeconds
            : myMinMax.maxCToMDeltaSeconds;
  }
#ifdef _WIN32
  return 0;
#else
  return NULL;
#endif
}

void flushMinMax(MinMaxValues *mine) {
  MUTEX_LOCK(&timingLock);
  if (mine->maxDepth > minMaxVals.maxDepth)
    minMaxVals.maxDepth = mine->maxDepth;
  if (mine->minFileSize < minMaxVals.minFileSize)
    minMaxVals.minFileSize = mine->minFileSize;
  if (mine->maxFileSize > minMaxVals.maxFileSize)
    minMaxVals.maxFileSize = mine->maxFileSize;
  if (mine->minCToMDeltaSeconds < minMaxVals.minCToMDeltaSeconds)
    minMaxVals.minCToMDeltaSeconds = mine->minCToMDeltaSeconds;
  if (mine->maxCToMDeltaSeconds > minMaxVals.maxCToMDeltaSeconds)
    minMaxVals.maxCToMDeltaSeconds = mine->maxCToMDeltaSeconds;
  MUTEX_UNLOCK(&timingLock);
}

// ── Web-origin detection (did this file come from the internet?) ───────────
// Think of this like a little sticky note the operating system puts on a
// file when it noticed a browser downloaded it. Windows writes that note as
// a hidden "Zone.Identifier" stream next to the file. macOS writes it as an
// extended attribute called "kMDItemWhereFroms". Linux doesn't have a single
// standard note like that, but some browsers/tools write one under the
// unofficial "user.xdg.referrer.url" name, so we check for it as a
// best-effort. If the note isn't there, this just returns 0 -- it's a
// bonus signal when present, not something every file will have.
uint8_t checkWebOrigin(const char *filePath) {
#ifdef _WIN32
  char adsPath[PATH_MAX + 20];
  snprintf(adsPath, sizeof(adsPath), "%s:Zone.Identifier", filePath);
  DWORD attr = GetFileAttributesA(adsPath);
  return (attr != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
#elif defined(__APPLE__)
  ssize_t len = getxattr(filePath, "com.apple.quarantine", NULL, 0, 0, 0);
  if (len > 0)
    return 1;
  len =
      getxattr(filePath, "com.apple.metadata:kMDItemWhereFroms", NULL, 0, 0, 0);
  return (len > 0) ? 1 : 0;
#else
  // Plain Linux getxattr only takes 4 arguments -- no position/options
  // like the macOS/BSD version. Mixing these two up is a real compile
  // error on Linux, so they're kept in separate branches rather than one
  // shared call.
  ssize_t len = getxattr(filePath, "user.xdg.referrer.url", NULL, 0);
  return (len > 0) ? 1 : 0;
#endif
}

// ── Context features that need no file I/O at all ───────────────────────────
// These two features come from information we already have in memory
// (the path string and the mtime), so there's no disk read involved.
void extractContextFeatures(const char *filePath, FileIdentity *id) {
  // 900 seconds = 15 minutes. A file saved at 10:03 and another saved at
  // 10:11 land in the SAME bucket (both are mtime/900 = same number), so
  // they look like part of the same "work session". A file saved at 10:03
  // and another at 10:40 land in DIFFERENT buckets.
  id->burstSessionBucket = (uint64_t)(id->mtime / 900);

  time_t mt = (time_t)id->mtime;
  struct tm tmResult;
#ifdef _WIN32
  gmtime_s(&tmResult, &mt);
#else
  gmtime_r(&mt, &tmResult);
#endif
  id->hourOfDay = (uint8_t)tmResult.tm_hour;
  id->dayOfWeek = (uint8_t)tmResult.tm_wday;

  id->isGenericDumpPath = 0;
  if (strstr(filePath, "/Downloads/") || strstr(filePath, "\\Downloads\\") ||
      strstr(filePath, "/Desktop/") || strstr(filePath, "\\Desktop\\") ||
      strstr(filePath, "/Temp/") || strstr(filePath, "\\Temp\\"))
    id->isGenericDumpPath = 1;
}

// ── Content sampling: entropy, printable ratio, byte histogram ─────────────
// This is the heart of what Layer 3 needs. We open the file ONCE, read up to
// SAMPLE_SIZE (4096) bytes from the very front of it, and compute three
// things from that one sample:
//
//   1. printableRatio -- what fraction of the bytes look like normal
//      readable text (letters, numbers, punctuation, tabs/newlines)?
//      A .txt file will be close to 1.0. A compiled .exe will be close to 0.
//
//   2. byteEntropy -- how "surprising" or random the bytes are, using
//      Shannon's formula. A file that's all one repeated byte scores 0.0
//      (totally predictable). A file that's truly random bytes scores close
//      to 8.0 (totally unpredictable). Compressed/encrypted files tend to
//      sit up near 7.9-8.0 too, which is why we don't rely on entropy alone.
//
//   3. byteFreqSketch -- a coarse fingerprint of WHICH byte values show up.
//      We sort the 256 possible byte values into 16 buckets and record what
//      fraction of the sample fell into each bucket. Two files with a
//      similar "shape" to this fingerprint are more likely to be a similar
//      kind of content.
//
// If the extension lookup didn't already figure out what this file is
// (TAG_UNKNOWN) and deep scanning is turned on, we ALSO run the magic-byte
// signature check here, reusing the bytes we already read instead of
// opening the file a second time.
void extractFileFeatures(FileIdentity *id, double *webOriginTime,
                         double *fileIOTime, double *contentMathTime,
                         double *magicBytesTime) {
  // Default every feature to a safe, well-defined value first. If the file
  // can't be opened or is empty, everything downstream (the vector math in
  // Layer 3) still gets clean zeros instead of leftover garbage memory.
  id->byteEntropy = 0.0f;
  id->printableRatio = 0.0f;
  memset(id->byteFreqSketch, 0, sizeof(id->byteFreqSketch));
  int digitCount = 0, hexCount = 0, vowelCount = 0, letterCount = 0,
      nameLen = 0, isAllCaps = 1;
  for (int i = 0; id->name[i] != '\0'; i++) {
    unsigned char c = (unsigned char)id->name[i];
    nameLen++;

    if (c >= '0' && c <= '9')
      digitCount++;
    if (isxdigit(c))
      hexCount++;
    if (isalpha(c)) {
      letterCount++;
      if (islower(c))
        isAllCaps = 0;
      unsigned char lc = tolower(c);
      if (lc == 'a' || lc == 'e' || lc == 'i' || lc == 'o' || lc == 'u')
        vowelCount++;
    }
  }

  id->digitRatioInName =
      nameLen > 0 ? (float)digitCount / (float)nameLen : 0.0f;

  if (id->digitRatioInName > 0.5f)
    id->namePatternType = NAME_TIMESTAMP_LIKE;
  else if (nameLen >= 8 && (float)hexCount / (float)nameLen > 0.9f &&
           vowelCount == 0)
    id->namePatternType = NAME_HASH_LIKE;
  else if (isAllCaps && letterCount > 0)
    id->namePatternType = NAME_ALL_CAPS;
  else
    id->namePatternType = NAME_NORMAL;
  char pathBuff[PATH_MAX];
  buildPath(id, pathBuff, sizeof(pathBuff));

  // These two don't need the file open, so do them regardless of whether
  // the read below succeeds.
  extractContextFeatures(pathBuff, id);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  id->downloadedFromWeb = checkWebOrigin(pathBuff);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  *webOriginTime += elapsedSeconds(t0, t1);

  clock_gettime(CLOCK_MONOTONIC, &t0);
  FILE *f = fopen(pathBuff, "rb");
  if (!f) {
    clock_gettime(CLOCK_MONOTONIC, &t1);
    *fileIOTime += elapsedSeconds(t0, t1);
    return;
  }

  unsigned char buffer[SAMPLE_SIZE];
  size_t bytesRead = fread(buffer, 1, SAMPLE_SIZE, f);
  fclose(f);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  *fileIOTime += elapsedSeconds(t0, t1);
  if (bytesRead == 0)
    return;
  memcpy(id->headerBytes, buffer, sizeof(id->headerBytes));

  clock_gettime(CLOCK_MONOTONIC, &t0);
  // Count how many times each of the 256 possible byte values shows up.
  size_t counts[256] = {0};
  size_t printableCount = 0;
  size_t whiteSpaceCount = 0;
  for (size_t i = 0; i < bytesRead; i++) {
    unsigned char b = buffer[i];
    counts[b]++;
    // ASCII printable: space (0x20) to tilde (0x7E), plus tab, LF, CR
    if ((b >= 0x20 && b <= 0x7E) || b == 0x09 || b == 0x0A || b == 0x0D) {
      printableCount++;
      if (b == 0x20)
        whiteSpaceCount++;
    }
  }

  id->printableRatio = (float)printableCount / (float)bytesRead;
  id->whitespaceRatio = (float)whiteSpaceCount / (float)bytesRead;

  // Shannon entropy: for every byte value that actually appeared, take its
  // probability p (how often it showed up out of the whole sample) and
  // add -p*log2(p) to the running total. Values that appear very often or
  // very rarely contribute less "surprise" than values that appear at a
  // medium, unpredictable frequency.
  double entropy = 0.0;
  for (int i = 0; i < 256; i++) {
    if (counts[i] > 0) {
      double p = (double)counts[i] / (double)bytesRead;
      entropy -= p * log2(p);
    }
  }
  id->byteEntropy = (float)entropy > 8.0f ? 8.0f : (float)entropy;

  // Fold the 256 counts down into HISTOGRAM_BINS (16) buckets of 16
  // consecutive byte values each, then normalize so each bin is a
  // fraction of the whole sample (all 16 bins add up to ~1.0).
  int valuesPerBin = 256 / HISTOGRAM_BINS;
  for (int i = 0; i < HISTOGRAM_BINS; i++) {
    size_t binSum = 0;
    for (int j = 0; j < valuesPerBin; j++) {
      binSum += counts[i * valuesPerBin + j];
    }
    id->byteFreqSketch[i] = (float)binSum / (float)bytesRead;
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  *contentMathTime += elapsedSeconds(t0, t1);

  // Only run the (pricier) magic-byte signature scan when the extension
  // didn't already tell us what this is, and only when deep scanning is
  // requested. Reuses the buffer we already have -- no second file open.
  if ((id->tags & TAG_UNKNOWN) && deepScan) {
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int magicTableId = 0;
    for (int i = 0; MAGIC_TABLE[i].trueType != NULL; i++) {
      const MagicEntry *row = &MAGIC_TABLE[i];
      if (bytesRead >= row->sigLen &&
          memcmp(buffer, row->sig, row->sigLen) == 0) {
        id->tags |= row->tag;
        break;
      }
      magicTableId++;
    }
    id->magicTableIdx = magicTableId;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    *magicBytesTime += elapsedSeconds(t0, t1);
  }
}

void buildPath(FileIdentity *id, char *buffer, size_t bufferSize) {
  int idx = 0, depth = 0;
  char *pathParts[64];
  int folderID = id->folderID;
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
  char *fileName = id->name;
  size_t len = strlen(fileName);
  if (ptr + len + 1 < end) {
    *ptr++ = '/';
    memcpy(ptr, fileName, len);
    ptr += len;
  }
  *ptr = '\0';
}

void buildFilePathToFolder(FileIdentity *id, char *buffer, size_t bufferSize) {
  int idx = 0, depth = 0;
  char *pathParts[64];
  int folderID = id->folderID;
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

void checkExtention(FileIdentity *id) {
  const char *extType = NULL;
  FileTag extTag = TAG_NONE;
  int extTableIdx = 0;
  char *extension = getFileExtension(id->name);
  // strncpy(id->extension, getFileExtension(id->name),
  // sizeof(id->extension)-1);

  for (int i = 0; EXT_TABLE[i].ext != NULL; i++) {
    if (strcmp(EXT_TABLE[i].ext, extension) == 0) {
      extType = EXT_TABLE[i].trueType;
      extTag = EXT_TABLE[i].tag;
      break;
    }
    extTableIdx++;
  }

  if (extType) {
    id->extTableIdx = extTableIdx;
    // strncpy(id->trueType, extType, sizeof(id->trueType)-1);
    id->tags |= extTag;
    /*
    for (int i = 0; MAGIC_TABLE[i].trueType != NULL; i++){
        const MagicEntry *row = &MAGIC_TABLE[i];
        if (strcmp(id->trueType, row->trueType) == 0){
            strncpy(id->mimeHint, row->mimeHint, sizeof(id->mimeHint)-1);
            break;
        }
    }
    */
    return;
  }
  id->tags |= TAG_UNKNOWN;
  id->extTableIdx = extTableIdx;
  // strncpy(id->trueType, "UNKNOWN", sizeof(id->trueType)-1);
}

void getFileTag(int tags, char *out, size_t outSize) {
  out[0] = '\0';
  if (tags == TAG_NONE)
    return;
  // Define a lookup table for the bits
  static const struct {
    FileTag tag;
    char *name;
  } tagNames[] = {{TAG_CODE, "Code"},           {TAG_BINARY, "Binary"},
                  {TAG_DOCUMENT, "Document"},   {TAG_MEDIA, "Media"},
                  {TAG_TEMP, "Temporary"},      {TAG_ARCHIVE, "Archive"},
                  {TAG_SHORTCUT, "Shortcut"},   {TAG_SYSTEM_FILE, "System"},
                  {TAG_DISGUISED, "DISGUISED"}, {TAG_APP_DATA, "App Data"},
                  {TAG_UNKNOWN, "Unknown"},     {0, NULL}};

  int first = 1;
  for (int i = 0; tagNames[i].name != NULL; i++) {
    // Use bitwise AND to see if the specific bit is set
    if (tags & tagNames[i].tag) {
      if (!first)
        strncat(out, ",", outSize - strlen(out) - 1);
      strncat(out, tagNames[i].name, outSize - strlen(out) - 1);
      first = 0;
    }
  }
}

char *getFileExtension(char *filename) {
  char *dot = strrchr(filename, '.');
  if (!dot || dot == filename || *(dot + 1) == '\0' ||
      isdigit((unsigned char)*(dot + 1))) {
    return "";
  }
  return dot + 1;
}

void getParentDir(const char *path, char *out, size_t outSize) {
  strncpy(out, path, outSize - 1);
  out[outSize - 1] = '\0';
  char *lastSlash = strrchr(out, '/');
  if (lastSlash)
    *lastSlash = '\0';
  else
    out[0] = '\0';
}

void writeIdentity(FILE *out, FileIdentity *id) {
  char tagsStr[256];
  getFileTag(id->tags, tagsStr, sizeof(tagsStr));
  char pathBuff[PATH_MAX];
  buildPath(id, pathBuff, sizeof(pathBuff));

  // Pack the 16 histogram bins into one comma-separated string so it can
  // still live in a single tab-delimited column. Layer 3 just needs to
  // split this string on commas to get the 16 numbers back.
  char sketchStr[HISTOGRAM_BINS * 12];
  sketchStr[0] = '\0';
  for (int i = 0; i < HISTOGRAM_BINS; i++) {
    char part[16];
    snprintf(part, sizeof(part), i == 0 ? "%.6f" : ",%.6f",
             id->byteFreqSketch[i]);
    strncat(sketchStr, part, sizeof(sketchStr) - strlen(sketchStr) - 1);
  }

  // headerBytes is raw binary -- hex-encode it so it can't corrupt the
  // tab-delimited file if a byte happens to be '\t', '\n', or '\0'.
  char headerHex[17];
  for (int i = 0; i < 8; i++)
    snprintf(headerHex + i * 2, 3, "%02x", id->headerBytes[i]);

  fprintf(
      out,
      "%s\t%s\t%s\t%lld\t%ld\t%ld\t%.6f\t%.6f\t%s\t%llu\t%u\t%u\t"
      "%u\t%.6f\t%s\t%.6f\t%u\t%ld\t%u\t%u\n",
      strcmp(pathBuff, "") != 0 ? pathBuff : "",
      strcmp(tagsStr, "") != 0 ? tagsStr : "",
      strcmp(EXT_TABLE[id->extTableIdx].trueType, "") != 0
          ? EXT_TABLE[id->extTableIdx].trueType
          : "",
      id->fileSize, id->ctime, id->mtime, id->byteEntropy, id->printableRatio,
      sketchStr, (unsigned long long)id->burstSessionBucket,
      (unsigned)id->downloadedFromWeb, (unsigned)id->isGenericDumpPath,
      (unsigned)id->depth, id->whitespaceRatio, headerHex, id->digitRatioInName,
      (unsigned)id->namePatternType, id->cTomDeltaSeconds,
      (unsigned)id->hourOfDay, (unsigned)id->dayOfWeek);
}
