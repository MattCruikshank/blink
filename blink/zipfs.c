/*-*- mode:c;indent-tabs-mode:nil;c-basic-offset:2;tab-width:8;coding:utf-8 -*-│
│ vi: set et ft=c ts=2 sts=2 sw=2 fenc=utf-8                               :vi │
╞══════════════════════════════════════════════════════════════════════════════╡
│ Copyright 2024 Portator Authors                                              │
│                                                                              │
│ Permission to use, copy, modify, and/or distribute this software for         │
│ any purpose with or without fee is hereby granted, provided that the         │
│ above copyright notice and this permission notice appear in all copies.      │
│                                                                              │
│ THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL                │
│ WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED                │
│ WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE             │
│ AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL         │
│ DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR        │
│ PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER               │
│ TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR             │
│ PERFORMANCE OF THIS SOFTWARE.                                                │
╚─────────────────────────────────────────────────────────────────────────────*/
#include "blink/zipfs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include "blink/assert.h"
#include "blink/errno.h"
#include "blink/log.h"
#include "blink/vfs.h"

#ifndef DISABLE_VFS

// ZipfsDevice holds the source path for the mounted zip filesystem.
struct ZipfsDevice {
  const char *source;  // Host path to the zip root (e.g., "/zip")
  size_t sourcelen;
};

static u64 ZipfsHash(u64 parent, const char *data, size_t size) {
  u64 hash;
  if (data == NULL) {
    efault();
    return 0;
  }
  hash = parent;
  while (size--) {
    hash = *data++ + (hash << 6) + (hash << 16) - hash;
  }
  return hash;
}

int ZipfsInit(const char *source, u64 flags, const void *data,
              struct VfsDevice **device, struct VfsMount **mount) {
  struct ZipfsDevice *zipdevice;
  struct ZipfsInfo *zipfsrootinfo;
  struct stat st;
  const char *src;

  // Default to "/zip" if no source provided
  src = (source && source[0]) ? source : "/zip";

  if (stat(src, &st) == -1) {
    return -1;
  }
  if (!S_ISDIR(st.st_mode)) {
    return enotdir();
  }

  zipdevice = NULL;
  zipfsrootinfo = NULL;
  *device = NULL;
  *mount = NULL;

  zipdevice = (struct ZipfsDevice *)malloc(sizeof(struct ZipfsDevice));
  if (zipdevice == NULL) {
    return enomem();
  }
  zipdevice->source = strdup(src);
  if (zipdevice->source == NULL) {
    goto cleananddie;
  }
  zipdevice->sourcelen = strlen(zipdevice->source);
  // Remove trailing slash if present
  if (zipdevice->sourcelen > 0 &&
      zipdevice->source[zipdevice->sourcelen - 1] == '/') {
    zipdevice->sourcelen--;
  }

  if (VfsCreateDevice(device) == -1) {
    goto cleananddie;
  }
  (*device)->data = zipdevice;
  (*device)->ops = &g_zipfs.ops;

  *mount = (struct VfsMount *)malloc(sizeof(struct VfsMount));
  if (*mount == NULL) {
    goto cleananddie;
  }

  if (VfsCreateInfo(&(*mount)->root) == -1) {
    goto cleananddie;
  }
  unassert(!VfsAcquireDevice(*device, &(*mount)->root->device));

  if (ZipfsCreateInfo(&zipfsrootinfo) == -1) {
    goto cleananddie;
  }
  zipfsrootinfo->hostpath = strdup(src);
  if (zipfsrootinfo->hostpath == NULL) {
    goto cleananddie;
  }
  zipfsrootinfo->mode = st.st_mode;

  (*mount)->root->data = zipfsrootinfo;
  (*mount)->root->mode = st.st_mode;
  (*mount)->root->ino =
      ZipfsHash(st.st_dev, (const char *)&st.st_ino, sizeof(st.st_ino));

  // Weak reference
  (*device)->root = (*mount)->root;

  VFS_LOGF("Mounted a zipfs device for \"%s\"", src);
  return 0;

cleananddie:
  if (*device) {
    unassert(!VfsFreeDevice(*device));
  } else {
    if (zipdevice) {
      free((void *)zipdevice->source);
      free(zipdevice);
    }
  }
  if (*mount) {
    if ((*mount)->root) {
      unassert(!VfsFreeInfo((*mount)->root));
    } else {
      free(zipfsrootinfo->hostpath);
      free(zipfsrootinfo);
    }
    free(*mount);
  }
  return -1;
}

