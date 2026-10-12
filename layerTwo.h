#ifndef LAYER2_H
#define LAYER2_H
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifndef NUM_THREADS
#define NUM_THREADS 12
#endif
#ifndef NUM_NAME_TOKEN_BUCKETS
#define NUM_NAME_TOKEN_BUCKETS 32
#endif

#define BUF_SIZE 1024 * 1024

// ── Layer 3 feature-vector constants ────────────────────────────────────────
// SAMPLE_SIZE: how many bytes we read from the FRONT of every file to work
//              out entropy / printable-ratio / histogram. We never read a
//              whole file -- that would be slow on big files and we don't
//              need it. The first 4KB is almost always enough to tell "this
//              looks like text" from "this looks like a compressed blob".
#define SAMPLE_SIZE 4096
// HISTOGRAM_BINS: a byte can be any value 0-255 (256 possible values). We
//                 don't want 256 separate buckets -- that's too fine-grained
//                 and noisy -- so we group them into 16 buckets of 16
//                 consecutive values each.
#define HISTOGRAM_BINS 16
// MAX_FILENAME_LEN: room for a filename (NOT a full path). PATH_MAX (4096 on
//                   Linux) is sized for an entire path, but id->name only
//                   ever holds the last piece -- buildPath() reconstructs
//                   the rest from the folder hierarchy. Every real
//                   filesystem (ext4, NTFS, APFS) caps a single filename at
//                   255 bytes, so 256 (255 + the null terminator) covers
//                   every real file with room to spare. Using PATH_MAX here
//                   instead was wasting ~3.8KB per file for nothing.
#define MAX_FILENAME_LEN 256

// ── Threading primitives (Windows / POSIX) ──────────────────────────────────
#ifdef _WIN32
#include <process.h>
#include <windows.h>
typedef CRITICAL_SECTION mutex_t;
typedef HANDLE thread_t;
#define MUTEX_INIT(m) InitializeCriticalSection(m)
#define MUTEX_DESTROY(m) DeleteCriticalSection(m)
#define MUTEX_LOCK(m) EnterCriticalSection(m)
#define MUTEX_UNLOCK(m) LeaveCriticalSection(m)
#else
#include <pthread.h>
typedef pthread_mutex_t mutex_t;
typedef pthread_t thread_t;
#define MUTEX_INIT(m) pthread_mutex_init(m, NULL)
#define MUTEX_DESTROY(m) pthread_mutex_destroy(m)
#define MUTEX_LOCK(m) pthread_mutex_lock(m)
#define MUTEX_UNLOCK(m) pthread_mutex_unlock(m)
#endif

// ── Tags (bitmask so a file can hold multiple) ──────────────────────────────
typedef enum {
  TAG_NONE = 0,
  TAG_CODE = 1 << 0,
  TAG_BINARY = 1 << 1,
  TAG_DOCUMENT = 1 << 2,
  TAG_MEDIA = 1 << 3,
  TAG_TEMP = 1 << 4,
  TAG_ARCHIVE = 1 << 5,
  TAG_SHORTCUT = 1 << 6,
  TAG_SYSTEM_FILE = 1 << 7,
  TAG_DISGUISED = 1 << 8,
  TAG_UNKNOWN = 1 << 9,
  TAG_APP_DATA = 1 << 10
} FileTag;

#define NUM_TAG_BITS 11

typedef enum {
  NAME_NORMAL = 0,
  NAME_TIMESTAMP_LIKE = 1, // heavy digit ratio, digit runs w/ separators
  NAME_HASH_LIKE = 2,      // long run of hex/alnum, low separator count
  NAME_ALL_CAPS = 3
} NamePatternType;

#define NUM_NAME_PATTERN_TYPE 4

// ── The core output of Layer 2 ──────────────────────────────────────────────
typedef struct {
  char name[MAX_FILENAME_LEN];
  int fileID;
  int folderID;
  uint16_t tags;
  // char extension[32];
  int16_t extTableIdx;
  int16_t magicTableIdx;
  bool downloadedFromWeb
      : 1; // 1 if the OS marked this as downloaded from the internet, else 0
  bool isGenericDumpPath
      : 1; // 1 if the file lives in a Downloads/Desktop/Temp folder, else 0
  uint8_t depth;

  float byteEntropy;    // "how random do the bytes look?" 0.0 (all one value)
                        // to 8.0 (totally random)
  float printableRatio; // "how much of it is readable text?" 0.0 (none) to 1.0
                        // (all of it)
  float byteFreqSketch[HISTOGRAM_BINS]; // "what does the byte value
                                        // distribution look like?" 16 numbers
                                        // that add up to ~1.0
  float whitespaceRatio;
  uint8_t headerBytes[8];
  float digitRatioInName;
  uint8_t
      namePatternType; // enum: normal / timestamp-like / hash-like / all-caps
  long long fileSize;

  uint64_t burstSessionBucket; // "which 15-minute window was this saved in?"
                               // mtime chopped into 900-second chunks
  long mtime;
  long ctime;
  long cTomDeltaSeconds;
  uint8_t hourOfDay;
  uint8_t dayOfWeek;
} FileIdentity;

