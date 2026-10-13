# SEG-057-T002: embeds a binary file as a C byte array. cmake -DIN=<file> -DOUT=<file.inc> -DSYMBOL=<name> -P embed_blob.cmake
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" hex_length)
math(EXPR byte_count "${hex_length} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," body "${hex}")
file(WRITE "${OUT}" "// GENERATED from ${SYMBOL} by embed_blob.cmake. DO NOT EDIT.\ninline constexpr unsigned char ${SYMBOL}[${byte_count}] = {${body}};\n")
