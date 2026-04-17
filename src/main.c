#include <stdio.h>
#include <stdlib.h>
#include "lexer/lexer.h"
#include "lexer/token.h"
#include "common/vec.h"


char* read_file_to_string(const char* filepath) {
    FILE* file = fopen(filepath, "r");
    if (file == NULL) {
        perror(filepath);
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    rewind(file);

    char* source_buffer = malloc(file_size + 1);
    if (source_buffer == NULL) {
        fprintf(stderr, "Could not allocate memory for file: %s\n", filepath);
        fclose(file);
        return NULL;
    }

    size_t bytes_read = fread(source_buffer, 1, file_size, file);
    if (bytes_read != file_size) {
        fprintf(stderr, "Error reading file: %s\n", filepath);
        free(source_buffer);
        fclose(file);
        return NULL;
    }

    source_buffer[file_size] = '\0';
    fclose(file);
    return source_buffer;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
        return EXIT_FAILURE;
    }

    char* source = read_file_to_string(argv[1]);
    if (source == NULL) {
        return EXIT_FAILURE;
    }

    struct Lexer lexer = lexer_new(source, 0);

    // Initialize the dynamic token vector
    Vec tokens;
    vec_init(&tokens, sizeof(struct Token));

    printf("Tokenizing source from %s...\n", argv[1]);

    // The main lexing loop
    struct Token token;
    for (;;) {
        token = lexer_next_token(&lexer);
        vec_push(&tokens, &token);
        if (token.kind == Eof) {
            break;
        }
    }

    // Print the tokens for verification
    printf("Found %zu tokens:\n", tokens.count);
    for (size_t i = 0; i < tokens.count; ++i) {
        struct Token* current_token_ptr = (struct Token*)vec_get(&tokens, i);
        if (current_token_ptr != NULL )
            printf("  - Kind: %-20s Lexeme: \"%s\"\n", 
                token_kind_to_str(current_token_ptr->kind), 
                current_token_ptr->lexeme);
    }

    // Clean up
    vec_free(&tokens);
    free(source);

    printf("\nTokenization complete and memory freed.\n");

    return EXIT_SUCCESS;
}
