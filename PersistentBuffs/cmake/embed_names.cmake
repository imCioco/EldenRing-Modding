# Converts a Paramdex names .txt into a C byte array so the DLL can print
# human names in the log without shipping loose data files.
#   cmake -DINPUT=<names.txt> -DOUTPUT=<out.inc> -DVAR=<symbol> -P embed_names.cmake
# Emits: static const unsigned char <VAR>[]; static const size_t <VAR>Len;
# (A byte array, not a string literal -- MSVC caps literals at 64 KB.)

if(NOT INPUT OR NOT OUTPUT OR NOT VAR)
    message(FATAL_ERROR "embed_names.cmake needs -DINPUT=, -DOUTPUT= and -DVAR=")
endif()

file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hexlen)
math(EXPR bytelen "${hexlen} / 2")

# "48656c6c6f" -> "0x48,0x65,..." (wrap lines every 20 bytes for sanity)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){20})" "\\1\n" bytes "${bytes}")

file(WRITE "${OUTPUT}" "// Auto-generated from ${INPUT} -- do not edit.
static const unsigned char ${VAR}[] = {
${bytes}
};
static const size_t ${VAR}Len = ${bytelen};
")
message(STATUS "embedded ${bytelen} bytes of names -> ${OUTPUT}")