int ZipfsCreateInfo(struct ZipfsInfo **output) {
  *output = (struct ZipfsInfo *)malloc(sizeof(struct ZipfsInfo));
  if (!*output) {
    return enomem();
  }
  (*output)->mode = 0;
  (*output)->filefd = -1;
  (*output)->dirstream = NULL;
  (*output)->hostpath = NULL;
  return 0;
}

int ZipfsFreeInfo(void *info) {
  struct ZipfsInfo *zipfsinfo = (struct ZipfsInfo *)info;
  if (info == NULL) {
    return 0;
  }
  VFS_LOGF("ZipfsFreeInfo(%p)", info);
  if (zipfsinfo->dirstream) {
    unassert(!closedir(zipfsinfo->dirstream));
  }
  if (zipfsinfo->filefd != -1) {
    unassert(!close(zipfsinfo->filefd));
  }
  free(zipfsinfo->hostpath);
  free(info);
  return 0;
}

int ZipfsFreeDevice(void *device) {
  struct ZipfsDevice *zipfsdevice = (struct ZipfsDevice *)device;
  if (device == NULL) {
    return 0;
  }
  VFS_LOGF("ZipfsFreeDevice(%p)", device);
  free((void *)zipfsdevice->source);
  free(zipfsdevice);
  return 0;
}

// Build the host path for a child entry
static char *ZipfsBuildHostPath(struct ZipfsInfo *parent, const char *name) {
  size_t parentlen, namelen;
  char *path;

  if (parent->hostpath == NULL) {
    return NULL;
  }
  parentlen = strlen(parent->hostpath);
  namelen = strlen(name);

  // +2 for '/' and null terminator
  path = (char *)malloc(parentlen + 1 + namelen + 1);
  if (path == NULL) {
    return NULL;
  }

  memcpy(path, parent->hostpath, parentlen);
  path[parentlen] = '/';
  memcpy(path + parentlen + 1, name, namelen);
  path[parentlen + 1 + namelen] = '\0';

  return path;
}

int ZipfsFinddir(struct VfsInfo *parent, const char *name,
                 struct VfsInfo **output) {
  struct ZipfsInfo *parentinfo;
  struct ZipfsInfo *outputinfo;
  struct stat st;
  char *hostpath;

  VFS_LOGF("ZipfsFinddir(%p, \"%s\", %p)", parent, name, output);

  if (parent == NULL || name == NULL || output == NULL) {
    efault();
    return -1;
  }
  if (!S_ISDIR(parent->mode)) {
    enotdir();
    return -1;
  }

  *output = NULL;
  outputinfo = NULL;
  parentinfo = (struct ZipfsInfo *)parent->data;

  // Build the host path for this entry
  hostpath = ZipfsBuildHostPath(parentinfo, name);
  if (hostpath == NULL) {
    enomem();
    return -1;
  }

  // Stat the entry on the host
  VFS_LOGF("ZipfsFinddir: trying stat(\"%s\")", hostpath);
  if (stat(hostpath, &st) == -1) {
    VFS_LOGF("ZipfsFinddir: stat(\"%s\") failed (%d: %s)", hostpath, errno,
             strerror(errno));
    free(hostpath);
    return -1;
  }
  VFS_LOGF("ZipfsFinddir: stat(\"%s\") succeeded, mode=0%o", hostpath,
           st.st_mode);

  if (ZipfsCreateInfo(&outputinfo) == -1) {
    free(hostpath);
    return -1;
  }
  outputinfo->mode = st.st_mode;
  outputinfo->hostpath = hostpath;
  outputinfo->filefd = -1;

  if (VfsCreateInfo(output) == -1) {
    ZipfsFreeInfo(outputinfo);
    return -1;
  }

  (*output)->name = strdup(name);
  if ((*output)->name == NULL) {
    enomem();
    unassert(!VfsFreeInfo(*output));
    ZipfsFreeInfo(outputinfo);
    return -1;
  }
  (*output)->namelen = strlen(name);
  (*output)->data = outputinfo;
  unassert(!VfsAcquireDevice(parent->device, &(*output)->device));
  (*output)->dev = parent->dev;
  (*output)->ino =
      ZipfsHash(st.st_dev, (const char *)&st.st_ino, sizeof(st.st_ino));
  (*output)->mode = st.st_mode;
  (*output)->refcount = 1;
  unassert(!VfsAcquireInfo(parent, &(*output)->parent));

  return 0;
}

