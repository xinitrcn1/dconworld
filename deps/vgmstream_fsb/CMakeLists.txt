message(STATUS "Fetching vgmstream FSB Vorbis setup headers...")
# FMOD's FSB5 Vorbis drops the Vorbis setup header and only keeps its CRC32;
# vgmstream (ISC style license, see its COPYING) ships the known ones as C
# arrays. Only that one header is downloaded, pinned and hash checked.
set(VGMSTREAM_COMMIT d9875b1fff307e4759dfb85e49e3d5e6fb196f7c)
set(VGMSTREAM_FSB_DIR "${CMAKE_BINARY_DIR}/_deps/vgmstream_fsb")
file(DOWNLOAD
    "https://raw.githubusercontent.com/vgmstream/vgmstream/${VGMSTREAM_COMMIT}/src/coding/libs/vorbis_codebooks_fsb.h"
    "${VGMSTREAM_FSB_DIR}/src/coding/libs/vorbis_codebooks_fsb.h"
    EXPECTED_HASH SHA256=f1e7a8c46de6babd0ed577bde0a13e8b7dab68dd9fd3082188b78dbc8057e752
    SHOW_PROGRESS
)
file(DOWNLOAD
    "https://raw.githubusercontent.com/vgmstream/vgmstream/${VGMSTREAM_COMMIT}/COPYING"
    "${VGMSTREAM_FSB_DIR}/COPYING"
)
# the header includes "../../util/vorbis_codebooks.h" for its entry type only
file(WRITE "${VGMSTREAM_FSB_DIR}/src/util/vorbis_codebooks.h" [=[
#pragma once
#include <stdint.h>
typedef struct {
    uint32_t id;
    uint32_t size;
    const uint8_t* codebooks;
} vcb_info_t;
]=])
add_library(dependency_vgmstream_fsb INTERFACE)
target_include_directories(dependency_vgmstream_fsb SYSTEM INTERFACE "${VGMSTREAM_FSB_DIR}/src/coding/libs")
