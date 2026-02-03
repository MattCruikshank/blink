#ifndef BLINK_ZIPFS_H_
#define BLINK_ZIPFS_H_

#include <dirent.h>
#include <sys/types.h>
#include <sys/uio.h>

#include "blink/vfs.h"

// ZipfsInfo holds per-file/directory state for zipfs nodes.
// Each open file or directory gets its own ZipfsInfo.
struct ZipfsInfo {
  int mode;           // S_IFDIR or S_IFREG
  int filefd;         // Open file descriptor for reads (-1 if not open)
  DIR *dirstream;     // Open directory stream for readdir (NULL if not open)
  char *hostpath;     // Full host path: "/zip/apps/foo/..."
};

int ZipfsInit(const char *, u64, const void *, struct VfsDevice **,
              struct VfsMount **);
int ZipfsCreateInfo(struct ZipfsInfo **);
int ZipfsFreeInfo(void *);
int ZipfsFreeDevice(void *);
int ZipfsFinddir(struct VfsInfo *, const char *, struct VfsInfo **);
int ZipfsOpen(struct VfsInfo *, const char *, int, int, struct VfsInfo **);
int ZipfsAccess(struct VfsInfo *, const char *, mode_t, int);
int ZipfsStat(struct VfsInfo *, const char *, struct stat *, int);
int ZipfsFstat(struct VfsInfo *, struct stat *);
int ZipfsClose(struct VfsInfo *);
ssize_t ZipfsRead(struct VfsInfo *, void *, size_t);
ssize_t ZipfsReadv(struct VfsInfo *, const struct iovec *, int);
ssize_t ZipfsPread(struct VfsInfo *, void *, size_t, off_t);
off_t ZipfsSeek(struct VfsInfo *, off_t, int);
int ZipfsOpendir(struct VfsInfo *, struct VfsInfo **);
struct dirent *ZipfsReaddir(struct VfsInfo *);
void ZipfsRewinddir(struct VfsInfo *);
#ifdef HAVE_SEEKDIR
void ZipfsSeekdir(struct VfsInfo *, long);
long ZipfsTelldir(struct VfsInfo *);
#endif
int ZipfsClosedir(struct VfsInfo *);
ssize_t ZipfsReadlink(struct VfsInfo *, char **);

extern struct VfsSystem g_zipfs;

#endif  // BLINK_ZIPFS_H_