int ZipfsOpen(struct VfsInfo *parent, const char *name, int flags, int mode,
              struct VfsInfo **output) {
  struct ZipfsInfo *parentinfo;
  struct ZipfsInfo *outputinfo;
  struct stat st;
  char *hostpath;
  int fd;

  VFS_LOGF("ZipfsOpen(%p, \"%s\", %d, %d, %p)", parent, name, flags, mode,
           output);

  if (parent == NULL || name == NULL || output == NULL) {
    return efault();
  }
  if (!S_ISDIR(parent->mode)) {
    return enotdir();
  }

  // Only allow read-only access
  if ((flags & O_ACCMODE) != O_RDONLY) {
    return eacces();
  }
  // Reject creation/truncation flags
  if (flags & (O_CREAT | O_TRUNC | O_APPEND)) {
    return eacces();
  }

  *output = NULL;
  outputinfo = NULL;
  parentinfo = (struct ZipfsInfo *)parent->data;

  // Build the host path
  hostpath = ZipfsBuildHostPath(parentinfo, name);
  if (hostpath == NULL) {
    return enomem();
  }

  // Open the file on the host
  fd = open(hostpath, O_RDONLY);
  if (fd == -1) {
    VFS_LOGF("ZipfsOpen: open(\"%s\", O_RDONLY) failed (%d)", hostpath, errno);
    free(hostpath);
    return -1;
  }

  if (fstat(fd, &st) == -1) {
    close(fd);
    free(hostpath);
    return -1;
  }

  if (ZipfsCreateInfo(&outputinfo) == -1) {
    close(fd);
    free(hostpath);
    return -1;
  }
  outputinfo->mode = st.st_mode;
  outputinfo->hostpath = hostpath;
  outputinfo->filefd = fd;

  if (VfsCreateInfo(output) == -1) {
    ZipfsFreeInfo(outputinfo);
    return -1;
  }

  (*output)->name = strdup(name);
  if ((*output)->name == NULL) {
    enomem();
    unassert(!VfsFreeInfo(*output));
    ZipfsFreeInfo(outputinfo);
    return -1;
  }
  (*output)->namelen = strlen(name);
  (*output)->data = outputinfo;
  unassert(!VfsAcquireDevice(parent->device, &(*output)->device));
  (*output)->dev = parent->dev;
  (*output)->ino =
      ZipfsHash(st.st_dev, (const char *)&st.st_ino, sizeof(st.st_ino));
  (*output)->mode = st.st_mode;
  (*output)->refcount = 1;
  unassert(!VfsAcquireInfo(parent, &(*output)->parent));

  return 0;
}

int ZipfsAccess(struct VfsInfo *parent, const char *name, mode_t mode,
                int flags) {
  struct ZipfsInfo *parentinfo;
  char *hostpath;
  int ret;

  VFS_LOGF("ZipfsAccess(%p, \"%s\", %d, %d)", parent, name, mode, flags);

  if (parent == NULL || name == NULL) {
    return efault();
  }

  // Deny write access
  if (mode & W_OK) {
    return eacces();
  }

  parentinfo = (struct ZipfsInfo *)parent->data;
  hostpath = ZipfsBuildHostPath(parentinfo, name);
  if (hostpath == NULL) {
    return enomem();
  }

  ret = access(hostpath, mode);
  free(hostpath);
  return ret;
}

