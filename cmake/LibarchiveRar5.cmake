# libarchive's fix for RAR5's last block (libarchive PR #3361), applied to the
# bundled 3.8.10 (#1204). A last compressed block that yields no output makes
# do_unpack() return ARCHIVE_EOF, which 3.8.10 treats as an error, so every
# file read after it was reported damaged. The fix lets EOF through to the
# end-of-file handling, checksums included. Run again, it changes nothing.
set(source "${SOURCE_DIR}/libarchive/archive_read_support_format_rar5.c")
file(READ "${source}" contents)
set(call "ret = do_unpack\\(a, rar5, buff, size, offset\\);")
if(contents MATCHES "${call}[ \t\r\n]+if\\(ret < ARCHIVE_OK\\) {")
  return()
endif()
if(NOT contents MATCHES "${call}[ \t\r\n]+if\\(ret != ARCHIVE_OK\\) {")
  message(FATAL_ERROR "The RAR5 last-block fix does not fit ${source}")
endif()
string(REGEX REPLACE "(${call}[ \t\r\n]+)if\\(ret != ARCHIVE_OK\\) {" "\\1if(ret < ARCHIVE_OK) {"
       contents "${contents}")
file(WRITE "${source}" "${contents}")
