#ifndef LAYER3_H
#define LAYER3_H
#endif
#include "layerTwo.h"

#define MIN_BUCKET_N_FOR_STATS 5 
#define TOTAL_VECTOR_DIM (NUM_NAME_TOKEN_BUCKETS + NUM_TAG_BITS + NUM_EXT_ENTRIES + NUM_MAGIC_ENTRIES + HISTOGRAM_BINS + NUM_NAME_PATTERN_TYPE + 13)
#define sign(x) ((x > 0) - (x < 0))
static const double PI = 3.14159265358979323846;

typedef struct{
    float nameTokenHash[NUM_NAME_TOKEN_BUCKETS];
    float tagsOneHot[NUM_TAG_BITS];
    int16_t extTableIdx;
    int16_t magicTableIdx;
    bool downloadedFromWeb : 1;               // 1 if the OS marked this as downloaded from the internet, else 0
    bool isGenericDumpPath : 1;               // 1 if the file lives in a Downloads/Desktop/Temp folder, else 0
    float depth;
    
    float byteEntropy;                     // "how random do the bytes look?" 0.0 (all one value) to 8.0 (totally random)
    float printableRatio;                  // "how much of it is readable text?" 0.0 (none) to 1.0 (all of it)
    float byteFreqSketch[HISTOGRAM_BINS];  // "what does the byte value distribution look like?" 16 numbers that add up to ~1.0
    float whitespaceRatio;
    float digitRatioInName;
    float namePatternOneHot[NUM_NAME_PATTERN_TYPE];   // enum: normal / timestamp-like / hash-like / all-caps
    float fileSize;

    float cTomDeltaSeconds;
    float hourOfDaySin;
    float hourOfDayCos;
    float dayOfWeekSin;
    float dayOfWeekCos;
} FileVector;

int layerThree(char *outputPath);
void vectorize(FileIdentity *id, FileVector *fileVector);
double minMax(double val, double min, double max);
float calculateDistanceSquared(FileVector *a, FileVector *b);
#ifdef _WIN32
unsigned __stdcall createVectorList(void *arg);
#else
void *createVectorList(void *arg);
#endif