int ZipfsStat(struct VfsInfo *parent, const char *name, struct stat *st,
              int flags) {
  struct ZipfsInfo *parentinfo;
  char *hostpath;
  int ret;

  VFS_LOGF("ZipfsStat(%p, \"%s\", %p, %d)", parent, name, st, flags);

  if (parent == NULL || name == NULL || st == NULL) {
    return efault();
  }

  parentinfo = (struct ZipfsInfo *)parent->data;
  hostpath = ZipfsBuildHostPath(parentinfo, name);
  if (hostpath == NULL) {
    return enomem();
  }

  if (flags & AT_SYMLINK_NOFOLLOW) {
    ret = lstat(hostpath, st);
  } else {
    ret = stat(hostpath, st);
  }

  if (ret != -1) {
    st->st_ino =
        ZipfsHash(st->st_dev, (const char *)&st->st_ino, sizeof(st->st_ino));
    st->st_dev = parent->dev;
  }

  free(hostpath);
  return ret;
}

int ZipfsFstat(struct VfsInfo *info, struct stat *st) {
  struct ZipfsInfo *zipinfo;
  int ret;

  VFS_LOGF("ZipfsFstat(%p, %p)", info, st);

  if (info == NULL || st == NULL) {
    return efault();
  }

  zipinfo = (struct ZipfsInfo *)info->data;

  if (zipinfo->filefd != -1) {
    ret = fstat(zipinfo->filefd, st);
  } else if (zipinfo->hostpath != NULL) {
    ret = stat(zipinfo->hostpath, st);
  } else {
    return ebadf();
  }

  if (ret != -1) {
    st->st_ino =
        ZipfsHash(st->st_dev, (const char *)&st->st_ino, sizeof(st->st_ino));
    st->st_dev = info->dev;
  }

  return ret;
}

int ZipfsClose(struct VfsInfo *info) {
  struct ZipfsInfo *zipinfo;
  int ret;

  VFS_LOGF("ZipfsClose(%p)", info);

  if (info == NULL) {
    return efault();
  }

  zipinfo = (struct ZipfsInfo *)info->data;
  if (zipinfo->filefd == -1) {
    return ebadf();
  }

  ret = close(zipinfo->filefd);
  zipinfo->filefd = -1;
  return ret;
}

ssize_t ZipfsRead(struct VfsInfo *info, void *buf, size_t size) {
  struct ZipfsInfo *zipinfo;

  VFS_LOGF("ZipfsRead(%p, %p, %ld)", info, buf, size);

  if (info == NULL || buf == NULL) {
    return efault();
  }

  zipinfo = (struct ZipfsInfo *)info->data;
  if (zipinfo->filefd == -1) {
    return ebadf();
  }

  return read(zipinfo->filefd, buf, size);
}

ssize_t ZipfsReadv(struct VfsInfo *info, const struct iovec *iov, int iovcnt) {
  struct ZipfsInfo *zipinfo;

  VFS_LOGF("ZipfsReadv(%p, %p, %d)", info, iov, iovcnt);

  if (info == NULL || iov == NULL) {
    return efault();
  }

  zipinfo = (struct ZipfsInfo *)info->data;
  if (zipinfo->filefd == -1) {
    return ebadf();
  }

  return readv(zipinfo->filefd, iov, iovcnt);
}

ssize_t ZipfsPread(struct VfsInfo *info, void *buf, size_t size, off_t offset) {
  struct ZipfsInfo *zipinfo;

  VFS_LOGF("ZipfsPread(%p, %p, %ld, %ld)", info, buf, size, offset);

  if (info == NULL || buf == NULL) {
    return efault();
  }

  zipinfo = (struct ZipfsInfo *)info->data;
  if (zipinfo->filefd == -1) {
    return ebadf();
  }

  return pread(zipinfo->filefd, buf, size, offset);
}

off_t ZipfsSeek(struct VfsInfo *info, off_t offset, int whence) {
  struct ZipfsInfo *zipinfo;

  VFS_LOGF("ZipfsSeek(%p, %ld, %d)", info, offset, whence);

  if (info == NULL) {
    return efault();
  }

  zipinfo = (struct ZipfsInfo *)info->data;
  if (zipinfo->filefd == -1) {
    return ebadf();
  }

  return lseek(zipinfo->filefd, offset, whence);
}

