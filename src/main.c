#include <stdio.h>
#include <stdlib.h>
#include "lexer/layout.h"
#include "lexer/lexer.h"
#include "lexer/token.h"
#include "common/vec.h"
#include "common/stringpool.h"


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
    // ----------------------------------------------
    // Pass 0: Read the source file(s) into a string.
    // -----------------------------------------------
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
        return EXIT_FAILURE;
    }

    char* source = read_file_to_string(argv[1]);
    if (source == NULL) {
        return EXIT_FAILURE;
    }

    // ---------------------
    // Pass 1: Tokenization
    // ---------------------

    struct Lexer lexer = lexer_new(source, 0);

    // Initialize the dynamic token vector
    Vec tokens;
    vec_init(&tokens, sizeof(struct Token));

    // Initialize the string pool
    StringPool pool;
    pool_init(&pool, 1024); // Start with 1KB, will grow as needed

    printf("Tokenizing source from %s...\n", argv[1]);

    struct Token token;
    for (;;) {
        token = tokenizer(&lexer, &pool);
        vec_push(&tokens, &token);
        if (token.kind == Eof) {
            break;
        }
    }

    // --------------------------------------
    // Pass 2: Layout normalization 
    // --------------------------------------

    Vec* laid_out = normalizer(&tokens, &pool);

    FILE* out = fopen("layout_debug.ore", "w");
    if (out) {
        for (size_t i = 0; i < laid_out->count; ++i) {
            struct Token* t = (struct Token*)vec_get(laid_out, i);
            if (t == NULL) continue;
            
            if (t->kind == Eof) break;
            
            if (t->origin == Layout) {
                if (t->kind == LBrace) fprintf(out, "{\n");
                else if (t->kind == RBrace) fprintf(out, "}\n");
                else if (t->kind == Semicolon) fprintf(out, ";\n");
                continue;
            }
            
            // Source semicolons/braces too
            if (t->kind == Semicolon) { fprintf(out, ";\n"); continue; }
            if (t->kind == LBrace) { fprintf(out, "{\n"); continue; }
            if (t->kind == RBrace) { fprintf(out, "}\n"); continue; }
            
            // Source tokens — print the lexeme
            const char* lex = pool_get(&pool, t->string_id, t->string_len);
            if (t->kind == StringLit) fprintf(out, "\"%s\" ", lex);
            else if (t->kind == ByteLit) fprintf(out, "'%s' ", lex);
            else fprintf(out, "%s ", lex);
        }
        fprintf(out, "\n");
        fclose(out);
        printf("Layout output written to layout_debug.ore\n");
    }

    // --------------------------------------------
    // For now, we just print the tokens we found.
    // --------------------------------------------

    // Print the tokens for verification
    printf("Found %zu tokens:\n", tokens.count);
    for (size_t i = 0; i < tokens.count; ++i) {
        struct Token* current_token_ptr = (struct Token*)vec_get(&tokens, i);
        if (current_token_ptr != NULL )
            printf("  - Kind: %-20s Lexeme: \"%s\"\n", 
                token_kind_to_str(current_token_ptr->kind), 
                pool_get(&pool, current_token_ptr->string_id, current_token_ptr->string_len));
    }

    // Clean up
    pool_free(&pool);
    vec_free(laid_out);
    free(laid_out);
    vec_free(&tokens);
    free(source);

    printf("\nTokenization complete and memory freed.\n");

    return EXIT_SUCCESS;
}
