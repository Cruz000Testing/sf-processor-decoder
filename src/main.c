#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <emscripten.h>
#include <emscripten/fetch.h>
#include "brotli/decode.h"

// Constantes de seguridad
#define MAX_DECOMPRESSION_RATIO 10
#define INITIAL_BUFFER_SIZE (1024 * 1024) // 1MB inicial
#define MAX_BUFFER_SIZE (200 * 1024 * 1024) // 200MB máximo

// Estructura para manejar el estado de forma segura
typedef struct {
    uint8_t* data;
    size_t size;
    double download_time;
    double decompress_time;
    int error_code;
    char error_message[256];
} SafeState;

SafeState g_state = {0};

// Función para limpiar el estado de forma segura
void safe_cleanup() {
    if (g_state.data) {
        free(g_state.data);
        g_state.data = NULL;
    }
    g_state.size = 0;
    g_state.error_code = 0;
    memset(g_state.error_message, 0, sizeof(g_state.error_message));
};

// Función para establecer un mensaje de error
void set_error(const char* message, int code) {
    g_state.error_code = code;
    strncpy(g_state.error_message, message, sizeof(g_state.error_message) - 1);
    g_state.error_message[sizeof(g_state.error_message) - 1] = '\0';
    printf("ERROR [%d]: %s\n", code, message);
};

// Función para descomprimir con Brotli de forma segura
int safe_decompress_brotli(const uint8_t* compressed_data, size_t compressed_size) {
    if (!compressed_data || compressed_size == 0) {
        set_error("Datos comprimidos inválidos", 1);
        return 0;
    };

    // Permitir archivos comprimidos de hasta 50MB (que podrían descomprimirse a ~500MB)
    if (compressed_size > 50 * 1024 * 1024) {
        set_error("Archivo comprimido demasiado grande para descomprimir de forma segura", 2);
        return 0;
    };

    size_t buffer_size = compressed_size * MAX_DECOMPRESSION_RATIO;
    if (buffer_size > MAX_BUFFER_SIZE) {
        buffer_size = MAX_BUFFER_SIZE;
    };

    uint8_t* buffer = malloc(buffer_size);
    if (!buffer) {
        set_error("Fallo de asignación de memoria", 3);
        return 0;
    };

    BrotliDecoderState* state = BrotliDecoderCreateInstance(NULL, NULL, NULL);
    if (!state) {
        free(buffer);
        set_error("No se pudo crear el estado del decodificador Brotli", 4);
        return 0;
    };

    size_t available_in = compressed_size;
    const uint8_t* next_in = compressed_data;
    size_t available_out = buffer_size;
    uint8_t* next_out = buffer;

    BrotliDecoderResult result;
    int decompression_success = 0;

    do {
        result = BrotliDecoderDecompressStream(
            state, &available_in, &next_in, &available_out, &next_out, NULL);
        
        if (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
            size_t used = buffer_size - available_out;
            size_t new_buffer_size = buffer_size * 2;
            
            // Verificar límite máximo
            if (new_buffer_size > MAX_BUFFER_SIZE) {
                set_error("Límite máximo de descompresión excedido", 5);
                break;
            }
            
            uint8_t* new_buffer = realloc(buffer, new_buffer_size);
            if (!new_buffer) {
                set_error("Fallo al redimensionar el búfer de descompresión", 6);
                break;
            }
            
            buffer = new_buffer;
            next_out = buffer + used;
            available_out = new_buffer_size - used;
            buffer_size = new_buffer_size;
        } else if (result == BROTLI_DECODER_RESULT_ERROR) {
            set_error("Error durante la descompresión Brotli", 7);
            break;
        };
    } while (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT);

    if (result == BROTLI_DECODER_RESULT_SUCCESS) {
        size_t decompressed_size = buffer_size - available_out;
        
        // Verificar que los datos descomprimidos no estén vacíos
        if (decompressed_size == 0) {
            set_error("Datos descomprimidos vacíos", 8);
        } else {
            // Asignar memoria para los datos descomprimidos
            safe_cleanup();
            g_state.data = malloc(decompressed_size);
            
            if (!g_state.data) {
                set_error("Fallo al asignar memoria para datos descomprimidos", 9);
            } else {
                memcpy(g_state.data, buffer, decompressed_size);
                g_state.size = decompressed_size;
                decompression_success = 1;
            };
        };
    };

    BrotliDecoderDestroyInstance(state);
    free(buffer);
    return decompression_success;
};