int ZipfsOpendir(struct VfsInfo *info, struct VfsInfo **output) {
  struct ZipfsInfo *zipinfo;
  DIR *dirstream;

  VFS_LOGF("ZipfsOpendir(%p, %p)", info, output);

  if (info == NULL || output == NULL) {
    return efault();
  }

  zipinfo = (struct ZipfsInfo *)info->data;

  if (!S_ISDIR(zipinfo->mode)) {
    return enotdir();
  }

  if (zipinfo->hostpath == NULL) {
    return ebadf();
  }

  dirstream = opendir(zipinfo->hostpath);
  if (dirstream == NULL) {
    return -1;
  }

  zipinfo->dirstream = dirstream;
  unassert(!VfsAcquireInfo(info, output));
  return 0;
}

struct dirent *ZipfsReaddir(struct VfsInfo *info) {
  struct ZipfsInfo *zipinfo;

  VFS_LOGF("ZipfsReaddir(%p)", info);

  if (info == NULL) {
    efault();
    return NULL;
  }

  zipinfo = (struct ZipfsInfo *)info->data;
  if (zipinfo->dirstream == NULL) {
    return NULL;
  }

  return readdir(zipinfo->dirstream);
}

void ZipfsRewinddir(struct VfsInfo *info) {
  struct ZipfsInfo *zipinfo;

  VFS_LOGF("ZipfsRewinddir(%p)", info);

  if (info == NULL) {
    efault();
    return;
  }

  zipinfo = (struct ZipfsInfo *)info->data;
  if (zipinfo->dirstream != NULL) {
    rewinddir(zipinfo->dirstream);
  }
}

#ifdef HAVE_SEEKDIR
void ZipfsSeekdir(struct VfsInfo *info, long loc) {
  struct ZipfsInfo *zipinfo;

  VFS_LOGF("ZipfsSeekdir(%p, %ld)", info, loc);

  if (info == NULL) {
    efault();
    return;
  }

  zipinfo = (struct ZipfsInfo *)info->data;
  if (zipinfo->dirstream != NULL) {
    seekdir(zipinfo->dirstream, loc);
  }
}

long ZipfsTelldir(struct VfsInfo *info) {
  struct ZipfsInfo *zipinfo;

  VFS_LOGF("ZipfsTelldir(%p)", info);

  if (info == NULL) {
    efault();
    return -1;
  }

  zipinfo = (struct ZipfsInfo *)info->data;
  if (zipinfo->dirstream == NULL) {
    return ebadf();
  }

  return telldir(zipinfo->dirstream);
}
#endif

int ZipfsClosedir(struct VfsInfo *info) {
  struct ZipfsInfo *zipinfo;

  VFS_LOGF("ZipfsClosedir(%p)", info);

  if (info == NULL) {
    return efault();
  }

  zipinfo = (struct ZipfsInfo *)info->data;
  if (zipinfo->dirstream == NULL) {
    return ebadf();
  }

  if (closedir(zipinfo->dirstream) == -1) {
    return -1;
  }

  zipinfo->dirstream = NULL;
  unassert(!VfsFreeInfo(info));
  return 0;
}

ssize_t ZipfsReadlink(struct VfsInfo *info, char **output) {
  VFS_LOGF("ZipfsReadlink(%p, %p)", info, output);

  // Zipfs doesn't support symlinks
  return einval();
}

// VfsSystem definition for zipfs
struct VfsSystem g_zipfs = {
    .name = "zipfs",
    .nodev = true,
    .ops =
        {
            .Init = ZipfsInit,
            .Freeinfo = ZipfsFreeInfo,
            .Freedevice = ZipfsFreeDevice,
            .Finddir = ZipfsFinddir,
            .Open = ZipfsOpen,
            .Access = ZipfsAccess,
            .Stat = ZipfsStat,
            .Fstat = ZipfsFstat,
            .Close = ZipfsClose,
            .Read = ZipfsRead,
            .Readv = ZipfsReadv,
            .Pread = ZipfsPread,
            .Seek = ZipfsSeek,
            .Opendir = ZipfsOpendir,
            .Readdir = ZipfsReaddir,
            .Rewinddir = ZipfsRewinddir,
#ifdef HAVE_SEEKDIR
            .Seekdir = ZipfsSeekdir,
            .Telldir = ZipfsTelldir,
#endif
            .Closedir = ZipfsClosedir,
            .Readlink = ZipfsReadlink,
            // All write operations are NULL (will fail with appropriate error)
        },
};

#endif  // DISABLE_VFS