typedef struct {
  uint8_t maxDepth;
  long long minFileSize;
  long long maxFileSize;
  long minCToMDeltaSeconds;
  long maxCToMDeltaSeconds;
} MinMaxValues;

typedef struct {
  FileIdentity *fileIdentities;
  size_t count;
  size_t capacity;
} FileIdentityArrayList;

extern FileIdentityArrayList fileIdentityList;
extern MinMaxValues minMaxVals;
// ── Public interface ────────────────────────────────────────────────────────
#ifdef _WIN32
unsigned __stdcall processFile(void *arg);
#else
void *processFile(void *arg);
#endif

void flushMinMax(MinMaxValues *mine);
void buildPath(FileIdentity *id, char *buffer, size_t bufferSize);
void buildFilePathToFolder(FileIdentity *id, char *buffer, size_t bufferSize);
char *getFileExtension(char *filePath);
void checkExtention(FileIdentity *id);
void getParentDir(const char *path, char *out, size_t outSize);
void writeIdentity(FILE *out, FileIdentity *id);
void getFileTag(int tags, char *out, size_t outSize);
int layerTwo(char *outputPath, int deepScanFlag);

// ── Layer 3 feature gathering ──────────────────────────────────────────────
void extractFileFeatures(FileIdentity *id, double *webOriginTime,
                         double *fileIOTime, double *contentMathTime,
                         double *magicBytesTime);
// void extractFileFeatures(FileIdentity *id);
uint8_t checkWebOrigin(const char *filePath);
void extractContextFeatures(const char *filePath, FileIdentity *id);

// ── Magic byte table ────────────────────────────────────────────────────────
typedef struct {
  uint8_t sig[8];
  size_t sigLen;
  const char *trueType;
  FileTag tag;
} MagicEntry;

// ── Extension table ─────────────────────────────────────────────────────────
typedef struct {
  const char *ext;
  FileTag tag;
  const char *trueType;
} ExtEntry;

#define EXT_TABLE_LIST                                                         \
  X("c", TAG_CODE, "C_SOURCE")                                                 \
  X("h", TAG_CODE, "C_HEADER")                                                 \
  X("cpp", TAG_CODE, "CPP_SOURCE")                                             \
  X("py", TAG_CODE, "PYTHON")                                                  \
  X("js", TAG_CODE, "JAVASCRIPT")                                              \
  X("ts", TAG_CODE, "TYPESCRIPT")                                              \
  X("java", TAG_CODE, "JAVA")                                                  \
  X("rs", TAG_CODE, "RUST")                                                    \
  X("go", TAG_CODE, "GO")                                                      \
  X("sh", TAG_CODE, "SHELL")                                                   \
  X("bat", TAG_CODE, "BATCH")                                                  \
  X("html", TAG_CODE, "HTML")                                                  \
  X("htm", TAG_CODE, "HTML")                                                   \
  X("css", TAG_CODE, "CSS")                                                    \
  X("json", TAG_CODE, "JSON")                                                  \
  X("yml", TAG_CODE, "YAML")                                                   \
  X("yaml", TAG_CODE, "YAML")                                                  \
  X("xml", TAG_CODE, "XML")                                                    \
  X("toml", TAG_CODE, "TOML")                                                  \
  X("ini", TAG_APP_DATA, "INI")                                                \
  X("cfg", TAG_APP_DATA, "CONFIG")                                             \
  X("conf", TAG_APP_DATA, "CONFIG")                                            \
  X("pdf", TAG_DOCUMENT, "PDF")                                                \
  X("md", TAG_DOCUMENT, "MD")                                                  \
  X("txt", TAG_DOCUMENT, "TXT")                                                \
  X("docx", TAG_DOCUMENT, "DOCX")                                              \
  X("xlsx", TAG_DOCUMENT, "XLSX")                                              \
  X("pptx", TAG_DOCUMENT, "PPTX")                                              \
  X("rtf", TAG_DOCUMENT, "RTF")                                                \
  X("csv", TAG_DOCUMENT, "CSV")                                                \
  X("epub", TAG_DOCUMENT, "EPUB")                                              \
  X("png", TAG_MEDIA, "PNG")                                                   \
  X("jpg", TAG_MEDIA, "JPEG")                                                  \
  X("jpeg", TAG_MEDIA, "JPEG")                                                 \
  X("gif", TAG_MEDIA, "GIF")                                                   \
  X("mp4", TAG_MEDIA, "MP4")                                                   \
  X("mp3", TAG_MEDIA, "MP3")                                                   \
  X("wav", TAG_MEDIA, "WAV")                                                   \
  X("ico", TAG_MEDIA, "ICO")                                                   \
  X("jxr", TAG_MEDIA, "JPEG_XR")                                               \
  X("webp", TAG_MEDIA, "WEBP")                                                 \
  X("svg", TAG_MEDIA, "SVG")                                                   \
  X("mkv", TAG_MEDIA, "MKV")                                                   \
  X("avi", TAG_MEDIA, "AVI")                                                   \
  X("mov", TAG_MEDIA, "MOV")                                                   \
  X("flac", TAG_MEDIA, "FLAC")                                                 \
  X("zip", TAG_ARCHIVE, "ZIP")                                                 \
  X("gz", TAG_ARCHIVE, "GZIP")                                                 \
  X("tar", TAG_ARCHIVE, "TAR")                                                 \
  X("7z", TAG_ARCHIVE, "7ZIP")                                                 \
  X("o", TAG_TEMP, "OBJ")                                                      \
  X("exe", TAG_BINARY, "WINDOWS_EXE")                                          \
  X("sb3", TAG_APP_DATA, "SCRATCH")                                            \
  X("pub", TAG_APP_DATA, "SSH_PUBKEY")                                         \
  X("pem", TAG_APP_DATA, "PEM_KEY")                                            \
  X("pyc", TAG_TEMP, "PYC")                                                    \
  X("class", TAG_TEMP, "JAVA_CLASS")                                           \
  X("lnk", TAG_SHORTCUT, "SHORTCUT")                                           \
  X("url", TAG_SHORTCUT, "URL")                                                \
  X("search-ms", TAG_SYSTEM_FILE, "WIN_SEARCH")                                \
  X("searchconnector-ms", TAG_SYSTEM_FILE, "WIN_SEARCH")

