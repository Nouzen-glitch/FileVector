#include "layerThree.h"
#include "layerTwo.h"
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static thread_t threads[NUM_THREADS];
static mutex_t lock;
static size_t nextIndex = 0;
static FILE *out;
FileVector *fileVectorList;

typedef struct {
  int bestExample;
  float bestDist;
  float runnerUpDist;
} MoveCandidate;

typedef struct {
  float median;
  float Q1;
  float Q3;
  float IQR;
} MedianData;

typedef struct {
  int folderID;
  float *distances;
  size_t count;
  size_t capacity;
  MedianData stats;
} CategoryBucket;

CategoryBucket *buckets = NULL;
size_t bucketCount = 0, bucketCap = 0;

CategoryBucket *getBucket(int folderID) {
  for (size_t i = 0; i < bucketCount; i++)
    if (folderID == buckets[i].folderID)
      return &buckets[i];
  return NULL;
}

CategoryBucket *addBucket(int folderID) {
  size_t newCapacity = bucketCap == 0 ? 32 : bucketCap * 2;
  if (bucketCount >= bucketCap) {
    CategoryBucket *temp =
        realloc(buckets, newCapacity * sizeof(CategoryBucket));
    if (!temp)
      return NULL;
    buckets = temp;
    bucketCap = newCapacity;
  }

  CategoryBucket *b = &buckets[bucketCount++];
  b->folderID = folderID;
  b->distances = NULL;
  b->count = 0;
  b->capacity = 0;

  return b;
}

void bucketDistanceAdd(CategoryBucket *b, float distance) {
  size_t newCapacity = b->capacity == 0 ? 32 : b->capacity * 2;
  if (b->capacity <= b->count) {
    float *temp = realloc(b->distances, newCapacity * sizeof(int));
    if (!temp)
      return;
    b->distances = temp;
    b->capacity = newCapacity;
  }
  b->distances[b->count++] = distance;
}

static void merge(float arr[], float temp[], size_t left, size_t mid,
                  size_t right) {
  size_t i = left, j = mid + 1, k = left;
  while (i <= mid && j <= right) {
    if (arr[i] <= arr[j])
      temp[k++] = arr[i++];
    else
      temp[k++] = arr[j++];
  }

  while (i <= mid)
    temp[k++] = arr[i++];
  while (j <= right)
    temp[k++] = arr[j++];

  for (i = left; i <= right; i++)
    arr[i] = temp[i];
}

static void mergeSortRecuse(float arr[], float temp[], size_t left,
                            size_t right) {
  if (left >= right)
    return;

  size_t mid = left + (right - left) / 2;
  mergeSortRecuse(arr, temp, left, mid);
  mergeSortRecuse(arr, temp, mid + 1, right);
  merge(arr, temp, left, mid, right);
}

static void mergeSort(float arr[], size_t count) {
  if (count <= 1 || !arr)
    return;
  float *temp = (float *)malloc(count * sizeof(float));
  if (!temp)
    return; // FIX 3: Valid NULL check for heap allocation

  mergeSortRecuse(arr, temp, 0, count - 1);
  free(temp);
}

// ── Timing (single-threaded phases just accumulate directly; \\ization
//    phase is timed wall-clock around the thread create/join, since that's
//    what you actually feel as a user waiting on it) ─────────────────────
static double timeVectorize = 0.0; // thread spin-up + vectorize() work + join
static double timeMatching = 0.0;  // nearest-example search + TSV write loop
static size_t unmatchedCount = 0;

