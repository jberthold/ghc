/* ZipFile.c: create a zip archive from a list of files
 *
 * (c) Jost Berthold, and CRC32 code from public domain
 *    (jb.diku@gmail.com)
 *
 * The module may or may not use compression, depending on underlying
 * libraries (zlib, to be precise). CRC values are generated here, not
 * relying on external code (using public domain code from Apple, see
 * below).
 *
 * Part of the Parallel Haskell Runtime System based on GHC.
 * GHC's BSD license applies (see file LICENSE).
 */

#include "ZipFile.h"

#include <stdlib.h>
#include <string.h>

#ifdef STANDALONE
#include <stdio.h>
#include <errno.h>
#define sysErrorBelch(x) perror(x "\n")
#define errorBelch(x, ...) fprintf(stderr, x "\n", __VA_ARGS__)
#define stgMallocBytes(sz,msg) malloc(sz)
#define stgFree(p) free(p)
#define FLEXIBLE_ARRAY 0
#define ASSERT(x) if (!(x)) fprintf(stderr,"Assertion failed: %s",#x)
#else
#include "RtsUtils.h"
#endif

#ifdef HAVE_ZLIB
#error "Do not use ZLib, this code produces invalid zip files"
#include "zlib.h"
#endif

// Data types and definitions for zip files
// magic zip numbers
#define ZIP_FILE_HEADER 0x04034b50
#define ZIP_DATA_DESCR 0x08074b50
#define ZIP_CENTRAL_FILE_HEADER 0x02014b50
#define ZIP_CENTRAL_DIR_END 0x06054b50

typedef struct {
  StgWord32 sig;     // magic number
  StgWord16 extract; // min version to extract
  StgWord16 gflags;  // general flags
  StgWord16 compres; // method
  StgWord16 modtime; // MSDOS format: 5/6/5 bit
  StgWord16 moddate; // MSDOS format: 7/4/5 bit
  StgWord32 crc32; // might follow after file in data descriptor
  StgWord32 csize; // ditto
  StgWord32 usize; // ditto
  StgWord16 nlength; // name length
  StgWord16 elength; // extra field length (zero for us, for now)
  char   name[FLEXIBLE_ARRAY];
  // char   extra[FLEXIBLE_ARRAY]; // empty for us (for now)
} FileHeader;

typedef struct {
  StgWord32 crc32;
  StgWord32 csize;
  StgWord32 usize;
} DataDescr;

typedef struct {
  StgWord32 sig;
  StgWord16 madeBy;   // System that generated it
  StgWord16 extract;  // min version to extract
  StgWord16 gflags;   // general flags
  StgWord16 compres;  // method
  StgWord16 modtime;  // MSDOS 5/6/5 bit
  StgWord16 moddate;  // MSDOS 7/4/5 bit (years from 1980)
  StgWord32 crc32;    // here and in data descriptor
  StgWord32 csize;    // here and in data descriptor
  StgWord32 usize;    // here and in data descriptor
  StgWord16 nlength;
  StgWord16 elength;
  StgWord16 clength;
  StgWord16 dnum;     // disk number (FIXME starts at 0?)
  StgWord16 iattr;    // internal attributes (bit 0 set => ASCII data )
  StgWord32 eattr;    // external attributes (system dependent)
  StgWord32 offset;
  char   name[FLEXIBLE_ARRAY];
  // char   extra[FLEXIBLE_ARRAY]; // empty for us (for now)
  // char   comm[FLEXIBLE_ARRAY]; // empty for us (for now)
} CentralDirEntry;

typedef struct {
  StgWord32 sig;
  StgWord16 dnum;    // disk num (starts at 0?)
  StgWord16 cdd;     // disk where central dir starts
  StgWord16 nFiles;  // total number of files
  StgWord16 nHere;   // number of files in this part of central dir
  StgWord32 dsize;   // central dir size in bytes (excludes end marker)
  StgWord32 cdOff;   // offset of central dir from its (first) disk
  StgWord16 clength; // comment length
  char   comm[FLEXIBLE_ARRAY];
} CentralDirEnd;


// functions to write the data, avoiding alignment issues
int writeFH(FILE *fd, FileHeader *f);
int writeDD(FILE *fd, DataDescr dd);
int writeCD(FILE *fd, CentralDirEntry *d);
int writeCDE(FILE *fd, CentralDirEnd *d);

