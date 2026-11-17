# Turns a compiled SPIR-V binary into the C++ initializer list that CompileGLSL.cmake
# includes into a generated source file. Replaces the Linux-only
# `hexdump -ve "/4 \"0x%02X, \""` invocation so the build works on any host.
#
# The generated array is consumed as `std::vector<uint32_t>` (see
# server/utils/wivrn_vk_bundle.cpp) and handed to Vulkan as host byte order, so each
# four byte group is emitted as one byte-swapped 32-bit word.
#
# Usage: cmake -DSPV_IN=<file.spv> -DSPV_OUT=<file.spv.txt> -P SpvToCxx.cmake

if(NOT DEFINED SPV_IN OR NOT DEFINED SPV_OUT)
    message(FATAL_ERROR "SpvToCxx.cmake requires SPV_IN and SPV_OUT")
endif()

if(NOT EXISTS "${SPV_IN}")
    message(FATAL_ERROR "SpvToCxx.cmake: input '${SPV_IN}' does not exist")
endif()

file(READ "${SPV_IN}" _hex HEX)
string(LENGTH "${_hex}" _hex_length)

if(_hex_length EQUAL 0)
    message(FATAL_ERROR "SpvToCxx.cmake: '${SPV_IN}' is empty")
endif()

math(EXPR _remainder "${_hex_length} % 8")
if(NOT _remainder EQUAL 0)
    message(FATAL_ERROR "SpvToCxx.cmake: '${SPV_IN}' is not a whole number of 32-bit words")
endif()

# Uppercase the digits first so the 0x prefix stays lowercase, then fold each four
# bytes into one word in reverse order (SPIR-V is little endian on disk).
string(TOUPPER "${_hex}" _hex)
string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1, \n" _out "${_hex}")

file(WRITE "${SPV_OUT}" "${_out}")