static MedianData getMedianData(float arr[], size_t len) {
  MedianData MD = {0};
  if (!arr || len == 0)
    return MD;
  int setting = len % 2;
  size_t mid = len / 2;
  size_t q1Idx = (size_t)(0.25 * (len - 1));
  size_t q3Idx = (size_t)(0.75 * (len - 1));
  if (setting) {
    MD.median = arr[mid];
    MD.Q1 = (q1Idx < len) ? arr[q1Idx] : arr[len - 1];
    MD.Q3 = (q3Idx < len) ? arr[q3Idx] : arr[len - 1];
    MD.IQR = MD.Q3 - MD.Q1;
  } else {
    MD.median = mid > 0 ? (arr[mid - 1] + arr[mid]) / 2.0f : arr[0];
    MD.Q1 = (q1Idx < len) ? arr[q1Idx] : arr[len - 1];
    MD.Q3 = (q3Idx < len) ? arr[q3Idx] : arr[len - 1];
    MD.IQR = MD.Q3 - MD.Q1;
  }
  return MD;
}

static double elapsedSeconds(struct timespec start, struct timespec end) {
  return (double)(end.tv_sec - start.tv_sec) +
         (double)(end.tv_nsec - start.tv_nsec) / 1e9;
}

void printLayerThreeTimings(size_t totalFiles, size_t totalExamples) {
  fprintf(stderr, "\n--- layerThree() timing breakdown ---\n");
  fprintf(stderr, "files processed:                    %zu\n", totalFiles);
  fprintf(stderr, "examples loaded:                    %zu\n", totalExamples);
  fprintf(stderr, "files with no matched example:       %zu\n", unmatchedCount);
  fprintf(stderr, "vectorization (threaded):           %.3fs\n", timeVectorize);
  fprintf(stderr, "matching + write loop:               %.3fs\n", timeMatching);
  fprintf(stderr, "------------------------------------\n");
}

static unsigned long hashToken(const char *token) {
  unsigned long hash = 5381;
  int c;
  while ((c = (unsigned char)*token++))
    hash = ((hash << 5) + hash) + c; // hash * 33 + c
  return hash;
}

static void vectorizeNameTokens(const char *name, float *outBuckets) {
  memset(outBuckets, 0, sizeof(float) * NUM_NAME_TOKEN_BUCKETS);

  char base[MAX_FILENAME_LEN];
  strncpy(base, name, MAX_FILENAME_LEN - 1);
  base[MAX_FILENAME_LEN - 1] = '\0';
  char *dot = strrchr(base, '.');
  if (dot && dot != base)
    *dot = '\0';

  int tokenCount = 0;
  char token[MAX_FILENAME_LEN];
  int tlen = 0, allDigits = 1;

  for (int i = 0;; i++) {
    unsigned char c = (unsigned char)base[i];
    int isSeparator =
        (c == '_' || c == '-' || c == '.' || c == ' ' || c == '\0');
    if (!isSeparator) {
      if (tlen < MAX_FILENAME_LEN - 1) {
        token[tlen++] = (char)tolower(c);
        if (allDigits == 1 && !isdigit(c))
          allDigits = 0;
      }
    } else if (tlen > 0) {
      token[tlen] = '\0';
      if (!allDigits) {
        unsigned long h = hashToken(token) % NUM_NAME_TOKEN_BUCKETS;
        outBuckets[h] += 1.0f;
        tokenCount++;
      }
      tlen = 0;
      allDigits = 1;
    }
    if (c == '\0')
      break;
  }

  if (tokenCount > 0) {
    for (int i = 0; i < NUM_NAME_TOKEN_BUCKETS; i++) {
      outBuckets[i] /= (float)tokenCount;
    }
  }
}

static char outBuf[BUF_SIZE];