// this macro is used throughout, relying on presence of variables named
// res, total, fd
#warning "FIXME: should abort archiving when write fails"
#define chkWrite(field,bytes) \
  if ((res = fwrite(field, 1, bytes, fd)) == -1){ \
    sysErrorBelch("failed to write " #field); \
  } else { total += res; }

// write FileHeader to file, avoiding alignment padding
int writeFH(FILE *fd, FileHeader *f) {
  int res;
  int total=0;
  chkWrite(&f->sig,4); chkWrite(&f->extract,2);
  chkWrite(&f->gflags,2); chkWrite(&f->compres,2);
  chkWrite(&f->modtime,2); chkWrite(&f->moddate,2);
  chkWrite(&f->crc32,4); chkWrite(&f->csize,4); chkWrite(&f->usize,4);
  chkWrite(&f->nlength,2); chkWrite(&f->elength,2);
  chkWrite(f->name, f->nlength);
  return total;
}

// write DataDescr avoiding alignment padding
//  many libraries require a marker ZIP_DATA_DESCR
int writeDD(FILE *fd, DataDescr dd) {
  uint32_t marker = ZIP_DATA_DESCR; // std. sez "common, but not std."
  int res;
  int total=0;
  chkWrite(&marker,4);  // std. sez "common, but not std."
  chkWrite(&dd.crc32,4); chkWrite(&dd.csize,4); chkWrite(&dd.usize,4);
  return total;
}

// write CentralDirEntry to file, avoiding alignment padding
int writeCD(FILE *fd, CentralDirEntry *d) {
  int res;
  int total=0;
  chkWrite(&d->sig,4); chkWrite(&d->madeBy,2); chkWrite(&d->extract,2);
  chkWrite(&d->gflags,2); chkWrite(&d->compres,2);
  chkWrite(&d->modtime,2); chkWrite(&d->moddate,2);
  chkWrite(&d->crc32,4); chkWrite(&d->csize,4); chkWrite(&d->usize,4);
  chkWrite(&d->nlength,2); chkWrite(&d->elength,2); chkWrite(&d->clength,2);
  chkWrite(&d->dnum,2); chkWrite(&d->iattr,2); chkWrite(&d->eattr,4);
  chkWrite(&d->offset,4);
  chkWrite(d->name, d->nlength);
  return total;
}

// write CentralDirEnd avoiding alignment padding
int writeCDE(FILE *fd, CentralDirEnd *d) {
  int res;
  int total=0;
  chkWrite(&d->sig,4);
  chkWrite(&d->dnum,2); chkWrite(&d->cdd,2);
  chkWrite(&d->nFiles,2); chkWrite(&d->nHere,2);
  chkWrite(&d->dsize,4); chkWrite(&d->cdOff,4);
  chkWrite(&d->clength,2);
  chkWrite(d->comm, d->clength);
  return total;
}

// create entries with reasonable default values (allocating)
FileHeader *mkFileHeader(char* fname);
CentralDirEntry* mkCentralEntry(char* fname);
CentralDirEnd* mkEndMarker(const char *comment);

// create a file header with given name. Date and time arbitrary,
// compression is NONE.
FileHeader *mkFileHeader(char* fname) {
  FileHeader *f;
  StgWord16 l;

  if (fname == NULL)
    return NULL;

  l = (StgWord16) strlen(fname);

  f = stgMallocBytes(sizeof(FileHeader) + l,
                     "Zip file header");
  f->sig     = ZIP_FILE_HEADER;
  f->extract = 0x0014; // hard-code 2.0 for deflate
  f->gflags  = 0x0008; // bit 3 set for CRC in data descr. after file
  f->compres = 0x0000; // "no compression" method. deflate would be 8
  f->modtime = 0x8821; // 17:01:02
  f->moddate = 0x0063; // 1980-03-03
  f->crc32   = 0; // will follow after file
  f->csize   = 0; // will follow after file
  f->usize   = 0; // will follow after file
  f->nlength = l;
  f->elength = 0;
  strncpy(f->name, fname, l);
  return f;
}

// create a central directory entry for given file name. Date and time
// arbitrary, compression is NONE, length fields and crc set to 0.
CentralDirEntry* mkCentralEntry(char* fname) {
  CentralDirEntry *d;
  StgWord16 l;

  if (fname == NULL)
    return NULL;

  l = (StgWord16) strlen(fname);

  d = (CentralDirEntry*) stgMallocBytes(sizeof(CentralDirEntry) + l,
                                        "zip Central dir entry" );
  d->sig     = ZIP_CENTRAL_FILE_HEADER;
  d->madeBy  = 0x0000; // fake to be MSDOS
  d->extract = 0x0014; // hard-code 2.0 for deflate
  d->gflags  = 0x0008; // bit 3 set for CRC in data descr. after file
  d->compres = 0x0000; // "no compression" method. deflate would be 8
  d->modtime = 0x8821; // 17:01:02
  d->moddate = 0x0063; // 1980-03-03
  d->crc32   = 0;      // here and in data descriptor
  d->csize   = 0;      // here and in data descriptor
  d->usize   = 0;      // here and in data descriptor
  d->nlength = l;
  d->elength = 0;
  d->clength = 0;
  d->dnum    = 0; // disk number (FIXME starts at 0?)
  d->iattr   = 0; // hard-code for binary data (bit 0)
  d->eattr   = 0;
  d->offset  = 0;
  strncpy(d->name, fname, l);
  return d;
}

// create end-of-central-dir marker, using given archive comment (if
// not NULL). File count and all offsets/sized are set to 0.
CentralDirEnd* mkEndMarker(const char *comment) {
  CentralDirEnd *d;
  StgWord16 l;

  l = (StgWord16) ((comment==NULL) ? 0 : strlen(comment));

  d = (CentralDirEnd*) stgMallocBytes(sizeof(CentralDirEnd) + l,
                                      "Zip central dir end");
  d->sig     = ZIP_CENTRAL_DIR_END;
  d->dnum    = 0; // disk num (starts at 0?)
  d->cdd     = 0; // disk where central dir starts
  d->nFiles  = 0; // number of files
  d->nHere   = 0; // number of files in this part
  d->cdOff   = 0; // offset of central dir from its (first) disk
  d->clength = l; // comment length

  if (l > 0) {
    strncpy(d->comm, comment, l);
  }
  d->dsize   = sizeof(CentralDirEnd) + l;
  // central dir size in bytes: at least end marker
  return d;
}

/****************** CRC32 ************************
 * This code is derived from code available under this URL:
 * http://www.opensource.apple.com/source/xnu/xnu-1456.1.26/bsd/libkern/crc32.c
 * Copyright status of the original code is:
 *  COPYRIGHT (C) 1986 Gary S. Brown.  You may use this program, or
 *  code or tables extracted from it, as desired without restriction.
 */
void runCRC(StgWord32 *crc, StgWord8 *buf, int count);

static StgWord32 crctable[] = {
        0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
        0xe963a535, 0x9e6495a3,         0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
        0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
        0xf3b97148, 0x84be41de,         0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
        0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec,         0x14015c4f, 0x63066cd9,
        0xfa0f3d63, 0x8d080df5,         0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
        0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,         0x35b5a8fa, 0x42b2986c,
        0xdbbbc9d6, 0xacbcf940,         0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
        0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
        0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
        0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,         0x76dc4190, 0x01db7106,
        0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
        0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
        0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
        0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
        0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
        0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
        0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
        0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
        0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
        0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
        0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
        0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
        0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
        0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
        0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
        0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
        0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
        0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
        0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
        0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
        0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
        0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
        0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
        0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
        0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
        0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
        0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
        0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
        0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
        0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
        0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
        0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};

/* starting with *crc==0xffffffff, call this function on the bytes of
 * the file to be CRC'ed (chunked here). crc will be updated, and
 * contain the final value at the end */
void runCRC(StgWord32 *crc, StgWord8 *buf, int count) {
  while (count-- > 0) {
    *crc = (*crc >> 8) ^ crctable[(*crc & 0xff) ^ *buf++];
  }
}
/************** end CRC32 ********************/

/* interface implementation: create zip archive with given name
 * (archive) containing the files in the list of names (names,
 * variable count gives its length), and the given comment (comment,
 * or NULL if none).
 *
 * Returns: Indication of success or failure.
 * Upon failures, the function behaves as follows:
 *   - failing to create or access archive: abort operation, return false
 *   - failing to access one of the files: skip file (resp. rest of
 *                                         file) but continue with
 *                                         next file, return true
 *   - no files given for archiving: return rtsFalse
 * Rationale: rtsTrue iff a valid zip file was created.
 */

// buffer size for filecopy and compression
#define BUFSIZE 1024

rtsBool compressFiles(char const* archive,
                      int count, char* names[],
                      char const* comment) {
  FileHeader *f;
  DataDescr dd;
  CentralDirEntry **ds;
  CentralDirEnd *de;
  int size;
  FILE *fd, *fdIn;
  StgWord8 buffer[BUFSIZE];
  int i;

#ifdef HAVE_ZLIB
  z_stream strm;
  StgWord8 outbuf[BUFSIZE];
  int ret;
  int flush;
#endif

  if (count == 0) return rtsFalse;

  ds = (CentralDirEntry**) stgMallocBytes(sizeof(CentralDirEntry*) * count,
                                          "Zip central dir array");
  de = mkEndMarker(comment);
  de->nFiles = 0;
  de->nHere  = 0;
  de->dsize  = 0;
  de->cdOff  = 0;

  if ((fd = fopen(archive, "wb")) == NULL) {
    sysErrorBelch("Cannot zip; error opening zip file");
    return rtsFalse;
  }

  for (i=0; i < count; i++) {
    // printf("now file %s (%d)\n", names[i], i);

    size = 0;

    if ((fdIn = fopen(names[i], "rb"))==NULL) {
      sysErrorBelch("Cannot open input file (skipping)");
      errorBelch("File %s skipped\n", names[i]);
      ds[i] = NULL;
      continue;
    }

    ds[i] = mkCentralEntry(names[i]);
    ds[i]->offset = de->cdOff; // remember file offset (current cdOff)
    f = mkFileHeader(names[i]);

    // write FileHeader
#ifdef HAVE_ZLIB
    f->compres=8; // zlib deflate compression
#else
    f->compres=0; // no compression
#endif
    de->cdOff += writeFH(fd, f);
    stgFree(f);

    // compress and write file data here, collecting crc and usize
    dd.usize=0; dd.csize = 0;
    dd.crc32=0xffffffff; // "preconditioning"

#ifdef HAVE_ZLIB
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    ret = deflateInit(&strm, Z_DEFAULT_COMPRESSION);
    if (ret != Z_OK) {
      // failed to initialise, abort operation
      errorBelch("Failed to init zlib, aborting archiving (code %d)", ret);
      fclose(fdIn); fclose(fd); return rtsFalse;
    }

    do {
      // read next chunk, abort if any errors occur
      size = fread(buffer, 1, BUFSIZE, fdIn);
      if (size < 0) {
        sysErrorBelch("Could not read input file (skipping remainder of file)");
        break;
      }
      dd.usize += size;
      runCRC(&dd.crc32, buffer, size);
      strm.avail_in = size;
      flush = feof(fdIn) ? Z_FINISH : Z_NO_FLUSH; // flush at EOF
      strm.next_in = buffer;

      // run deflate() on input until output buffer not completely
      // filled; finish compression if all input read (EOF)
      do {
        strm.avail_out = BUFSIZE;
        strm.next_out = outbuf;
        ret = deflate(&strm, flush);
        ASSERT(ret != Z_STREAM_ERROR); // state not clobbered

        size = BUFSIZE - strm.avail_out; // how much outbuf used?
        // update crc and compressed size
        dd.csize += size;
        // write output
        if (size != (int) fwrite(outbuf, 1, size, fd)) {
          sysErrorBelch("Could not write to output file (aborting archiving)");
          (void) deflateEnd(&strm);
          fclose (fdIn); fclose(fd); return rtsFalse;
        }
      } while (strm.avail_out == 0); // outbuf not utilised => compression ends
      ASSERT(strm.avail_in == 0); // all read data from input file consumed

      // loop continues until EOF in fdIn reached
    } while (flush != Z_FINISH);

    ASSERT(ret == Z_STREAM_END); // stream cleanly ended
    (void) deflateEnd(&strm);

#else
    if ((size = fread(buffer, 1, sizeof(buffer), fdIn)) < 0) {
      sysErrorBelch("Could not read input file (skipping remainder of file)");
    }
    while (size > 0) { // copy the file (no compression)
      if (size != (int)fwrite(buffer, 1, size, fd)) {
        sysErrorBelch("Could not write to output file (aborting archiving)");
        fclose (fdIn); fclose(fd); return rtsFalse;
      }
      dd.usize += size;
      runCRC(&dd.crc32, buffer, size);
      if ((size = fread(buffer, 1, sizeof(buffer), fdIn)) < 0) {
        sysErrorBelch("Could not read input file (skipping remainder of file)");
        break; // anyway not size > 0, but to be clear here...
      }
    }
    dd.csize=dd.usize; // no compression, so same size
#endif
    fclose(fdIn);

    dd.crc32 ^= 0xffffffff; // ones-complement of crc32
    de->cdOff += dd.csize; // add size to offset

    // set sizes and crc
    ds[i]->crc32=dd.crc32;
    ds[i]->csize=dd.csize;
    ds[i]->usize=dd.usize;

    // write data descriptor
    de->cdOff += writeDD(fd, dd);

    // update count
    de->nFiles++; de->nHere++;
  }

  for(i=0; i < count; i++) {
    // now writing out all CentralDirEntries, adding their sizes here
    if (ds[i] != NULL) {
      de->dsize += writeCD( fd, ds[i] );
      stgFree(ds[i]);
    }
  }
  stgFree(ds);
  writeCDE(fd, de);
  stgFree(de);
  fclose(fd);
  return rtsTrue;
}
