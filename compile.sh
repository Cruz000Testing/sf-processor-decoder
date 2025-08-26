#!/bin/bash

# Compilar con Emscripten - versión optimizada
emcc -O3 \
    -I ./brotli/include \
    src/main.c \
    brotli/common/*.c \
    brotli/dec/*.c \
    -s WASM=1 \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s MAXIMUM_MEMORY=4GB \
    -s INITIAL_MEMORY=64MB \
    -s STACK_SIZE=5MB \
    -s ASSERTIONS=1 \
    -s SAFE_HEAP=1 \
    -s STACK_OVERFLOW_CHECK=2 \
    -s EXPORTED_FUNCTIONS="['_initiate_download','_get_decompressed_data','_get_decompressed_size','_get_error_message','_get_error_code','_free_resources','_init_module']" \
    -s EXPORTED_RUNTIME_METHODS="['ccall','cwrap','UTF8ToString', 'HEAPU8']" \
    -s FILESYSTEM=0 \
    -s MODULARIZE=1 \
    -s EXPORT_ES6=1 \
    -lfetch \
    -o soundfont.js