int layerThree(char *outputPath) {
  char *oPath = outputPath ? outputPath : "moves.tsv";

  out = fopen(oPath, "w");
  if (!out) {
    fprintf(stderr, "Error: Could not open output file '%s'\n", oPath);
    return -1;
  }
  setvbuf(out, outBuf, _IOFBF, BUF_SIZE);
  fileVectorList = malloc(fileIdentityList.count * sizeof(FileVector));
  if (!fileVectorList) {
    fprintf(stderr, "Error: malloc failed for fileVectorList (%zu files)\n",
            fileIdentityList.count);
    fclose(out);
    return -1;
  }

  MoveCandidate *candidates =
      malloc(fileIdentityList.count * sizeof(MoveCandidate));
  float *distances = malloc(fileIdentityList.count * sizeof(float));
  if (!candidates || !distances) {
    fprintf(stderr,
            "Error: malloc failed for distance/candidate buffers (%zu files)\n",
            fileIdentityList.count);
    free(candidates);
    free(distances);
    fclose(out);
    return -1;
  }
  MUTEX_INIT(&lock);

  fprintf(stderr,
          "[layerThree] starting: %zu files, %zu examples, %d threads\n",
          fileIdentityList.count, exampleID->count, NUM_THREADS);

  struct timespec vt0, vt1;
  clock_gettime(CLOCK_MONOTONIC, &vt0);

  for (int i = 0; i < NUM_THREADS; i++) {
#ifdef _WIN32
    threads[i] =
        (HANDLE)_beginthreadex(NULL, 0, createVectorList, NULL, 0, NULL);
#else
    pthread_create(&threads[i], NULL, createVectorList, NULL);
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

  clock_gettime(CLOCK_MONOTONIC, &vt1);
  timeVectorize += elapsedSeconds(vt0, vt1);
  fprintf(stderr, "[layerThree] vectorization done in %.3fs\n", timeVectorize);
  for (int i = 0; i < exampleID->count; i++) {
    int exIdx = exampleID->id[i];
    int folderId = fileIdentityList.fileIdentities[exIdx].folderID;
    float nearest = INFINITY;
    for (int j = 0; j < exampleID->count; j++) {
      if (i == j)
        continue;
      int otherIdx = exampleID->id[j];
      if (fileIdentityList.fileIdentities[otherIdx].folderID != folderId)
        continue;
      float d = calculateDistanceSquared(&fileVectorList[exIdx],
                                         &fileVectorList[otherIdx]);
      if (d < nearest)
        nearest = d;
    }
    if (nearest == INFINITY)
      continue;
    CategoryBucket *bucket = getBucket(folderId);
    if (!bucket)
      bucket = addBucket(folderId);
    if (bucket)
      bucketDistanceAdd(bucket, nearest);
  }

  struct timespec mt0, mt1;
  clock_gettime(CLOCK_MONOTONIC, &mt0);

  size_t validCount = 0;
  for (size_t i = 0; i < fileIdentityList.count; i++) {
    float bestDist = INFINITY;
    int bestExample = -1;
    int bestFolderId = -1;
    float runnerUpDist = INFINITY;
    int isExample = 0;
    for (int j = 0; j < exampleID->count; j++) {
      if (exampleID->id[j] == i) {
        isExample = 1;
        continue;
      }

      float distance = calculateDistanceSquared(
          &fileVectorList[exampleID->id[j]], &fileVectorList[i]);
      int folderId = fileIdentityList.fileIdentities[exampleID->id[j]].folderID;

      if (distance < bestDist) {
        if (bestFolderId != -1 && bestFolderId != folderId &&
            bestDist < runnerUpDist)
          runnerUpDist = bestDist;

        bestDist = distance;
        bestExample = exampleID->id[j];
        bestFolderId = folderId;
      } else if (folderId != bestFolderId && distance < runnerUpDist) {
        runnerUpDist = distance;
      }
    }
    if (isExample) {
      candidates[i].bestExample = -1;
      continue;
    }
    candidates[i].bestExample = bestExample;
    candidates[i].bestDist = bestDist;
    candidates[i].runnerUpDist = runnerUpDist;
    if (bestExample != -1)
      distances[validCount++] = bestDist;
  }

  size_t rejectedCount = 0;
  size_t unsureCount = 0;

  mergeSort(distances, validCount);
  MedianData globalStats = getMedianData(distances, validCount);
  float globalRejectCeiling = globalStats.median; // or median + 1.5*IQR
  float globalAutoCeiling =
      globalStats.median -
      (globalStats.median / globalStats.Q3) * globalStats.IQR; // tune this

  for (size_t i = 0; i < bucketCount; i++) {
    mergeSort(buckets[i].distances, buckets[i].count);
    buckets[i].stats = getMedianData(buckets[i].distances, buckets[i].count);
  }

  float fallbackIQR = 0.0f;
  int wellPopulated = 0;
  for (size_t i = 0; i < bucketCount; i++) {
    if (buckets[i].count >= MIN_BUCKET_N_FOR_STATS) {
      fallbackIQR += buckets[i].stats.IQR;
      wellPopulated++;
    }
  }
  fallbackIQR =
      wellPopulated > 0 ? fallbackIQR / wellPopulated : globalStats.IQR;

  for (size_t i = 0; i < bucketCount; i++) {
    if (buckets[i].count < MIN_BUCKET_N_FOR_STATS) {
      float keepMedian = buckets[i].stats.median;
      buckets[i].stats.Q1 = fmax(0.0f, keepMedian - fallbackIQR * 0.5f);
      buckets[i].stats.Q3 = keepMedian + fallbackIQR * 0.5f;
      buckets[i].stats.IQR = fallbackIQR;
    }
    float realAuto =
        fmin(buckets[i].stats.median -
                 (buckets[i].stats.median / buckets[i].stats.Q3 / 2.0f) *
                     buckets[i].stats.IQR,
             globalAutoCeiling);
    if (realAuto < 0.0f)
      realAuto = 0.0f;
    float realReject = fmin(buckets[i].stats.Q3 + 0.5f * buckets[i].stats.IQR,
                            globalRejectCeiling);
    fprintf(stderr,
            "[layerThree] folder=%d n=%zu median=%.4f Q1=%.4f Q3=%.4f IQR=%.4f "
            "auto=%.4f reject=%.4f\n",
            buckets[i].folderID, buckets[i].count, buckets[i].stats.median,
            buckets[i].stats.Q1, buckets[i].stats.Q3, buckets[i].stats.IQR,
            realAuto, realReject);
    free(buckets[i].distances);
  }

  for (size_t i = 0; i < fileIdentityList.count; i++) {
    if (candidates[i].bestExample == -1) {
      unmatchedCount++;
      continue;
    }

    CategoryBucket *b = getBucket(
        fileIdentityList.fileIdentities[candidates[i].bestExample].folderID);
    if (!b) {
      rejectedCount++;
      continue;
    }
    MedianData medianData = b->stats;
    float autoMoveThreshold =
        fmin(medianData.median -
                 (medianData.median / medianData.Q3 / 2.0f) * medianData.IQR,
             globalAutoCeiling);
    if (autoMoveThreshold < 0.0f)
      autoMoveThreshold = 0.0f;
    float rejectThreshold = fmin(medianData.Q3 + 0.5f * medianData.IQR,
                                 globalRejectCeiling); // medianData.Q1;

    if (candidates[i].bestDist > rejectThreshold) {
      rejectedCount++;
      continue;
    }

    float margin = candidates[i].runnerUpDist - candidates[i].bestDist;
    int ambiguous = margin < 0.5f * medianData.IQR;

    char filePath[PATH_MAX];
    buildPath(&fileIdentityList.fileIdentities[i], filePath, sizeof(filePath));

    char exampleFilesFolder[PATH_MAX];
    buildFilePathToFolder(
        &fileIdentityList.fileIdentities[candidates[i].bestExample],
        exampleFilesFolder, sizeof(exampleFilesFolder));

    if (ambiguous || candidates[i].bestDist > autoMoveThreshold) {
      unsureCount++;
      fprintf(out, "%s ----> %s (Y/N)\n", filePath, exampleFilesFolder);
    } else {
      fprintf(out, "%s ----> %s\n", filePath, exampleFilesFolder);
    }
  }

  fprintf(
      stderr,
      "[layerThree] rejected as too far from any example: %zu / %zu (%.1f%%)\n",
      rejectedCount, validCount,
      validCount > 0 ? 100.0 * rejectedCount / validCount : 0.0);

  fprintf(
      stderr,
      "[layerThree] unsure as too far from any example: %zu / %zu (%.1f%%)\n",
      unsureCount, validCount,
      validCount > 0 ? 100.0 * unsureCount / validCount : 0.0);

  clock_gettime(CLOCK_MONOTONIC, &mt1);
  timeMatching += elapsedSeconds(mt0, mt1);

  MUTEX_DESTROY(&lock);
  fflush(out);
  fclose(out);
  free(fileVectorList);
  free(buckets);
  free(candidates);
  printLayerThreeTimings(fileIdentityList.count, (size_t)exampleID->count);
  return 0;
}

#ifdef _WIN32
unsigned __stdcall createVectorList(void *arg)
#else
void *createVectorList(void *arg)
#endif
{
  while (1) {
    size_t currentIndex;
    MUTEX_LOCK(&lock);
    if (nextIndex >= fileIdentityList.count) {
      MUTEX_UNLOCK(&lock);
      break;
    }
    currentIndex = nextIndex;
    nextIndex++;
    if (nextIndex % 5000 == 0 || nextIndex == fileIdentityList.count) {
      fprintf(stderr, "[layerThree]   vectorized %zu/%zu\n", nextIndex,
              fileIdentityList.count);
    }
    MUTEX_UNLOCK(&lock);
    FileIdentity *id = &fileIdentityList.fileIdentities[currentIndex];
    vectorize(id, &fileVectorList[currentIndex]);
  }
#ifdef _WIN32
  return 0;
#else
  return NULL;
#endif
}

void vectorize(FileIdentity *id, FileVector *fileVector) {
  fileVector->downloadedFromWeb = id->downloadedFromWeb;
  fileVector->isGenericDumpPath = id->isGenericDumpPath;
  fileVector->printableRatio = id->printableRatio;
  fileVector->whitespaceRatio = id->whitespaceRatio;
  fileVector->digitRatioInName = id->digitRatioInName;
  // 5

  fileVector->byteEntropy = (float)id->byteEntropy / 8.0f;
  fileVector->depth = (float)minMax(id->depth, 0, minMaxVals.maxDepth);
  fileVector->fileSize =
      (float)minMax(log(1 + id->fileSize), log(1 + minMaxVals.minFileSize),
                    log(1 + minMaxVals.maxFileSize));
  fileVector->cTomDeltaSeconds = (float)minMax(
      sign(id->cTomDeltaSeconds) * log(1 + fabs(id->cTomDeltaSeconds)),
      sign(minMaxVals.minCToMDeltaSeconds) *
          log(1 + fabs(minMaxVals.minCToMDeltaSeconds)),
      sign(minMaxVals.maxCToMDeltaSeconds) *
          log(1 + fabs(minMaxVals.maxCToMDeltaSeconds)));
  fileVector->hourOfDaySin = (float)sin(2.0 * PI * id->hourOfDay / 24.0);
  fileVector->hourOfDayCos = (float)cos(2.0 * PI * id->hourOfDay / 24.0);
  fileVector->dayOfWeekSin = (float)sin(2.0 * PI * id->dayOfWeek / 7.0);
  fileVector->dayOfWeekCos = (float)cos(2.0 * PI * id->dayOfWeek / 7.0);
  // 5+8 = 13

  memcpy(fileVector->byteFreqSketch, id->byteFreqSketch,
         sizeof(id->byteFreqSketch));
  memset(fileVector->namePatternOneHot, 0,
         sizeof(fileVector->namePatternOneHot));
  fileVector->namePatternOneHot[id->namePatternType] = 1.0f;

  fileVector->extTableIdx = id->extTableIdx;
  fileVector->magicTableIdx = id->magicTableIdx;

  // tags is a BITMASK -- a file can be TAG_CODE | TAG_APP_DATA at once --
  // so this needs a per-bit check, not a single index write.
  memset(fileVector->tagsOneHot, 0, sizeof(fileVector->tagsOneHot));
  for (int bit = 0; bit < NUM_TAG_BITS; bit++) {
    if (id->tags & (1 << bit))
      fileVector->tagsOneHot[bit] = 1.0f;
  }

  // 5 + 13 = 18

  vectorizeNameTokens(id->name, fileVector->nameTokenHash);
  // 1 + 18 = 19
}

float calculateDistanceSquared(FileVector *a, FileVector *b) {
  float sum = 0.0f;
  for (int i = 0; i < NUM_NAME_TOKEN_BUCKETS; i++)
    sum += ((a->nameTokenHash[i] - b->nameTokenHash[i]) *
            (a->nameTokenHash[i] - b->nameTokenHash[i])) /
           2.0f;
  for (int i = 0; i < NUM_TAG_BITS; i++)
    sum += ((a->tagsOneHot[i] - b->tagsOneHot[i]) *
            (a->tagsOneHot[i] - b->tagsOneHot[i])) /
           2.0f;

  if (a->extTableIdx != b->extTableIdx)
    sum += 1.0f;
  if (a->magicTableIdx != b->magicTableIdx)
    sum += 1.0f;

  for (int i = 0; i < HISTOGRAM_BINS; i++)
    sum += ((a->byteFreqSketch[i] - b->byteFreqSketch[i]) *
            (a->byteFreqSketch[i] - b->byteFreqSketch[i])) /
           2.0f;
  for (int i = 0; i < NUM_NAME_PATTERN_TYPE; i++)
    sum += ((a->namePatternOneHot[i] - b->namePatternOneHot[i]) *
            (a->namePatternOneHot[i] - b->namePatternOneHot[i])) /
           2.0f;

  sum += (a->downloadedFromWeb - b->downloadedFromWeb) *
             (a->downloadedFromWeb - b->downloadedFromWeb) +
         (a->isGenericDumpPath - b->isGenericDumpPath) *
             (a->isGenericDumpPath - b->isGenericDumpPath) +
         (a->depth - b->depth) * (a->depth - b->depth) +
         (a->byteEntropy - b->byteEntropy) * (a->byteEntropy - b->byteEntropy) +
         (a->printableRatio - b->printableRatio) *
             (a->printableRatio - b->printableRatio) +
         (a->whitespaceRatio - b->whitespaceRatio) *
             (a->whitespaceRatio - b->whitespaceRatio) +
         (a->digitRatioInName - b->digitRatioInName) *
             (a->digitRatioInName - b->digitRatioInName) +
         (a->fileSize - b->fileSize) * (a->fileSize - b->fileSize) +
         (a->cTomDeltaSeconds - b->cTomDeltaSeconds) *
             (a->cTomDeltaSeconds - b->cTomDeltaSeconds)

         + ((a->hourOfDaySin - b->hourOfDaySin) *
                (a->hourOfDaySin - b->hourOfDaySin) +
            (a->hourOfDayCos - b->hourOfDayCos) *
                (a->hourOfDayCos - b->hourOfDayCos)) /
               4.0f

         + ((a->dayOfWeekSin - b->dayOfWeekSin) *
                (a->dayOfWeekSin - b->dayOfWeekSin) +
            (a->dayOfWeekCos - b->dayOfWeekCos) *
                (a->dayOfWeekCos - b->dayOfWeekCos)) /
               4.0f;
  return sum;
}

double minMax(double val, double min, double max) {
  return max != min ? (val - min) / (max - min) : 0.0f;
}
