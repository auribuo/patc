#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CCLI_IMPLEMENTATION
#include "ccli.h"

#define NOB_IMPLEMENTATION
#include "nob.h"

#define parser_report_error(p, msg, ...)                                                                     \
    do {                                                                                                     \
        fprintf(stderr, "Error: %s:%zu: " msg "\n", (p)->filename, (p)->cursor - (p)->input, ##__VA_ARGS__); \
        exit(1);                                                                                             \
    } while (0)

#define report_error(msg, ...)                              \
    do {                                                    \
        fprintf(stderr, "Error: " msg "\n", ##__VA_ARGS__); \
        exit(1);                                            \
    } while (0)

#define cursor_offset(p) ((size_t)((p)->cursor - (p)->input))
#define min(a, b) ((a) < (b) ? (a) : (b))

static char patch_file[CCLI_MAX_STR_LEN];
static bool nowrite;

ccli_commands(commands,
              {"apply", "Apply a .patc files"},
              {"restore", "Restore backed up files if they exist"},
              {"check", "Only check the syntax of a patchfile"});

ccli_options(options,
             ccli_option_string_var_p(patch_file, "The patch to apply", "patchfile", true, true, ccli_scope_global()),
             ccli_option_bool_var(nowrite, "Only print subtitutions", false, false, ccli_scope_subcmd(0)));

typedef struct {
    Nob_String_View *items;
    size_t count;
    size_t capacity;
} files;

typedef struct {
    Nob_String_View filename;

    Nob_String_View to_match;
    Nob_String_View to_replace;
} patc;

typedef struct {
    patc *items;
    size_t count;
    size_t capacity;
} patches;

typedef struct {
    const char *filename;
    const char *input;
    size_t len;

    const char *cursor;
} parser;

void parser_expect_advance(parser *p, char c) {
    if (*p->cursor != c) {
        if (isalnum(c)) {
            parser_report_error(p, "expected %c, got %c", c, *p->cursor);
        } else {
            parser_report_error(p, "expected 0x%x, got 0x%x", c, *p->cursor);
        }
    }
    p->cursor++;
}

void parser_expect_eof_or_advance(parser *p, char c) {
    if (cursor_offset(p) == p->len) {
        return;
    }
    parser_expect_advance(p, c);
}

void parser_skip_white(parser *p) {
    while (cursor_offset(p) < p->len && isspace(*p->cursor)) {
        p->cursor++;
    }
}

Nob_String_View parser_parse_until(parser *p, char c) {
    const char *span_start = p->cursor;
    while (cursor_offset(p) < p->len) {
        if (*p->cursor == c) {
            return (Nob_String_View){
                .data = span_start,
                .count = (p->cursor++) - span_start};
        }
        p->cursor++;
    }
    parser_report_error(p, "expected sequence terminated by %c, got EOF", c);
}

void parser_expect_eof(parser *p) {
    if (cursor_offset(p) != p->len) {
        parser_report_error(p, "expected eof, got remaining input: %.*s", (int)(p->len - cursor_offset(p)), p->cursor);
    }
}

char parser_advance(parser *p) {
    if (cursor_offset(p) >= p->len) {
        parser_report_error(p, "unexpected EOF");
    }
    return (*p->cursor++);
}

Nob_String_View parse_block(parser *p, char delim) {
    Nob_String_View block = {.data = p->cursor, .count = 0};
    const char delimstr[3] = {
        delim,
        delim,
        '\0',
    };
    bool is_line_start = true;
    const char *last_newline = NULL;
    while (true) {
        if (cursor_offset(p) >= p->len) {
            parser_report_error(p, "expected match block terminated by \\n%s\\n, reached EOF instead", delimstr);
        }
        if (*p->cursor == '\n') {
            last_newline = p->cursor - 1;
            parser_skip_white(p);
            is_line_start = true;
            continue;
        }
        if (is_line_start) {
            if (memcmp(p->cursor, delimstr, 2) == 0) {
                if (last_newline == NULL) {
                    block.count = p->cursor - block.data;
                } else {
                    block.count = last_newline - block.data;
                }
                p->cursor += 3;
                break;
            } else {
                is_line_start = false;
                p->cursor++;
            }
        } else {
            p->cursor++;
        }
    }

    return block;
}

void parse_file_block(parser *p, patches *ps) {
    patc patch = {0};

    parser_expect_advance(p, '@');
    patch.filename = nob_sv_trim(parser_parse_until(p, '\n'));

    parser_expect_advance(p, '?');
    parser_expect_advance(p, '?');
    parser_expect_advance(p, '\n');

    patch.to_match = parse_block(p, '?');

    parser_skip_white(p);

    parser_expect_advance(p, '!');
    parser_expect_advance(p, '!');
    parser_expect_advance(p, '\n');

    patch.to_replace = parse_block(p, '!');

    parser_expect_eof_or_advance(p, '\n');
    parser_skip_white(p);
    nob_da_append(ps, patch);
}

void parse_file(parser *p, patches *ps) {
    while (cursor_offset(p) < p->len) {
        parse_file_block(p, ps);
    }
    parser_expect_eof(p);
}

bool match_content(const char *start, Nob_String_View to_match) {
    while (to_match.count > 0) {
        if (*start != *to_match.data) {
            break;
        }
        start++;
        to_match.data++;
        to_match.count--;
    }
    return to_match.count == 0;
}

void apply_patc(patc patch, Nob_String_Builder *in, Nob_String_Builder *out) {
    if (patch.to_match.count == 0) {
        return;
    }
    if (patch.to_match.count > in->count) {
        return;
    }
    bool found_any = false;
    for (size_t i = 0; i < in->count; ++i) {
        if (in->items[i] != *patch.to_match.data || !match_content(in->items + i, patch.to_match)) {
            nob_da_append(out, in->items[i]);
            continue;
        }
        found_any = true;
        nob_sb_append_buf(out, patch.to_replace.data, patch.to_replace.count);
        i += patch.to_match.count;
        nob_da_append(out, in->items[i]);
    }
    if (!found_any) {
        nob_log(NOB_WARNING, "Found no matches for patch ?? %.*s... ??", (int)(min(patch.to_match.count, 20)), patch.to_match.data);
    }
}

void run_patch(patches *ps) {
    Nob_String_Builder front = {0};
    Nob_String_Builder back = {0};
    Nob_String_View current_file = {0};

    for (size_t i = 0; i < ps->count; ++i) {
        patc patch = ps->items[i];
        const char *filename = nob_temp_sprintf(SV_Fmt, SV_Arg(patch.filename));
        if (nob_sv_eq(patch.filename, current_file)) {
            nob_da_reserve(&front, back.count);
            memcpy(front.items, back.items, back.count);
            front.count = back.count;
        } else {
            const char *current_filename = nob_temp_sprintf(SV_Fmt, SV_Arg(current_file));

            if (!nowrite) {
                nob_copy_file(filename, nob_temp_sprintf("%s.bak", filename));
                if (back.count > 0 && !nob_write_entire_file(current_filename, back.items, back.count)) {
                    report_error("failed to write patch file %s", filename);
                }
            } else {
                if (back.count > 0) {
                    printf("File %s after patching:\n%.*s\n", current_filename, (int)back.count, back.items);
                }
            }

            current_file = patch.filename;
            front.count = 0;
            if (!nob_read_entire_file(filename, &front)) {
                report_error("failed to read file to patch %s", filename);
            }
        }
        back.count = 0;
        nob_log(NOB_INFO, "Patching file %s", filename);
        apply_patc(patch, &front, &back);
    }

    const char *filename = nob_temp_sprintf(SV_Fmt, SV_Arg(current_file));
    if (back.count > 0) {
        if (!nowrite && !nob_write_entire_file(filename, back.items, back.count)) {
            return;
        } else if (nowrite) {
            printf("File %s after patching:\n%.*s\n", filename, (int)back.count, back.items);
        }
    }
}

void run_restore(patches *ps) {
    files fs = {0};
    for (size_t i = 0; i < ps->count; ++i) {
        patc p = ps->items[i];
        bool is_in = false;
        for (size_t j = 0; j < fs.count; ++j) {
            if (nob_sv_eq(fs.items[j], p.filename)) {
                is_in = true;
                break;
            }
        }
        if (!is_in) {
            nob_da_append(&fs, p.filename);
        }
    }
    for (size_t i = 0; i < fs.count; ++i) {
        nob_copy_file(nob_temp_sprintf(SV_Fmt ".bak", SV_Arg(fs.items[i])),
                      nob_temp_sprintf(SV_Fmt, SV_Arg(fs.items[i])));
    }
}

int main(int argc, char *argv[]) {
    const char *cmd = ccli_parse_opts(commands, options, argc, argv, NULL);

    Nob_String_Builder frdr = {0};
    if (!nob_read_entire_file(patch_file, &frdr)) {
        return 1;
    }

    parser p = {
        .filename = patch_file,
        .input = frdr.items,
        .cursor = frdr.items,
        .len = frdr.count};
    patches ps = {0};
    parse_file(&p, &ps);

    if (strcmp(cmd, "apply") == 0) {
        run_patch(&ps);
    } else if (strcmp(cmd, "restore") == 0) {
        run_restore(&ps);
    }

    nob_log(NOB_INFO, "Done");

    return 0;
}
