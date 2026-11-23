/* The two zlib calls sfArkLib makes, for its sources only: uncompress and
   adler32, renamed so that a system zlib in the same program is untouched.
   JOrganSfArk.cpp implements them with JUCE's own zlib. */
#pragma once

#define Z_OK 0
#define Z_DATA_ERROR (-3)
#define Z_MEM_ERROR (-4)
#define Z_BUF_ERROR (-5)

#define uncompress mp_sfark_uncompress
#define adler32 mp_sfark_adler32

int mp_sfark_uncompress(unsigned char* dest, unsigned long* destLen, const unsigned char* source,
                        unsigned long sourceLen);
unsigned long mp_sfark_adler32(unsigned long adler, const unsigned char* buf, unsigned int len);