// Callback para descarga exitosa
void download_success(emscripten_fetch_t* fetch) {
    g_state.download_time = emscripten_get_now() - g_state.download_time;

    if (!fetch->data || fetch->numBytes == 0) {
        set_error("Datos descargados vacíos o inválidos", 10);
        emscripten_fetch_close(fetch);
        return;
    };

    double start_decompress = emscripten_get_now();
    int success = safe_decompress_brotli((const uint8_t*)fetch->data, fetch->numBytes);
    g_state.decompress_time = emscripten_get_now() - start_decompress;

    if (success) {
        printf("Download time: %.2f ms\n", g_state.download_time);
        printf("Decompress time: %.2f ms\n", g_state.decompress_time);
        printf("Decompressed size: %zu bytes\n", g_state.size);
        
        // Notificar al JavaScript que la descompresión está completa
        EM_ASM(
            if (typeof onDecompressionComplete === "function") {
                onDecompressionComplete($0, $1, $2, $3);
            }
        , g_state.download_time, g_state.decompress_time, g_state.size, 0);
    } else {
        // Notificar al JavaScript que hubo un error
        EM_ASM(
            if (typeof onDecompressionComplete === "function") {
                onDecompressionComplete($0, $1, $2, $3);
            }
        , g_state.download_time, g_state.decompress_time, 0, g_state.error_code);
    };

    emscripten_fetch_close(fetch);
};

// Callback para descarga fallida
void download_failed(emscripten_fetch_t* fetch) {
    set_error("Error en la descarga", 11);
    printf("Download failed: %d\n", fetch->status);
    
    // Notificar al JavaScript que hubo un error
    EM_ASM(
        if (typeof onDecompressionComplete === "function") {
            onDecompressionComplete($0, $1, $2, $3);
        }
    , 0, 0, 0, fetch->status);
    
    emscripten_fetch_close(fetch);
};

// Función para iniciar la descarga (expuesta a JavaScript)
EMSCRIPTEN_KEEPALIVE
void initiate_download(const char* url) {
    if (!url || strlen(url) == 0) {
        set_error("URL inválida", 12);
        return;
    };
    
    safe_cleanup();
    g_state.download_time = emscripten_get_now();
    
    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    strcpy(attr.requestMethod, "GET");
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
    attr.onsuccess = download_success;
    attr.onerror = download_failed;
    
    emscripten_fetch(&attr, url);
};

// Función para obtener datos descomprimidos (expuesta a JavaScript)
EMSCRIPTEN_KEEPALIVE
uint8_t* get_decompressed_data() {
    return g_state.data;
};

// Función para obtener el tamaño de datos descomprimidos (expuesta a JavaScript)
EMSCRIPTEN_KEEPALIVE
size_t get_decompressed_size() {
    return g_state.size;
};

// Función para obtener el mensaje de error (expuesta a JavaScript)
EMSCRIPTEN_KEEPALIVE
const char* get_error_message() {
    return g_state.error_message;
};

// Función para obtener el código de error (expuesta a JavaScript)
EMSCRIPTEN_KEEPALIVE
int get_error_code() {
    return g_state.error_code;
};

// Función para liberar memoria (expuesta a JavaScript)
EMSCRIPTEN_KEEPALIVE
void free_resources() {
    safe_cleanup();
};

// Inicialización del módulo
EMSCRIPTEN_KEEPALIVE
void init_module() {
    safe_cleanup();
    printf("Módulo de descompresión Brotli inicializado\n");
};