// X(sig_bytes, sigLen, trueType, tag) -- sig_bytes must go through SIG(...),
// not bare {...}. Braces don't stop the preprocessor from splitting on
// commas (only parentheses do) -- {0x4D,0x5A} as a bare argument would be
// seen as TWO arguments, not one. SIG(...)'s own parentheses keep the
// commas inside from being treated as argument separators for X(...).
#define SIG(...) {__VA_ARGS__}
#define MAGIC_TABLE_LIST                                                       \
  X(SIG(0x4D, 0x5A), 2, "WINDOWS_EXE", TAG_BINARY)                             \
  X(SIG(0x7F, 0x45, 0x4C, 0x46), 4, "ELF", TAG_BINARY)                         \
  X(SIG(0x25, 0x50, 0x44, 0x46), 4, "PDF", TAG_DOCUMENT)                       \
  X(SIG(0x89, 0x50, 0x4E, 0x47), 4, "PNG", TAG_MEDIA)                          \
  X(SIG(0xFF, 0xD8, 0xFF), 3, "JPEG", TAG_MEDIA)                               \
  X(SIG(0x47, 0x49, 0x46), 3, "GIF", TAG_MEDIA)                                \
  X(SIG(0x49, 0x49, 0xBC, 0x01), 4, "JPEG_XR", TAG_MEDIA)                      \
  X(SIG(0x50, 0x4B, 0x03, 0x04), 4, "ZIP", TAG_ARCHIVE)                        \
  X(SIG(0x1F, 0x8B), 2, "GZIP", TAG_ARCHIVE)                                   \
  X(SIG(0x42, 0x5A, 0x68), 3, "BZIP2", TAG_ARCHIVE)                            \
  X(SIG(0x23, 0x21), 2, "SCRIPT", TAG_CODE)                                    \
  X(SIG(0xD0, 0xCF, 0x11, 0xE0), 4, "OLE2", TAG_DOCUMENT)                      \
  X(SIG(0x52, 0x49, 0x46, 0x46), 4, "RIFF", TAG_MEDIA)                         \
  X(SIG(0x49, 0x44, 0x33), 3, "MP3", TAG_MEDIA)

// EXT_TABLE_LIST/MAGIC_TABLE_LIST don't include the sentinel row -- every
// consumer (the array definition in layerTwo.c, and the counts below) adds
// it separately, so there's exactly one place that could get it wrong
// instead of two arrays that could drift apart.
//
// Wrapped in an enum (not a plain #define) so the count is fully resolved
// to a real integer constant RIGHT HERE -- macro expansion is lazy, so a
// plain "#define NUM_EXT_ENTRIES (EXT_TABLE_LIST)" would break the moment
// something used it after the #undef X below, since X wouldn't exist
// anymore at that later expansion point.
#define X(ext, tag, trueType) +1
enum { NUM_EXT_ENTRIES = 0 EXT_TABLE_LIST };
#undef X
#define X(sig, sigLen, trueType, tag) +1
enum { NUM_MAGIC_ENTRIES = 0 MAGIC_TABLE_LIST };
#undef X

extern const MagicEntry MAGIC_TABLE[];
extern const ExtEntry EXT_TABLE[];

typedef struct {
  int *id;
  size_t count;
  size_t capacity;
} ExampleRegistry;

extern ExampleRegistry *exampleID;
#endif
