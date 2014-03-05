/* ZipFile.h: create a zip archive from a list of files
 *
 * (c) Jost Berthold, and CRC32 code from public domain
 *    (jb.diku@gmail.com)
 *
 * The module may or may not use compression, depending on underlying
 * libraries (zlib, to be precise). CRC values are generated here, not
 * relying on external code (using public domain code from Apple, see
 * implementation in ZipFile.c).
 *
 * Part of the Parallel Haskell Runtime System based on GHC.
 * GHC's BSD license applies (see file LICENSE).
 */

#ifndef ZIPFILE_H
#define ZIPFILE_H

#ifdef STANDALONE
#include <stdint.h>
typedef uint8_t rtsBool;

typedef unsigned char StgWord8;
typedef uint16_t StgWord16;
typedef uint32_t StgWord32;
typedef uint64_t StgWord64;

#define rtsTrue 1
#define rtsFalse 0
#else
#include "Rts.h"

#undef HAVE_ZLIB /* deactivate zlib support for now */

#endif

/* Public Interface: create zip archive with given name (archive)
   containing the files in the list of names (names, variable count
   gives its length), and the given comment (comment, or NULL if
   none).
   Returns: Indication of success or failure.
   Upon failures, the function behaves as follows:
     - failing to create or access archive: abort operation, return false
     - failing to access one of the files: skip file (resp. rest of file)
                                           but continue with next file,
                                           return true
*/
rtsBool compressFiles(char const* archive, 
                      int count, char* names[],
                      char const* comment);

#endif /* ZIPFILE_H */
