// tui.c - Minimal ANSI TUI for CSV viewer
// Compile: gcc -o tui tui.c
// Usage: tui [-n] [-d <delim>] <file.csv>
//        -n: no header row
//        -d: one char delimiter or \t for tab

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>

// ============================================================================
// CSV Parser
// ============================================================================

#define FLAG_NEEDS_DECODE 0x01
#define INITIAL_CAP 1024

typedef struct Buffer {
    char *data;
    size_t len;
} Buffer;

typedef struct CellRef {
    size_t offset;
    uint32_t len;
    uint16_t col;
    uint8_t flags;
    uint8_t _pad;
} CellRef;

typedef struct ParsedCSV {
    Buffer buf;
    CellRef *cells;
    size_t cell_count;
    size_t cell_cap;
    size_t *row_start;
    size_t row_count;
    size_t row_cap;
    uint16_t max_cols;
} ParsedCSV;

typedef enum {
    PARSE_OK,
    PARSE_OOM,
    PARSE_INVALID,
    PARSE_OVERFLOW
} ParseResult;

typedef enum {
    FIELD_START,
    IN_UNQUOTED,
    IN_QUOTED,
    QUOTE_IN_QUOTED,
    POST_QUOTED_WS
} ParseState;

static Buffer buffer_load(const char *path) {
    Buffer buf = {0};
    int fd = open(path, O_RDONLY);
    if (fd == -1) { perror("open"); return buf; }

    struct stat st;
    if (fstat(fd, &st) == -1) { perror("fstat"); close(fd); return buf; }
    if (st.st_size < 0 || (unsigned long long)st.st_size > (unsigned long long)SIZE_MAX) {
        fprintf(stderr, "file too large\n"); close(fd); return buf;
    }

    buf.len = (size_t)st.st_size;
    if (buf.len == 0) { close(fd); return buf; }

    buf.data = mmap(NULL, buf.len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (buf.data == MAP_FAILED) { perror("mmap"); buf.data = NULL; }
    close(fd);
    return buf;
}

static bool ensure_cell_cap(ParsedCSV *csv, size_t needed) {
    if (needed <= csv->cell_cap) return true;
    size_t new_cap = csv->cell_cap ? csv->cell_cap * 2 : INITIAL_CAP;
    while (new_cap < needed) new_cap *= 2;
    CellRef *tmp = realloc(csv->cells, new_cap * sizeof(CellRef));
    if (!tmp) return false;
    csv->cells = tmp;
    csv->cell_cap = new_cap;
    return true;
}

static bool ensure_row_cap(ParsedCSV *csv, size_t needed) {
    if (needed <= csv->row_cap) return true;
    size_t new_cap = csv->row_cap ? csv->row_cap * 2 : INITIAL_CAP;
    while (new_cap < needed) new_cap *= 2;
    size_t *tmp = realloc(csv->row_start, new_cap * sizeof(size_t));
    if (!tmp) return false;
    csv->row_start = tmp;
    csv->row_cap = new_cap;
    return true;
}

static ParsedCSV parsed_csv_init(Buffer buf) {
    ParsedCSV csv = {0};
    csv.buf = buf;
    csv.cells = malloc(INITIAL_CAP * sizeof(CellRef));
    if (!csv.cells) return csv;
    csv.cell_cap = INITIAL_CAP;
    csv.row_start = malloc(INITIAL_CAP * sizeof(size_t));
    if (!csv.row_start) { free(csv.cells); csv.cells = NULL; return csv; }
    csv.row_cap = INITIAL_CAP;
    return csv;
}

static void parsed_csv_free(ParsedCSV *csv) {
    if (csv->buf.data) munmap(csv->buf.data, csv->buf.len);
    free(csv->cells);
    free(csv->row_start);
    *csv = (ParsedCSV){0};
}

static ParseResult emit_cell(ParsedCSV *csv, size_t cell_start, size_t cell_len, uint16_t *col, uint8_t flags) {
    if (cell_len > UINT32_MAX) return PARSE_OVERFLOW;
    if (*col == UINT16_MAX) return PARSE_OVERFLOW;
    if (!ensure_cell_cap(csv, csv->cell_count + 1)) return PARSE_OOM;
    csv->cells[csv->cell_count++] = (CellRef){cell_start, (uint32_t)cell_len, (*col)++, flags, 0};
    return PARSE_OK;
}

static ParseResult finalize_row(ParsedCSV *csv, uint16_t *col) {
    if (*col > csv->max_cols) csv->max_cols = *col;
    csv->row_count++;
    if (!ensure_row_cap(csv, csv->row_count + 1)) return PARSE_OOM;
    csv->row_start[csv->row_count] = csv->cell_count;
    *col = 0;
    return PARSE_OK;
}

static ParseResult parsed_csv_parse(ParsedCSV *csv, char delimiter) {
    char *data = csv->buf.data;
    size_t len = csv->buf.len;

    if (len == 0) {
        if (!ensure_row_cap(csv, 1)) return PARSE_OOM;
        csv->row_start[0] = 0;
        return PARSE_OK;
    }

    ParseState state = FIELD_START;
    size_t cell_start = 0, quoted_cell_len = 0;
    uint16_t col = 0;
    uint8_t flags = 0;

    if (!ensure_row_cap(csv, 1)) return PARSE_OOM;
    csv->row_start[0] = 0;

    bool skip_next = false;
    for (size_t i = 0; i <= len; i++) {
        if (skip_next) { skip_next = false; continue; }
        char c = (i < len) ? data[i] : '\n';
        if (c == '\r') { c = '\n'; if (i + 1 < len && data[i + 1] == '\n') skip_next = true; }

        switch (state) {
            case FIELD_START:
                if (i >= len) break;
                cell_start = i; flags = 0;
                if (c == '"') { state = IN_QUOTED; cell_start = i + 1; }
                else if (c == delimiter) { ParseResult r = emit_cell(csv, cell_start, 0, &col, flags); if (r != PARSE_OK) return r; }
                else if (c == '\n') { ParseResult r = emit_cell(csv, cell_start, 0, &col, flags); if (r != PARSE_OK) return r; r = finalize_row(csv, &col); if (r != PARSE_OK) return r; }
                else state = IN_UNQUOTED;
                break;
            case IN_UNQUOTED:
                if (c == delimiter) { ParseResult r = emit_cell(csv, cell_start, i - cell_start, &col, flags); if (r != PARSE_OK) return r; state = FIELD_START; }
                else if (c == '\n') { ParseResult r = emit_cell(csv, cell_start, i - cell_start, &col, flags); if (r != PARSE_OK) return r; r = finalize_row(csv, &col); if (r != PARSE_OK) return r; state = FIELD_START; }
                break;
            case IN_QUOTED:
                if (c == '"') { state = QUOTE_IN_QUOTED; quoted_cell_len = i - cell_start; }
                break;
            case QUOTE_IN_QUOTED:
                if (c == '"') { flags |= FLAG_NEEDS_DECODE; state = IN_QUOTED; }
                else if (c == ' ' || c == '\t') state = POST_QUOTED_WS;
                else if (c == delimiter || c == '\n') {
                    ParseResult r = emit_cell(csv, cell_start, quoted_cell_len, &col, flags); if (r != PARSE_OK) return r;
                    if (c == '\n') { r = finalize_row(csv, &col); if (r != PARSE_OK) return r; }
                    state = FIELD_START;
                } else return PARSE_INVALID;
                break;
            case POST_QUOTED_WS:
                if (c == ' ' || c == '\t') {}
                else if (c == delimiter || c == '\n') {
                    ParseResult r = emit_cell(csv, cell_start, quoted_cell_len, &col, flags); if (r != PARSE_OK) return r;
                    if (c == '\n') { r = finalize_row(csv, &col); if (r != PARSE_OK) return r; }
                    state = FIELD_START;
                } else return PARSE_INVALID;
                break;
        }
    }
    if (state == IN_QUOTED || state == QUOTE_IN_QUOTED || state == POST_QUOTED_WS) return PARSE_INVALID;
    return PARSE_OK;
}

static const CellRef *parsed_csv_get_cell(const ParsedCSV *csv, size_t row, uint16_t col) {
    if (row >= csv->row_count) return NULL;
    size_t start = csv->row_start[row];
    size_t end = csv->row_start[row + 1];
    for (size_t i = start; i < end; i++) {
        if (csv->cells[i].col == col) return &csv->cells[i];
    }
    return NULL;
}

// Decode a cell value, handling escaped quotes
// If out is NULL or out_cap is 0, returns required length without writing
// Otherwise writes up to out_cap bytes and returns actual decoded length
// Does NOT null-terminate; caller must add terminator if needed
static size_t cell_decode(const ParsedCSV *csv, const CellRef *cell,
        char *out, size_t out_cap) {
    const char *src = csv->buf.data + cell->offset;
    size_t src_len = cell->len;

    if (!(cell->flags & FLAG_NEEDS_DECODE)) {
        // No escaping: direct copy or length query
        if (out && out_cap > 0) {
            size_t copy_len = src_len < out_cap ? src_len : out_cap;
            memcpy(out, src, copy_len);
        }
        return src_len;
    }

    // Escaped quotes: decode "" -> "
    size_t j = 0;
    for (size_t i = 0; i < src_len; i++) {
        char c = src[i];
        if (c == '"' && i + 1 < src_len && src[i + 1] == '"') {
            if (out && j < out_cap) out[j] = '"';
            j++;
            i++;  // Skip second quote
        } else {
            if (out && j < out_cap) out[j] = c;
            j++;
        }
    }
    return j;
}

// ============================================================================
// CellValue Structure (Extended for Frequency Panes)
// ============================================================================

typedef struct {
    bool is_empty;
    bool is_num;        // True if numeric (double or int)
    bool is_int;        // True if i64 is valid for integer comparison
    double num;         // Numeric value (for CSV cells parsed as numbers)
    int64_t i64;        // Integer value (for freq count/percent, stable comparison)
    char str[256];      // Display/search string
    size_t str_len;
} CellValue;

// ============================================================================
// FreqRow Structure
// ============================================================================

typedef struct FreqRow {
    char   *value;      // Owned heap string (never NULL)
    size_t  value_len;  // Byte length of value
    size_t  count;      // Occurrence count
    int32_t pct_bp;     // Percent as basis points (0-10000), e.g., 1234 = 12.34%
} FreqRow;

// ============================================================================
// Pane Structure
// ============================================================================

typedef struct Pane {
    // === Row identity (mutually exclusive modes) ===
    // CSV pane: row_ids maps base_index -> global row_id
    size_t *row_ids;        // NULL for main pane (base_index == row_id)
    size_t  row_id_count;   // Count when row_ids != NULL

    // Frequency pane: freq_rows is the data source
    FreqRow *freq_rows;     // NULL for CSV panes
    size_t   freq_row_count;
    bool     is_freq_pane;
    uint16_t freq_source_col;     // Source column index (for status display)
    int      freq_col_widths[3];  // [0]=value (expandable), [1]=count (fixed), [2]=percent (fixed)

    // === Parent relationship (for derived panes) ===
    // For frequency panes, parent_csv_pane is the pane index that generated the freq table.
    // For non-derived panes, parent_csv_pane = -1.
    int      parent_csv_pane;

    // === Selection (base_index-keyed for ALL pane types) ===
    uint8_t *selection_bitmap;  // Bit i = base_index i is selected
    size_t   selection_bytes;   // = (pane_row_count + 7) / 8

    // === Cursor (display coordinates) ===
    size_t   cur_row;
    uint16_t cur_col;

    // === Viewport ===
    size_t   view_top;
    uint16_t view_left;

    // === Sort state ===
    size_t  *sort_index;    // Permutation: sort_index[display_row] = base_index
    size_t   sort_len;
    bool     sort_active;
    uint16_t sort_col;
    bool     sort_ascending;

    // === Search state ===
    bool     search_active;
    bool     search_has_query;
    char     search_buf[128];
    size_t   search_len;
    uint16_t search_start_col;

    // === Column search state ===
    bool     col_search_active;
    char     col_search_buf[128];
    size_t   col_search_len;
} Pane;

// ============================================================================
// TUI Structure
// ============================================================================

#define DEFAULT_COL_WIDTH 16
#define MAX_COL_WIDTH 256
#define COL_SEP " | "
#define COL_SEP_LEN 3

typedef struct {
    ParsedCSV *csv;
    bool has_header;

    // Terminal
    int term_rows;
    int term_cols;
    struct termios orig_termios;

    // Column widths (shared across panes)
    int *col_widths;
    uint16_t num_cols;

    // Panes
    Pane  *panes;
    size_t pane_count;
    size_t pane_cap;
    size_t active_pane;

    bool running;
} TUI;

// ============================================================================
// Forward Declarations
// ============================================================================

static size_t tui_data_row_count(TUI *tui);
static size_t tui_csv_row(TUI *tui, size_t row_id);
static CellValue tui_get_cell_value(TUI *tui, size_t csv_row, uint16_t col);

// ============================================================================
// Pane Functions
// ============================================================================

// Initialize a CSV-backed pane
// row_ids: NULL for main pane, or owned array of global row_ids (caller transfers ownership)
// count: number of rows (ignored if row_ids==NULL)
static void pane_init_csv(Pane *p, TUI *tui, size_t *row_ids, size_t count) {
    memset(p, 0, sizeof(Pane));
    p->row_ids = row_ids;
    p->row_id_count = count;
    p->is_freq_pane = false;
    p->freq_rows = NULL;
    p->parent_csv_pane = -1;

    // Determine pane row count for selection bitmap sizing
    size_t pane_rows;
    if (row_ids == NULL) {
        pane_rows = tui_data_row_count(tui);
    } else {
        pane_rows = count;
    }

    // Selection bitmap sized to base_index domain
    p->selection_bytes = (pane_rows + 7) / 8;
    if (p->selection_bytes > 0) {
        p->selection_bitmap = calloc(p->selection_bytes, 1);
    }
}

// Initialize a frequency pane
// freq_rows: owned array of FreqRow (caller transfers ownership), pre-sorted by count desc
// count: number of frequency rows
// source_col: original CSV column index (for status display)
static void pane_init_freq(Pane *p, FreqRow *freq_rows, size_t count, uint16_t source_col, int parent_csv_pane) {
    memset(p, 0, sizeof(Pane));
    p->freq_rows = freq_rows;
    p->freq_row_count = count;
    p->is_freq_pane = true;
    p->freq_source_col = source_col;
    p->row_ids = NULL;
    p->row_id_count = 0;
    p->parent_csv_pane = parent_csv_pane;

    // Column widths: [0]=value (expandable, default 24), [1]=count (fixed 10), [2]=percent (fixed 8)
    p->freq_col_widths[0] = 24;
    p->freq_col_widths[1] = 10;
    p->freq_col_widths[2] = 8;

    // Selection bitmap sized to freq_row_count
    p->selection_bytes = (count + 7) / 8;
    if (p->selection_bytes > 0) {
        p->selection_bitmap = calloc(p->selection_bytes, 1);
    }
}

static void pane_free(Pane *p) {
    free(p->row_ids);
    free(p->selection_bitmap);
    free(p->sort_index);

    // Free frequency data
    if (p->freq_rows) {
        for (size_t i = 0; i < p->freq_row_count; i++) {
            free(p->freq_rows[i].value);
        }
        free(p->freq_rows);
    }

    memset(p, 0, sizeof(Pane));
}

static size_t pane_row_count(TUI *tui, Pane *p) {
    if (p->is_freq_pane) {
        return p->freq_row_count;
    }
    if (p->row_ids == NULL) {
        return tui_data_row_count(tui);
    }
    return p->row_id_count;
}

// Convert display_row to base_index (unsorted pane-local index)
// This is the ONLY index mapping helper; all cell access goes through base_index
static size_t pane_display_to_base_index(Pane *p, size_t display_row) {
    if (p->sort_active && p->sort_index != NULL) {
        return p->sort_index[display_row];
    }
    return display_row;
}

// Returns number of columns for this pane
static uint16_t pane_num_cols(TUI *tui, Pane *p) {
    if (p->is_freq_pane) {
        return 3;
    }
    return tui->num_cols;
}

// Returns column width for given column
static int pane_col_width(TUI *tui, Pane *p, uint16_t col) {
    if (p->is_freq_pane) {
        if (col < 3) {
            return p->freq_col_widths[col];
        }
        return 0;
    }
    if (col < tui->num_cols) {
        return tui->col_widths[col];
    }
    return 0;
}

// Get display text for a cell
// base_index: pane-local unsorted row index
// col: column index
// buf/buf_len: output buffer (capped at MAX_COL_WIDTH for display)
// Returns: bytes written (not null-terminated)
//
// NOTE: Display buffer is capped at MAX_COL_WIDTH (256) bytes.
// FreqRow.value stores the full string; only rendering truncates.
static size_t pane_get_cell_text(TUI *tui, Pane *p, size_t base_index,
        uint16_t col, char *buf, size_t buf_len) {
    if (p->is_freq_pane) {
        if (base_index >= p->freq_row_count || col > 2) {
            return 0;
        }
        FreqRow *fr = &p->freq_rows[base_index];

        switch (col) {
            case 0:  // Value
                {
                    size_t copy_len = fr->value_len < buf_len ? fr->value_len : buf_len;
                    memcpy(buf, fr->value, copy_len);
                    return copy_len;
                }
            case 1:  // Count - canonical format: %zu
                return snprintf(buf, buf_len, "%zu", fr->count);
            case 2:  // Percent - canonical format: %d.%02d%%
                return snprintf(buf, buf_len, "%d.%02d%%",
                        fr->pct_bp / 100, fr->pct_bp % 100);
        }
        return 0;
    }

    // CSV pane: derive row_id internally, decode from CellRef
    size_t row_id = (p->row_ids != NULL) ? p->row_ids[base_index] : base_index;
    size_t csv_row = tui_csv_row(tui, row_id);
    const CellRef *cell = parsed_csv_get_cell(tui->csv, csv_row, col);

    if (!cell || cell->len == 0) {
        return 0;
    }

    return cell_decode(tui->csv, cell, buf, buf_len);
}

// Get cell value for sorting and searching
// For freq panes: count/percent use i64 for integer comparison (no floats)
// For all panes: str contains the display text (search matches display)
static CellValue pane_get_cell_value(TUI *tui, Pane *p, size_t base_index, uint16_t col) {
    CellValue val = {0};

    if (p->is_freq_pane) {
        if (base_index >= p->freq_row_count || col > 2) {
            val.is_empty = true;
            return val;
        }
        FreqRow *fr = &p->freq_rows[base_index];

        switch (col) {
            case 0:  // Value (string, not numeric)
                val.str_len = fr->value_len < sizeof(val.str) - 1
                    ? fr->value_len : sizeof(val.str) - 1;
                memcpy(val.str, fr->value, val.str_len);
                val.str[val.str_len] = '\0';
                val.is_num = false;
                val.is_int = false;
                break;

            case 1:  // Count (integer for comparison, string for search)
                val.is_num = true;
                val.is_int = true;
                val.i64 = (int64_t)fr->count;
                // str matches display format exactly (canonical: %zu)
                val.str_len = snprintf(val.str, sizeof(val.str), "%zu", fr->count);
                break;

            case 2:  // Percent (integer basis points for comparison, string for search)
                val.is_num = true;
                val.is_int = true;
                val.i64 = (int64_t)fr->pct_bp;
                // str matches display format exactly (canonical: %d.%02d%%)
                val.str_len = snprintf(val.str, sizeof(val.str),
                        "%d.%02d%%", fr->pct_bp / 100, fr->pct_bp % 100);
                break;
        }
        return val;
    }

    // CSV pane: derive row_id internally, use existing tui_get_cell_value
    size_t row_id = (p->row_ids != NULL) ? p->row_ids[base_index] : base_index;
    size_t csv_row = tui_csv_row(tui, row_id);
    return tui_get_cell_value(tui, csv_row, col);
}

// All selection operations use base_index (pane-local unsorted index)

static bool pane_is_selected(Pane *p, size_t base_index) {
    if (p->selection_bitmap == NULL) return false;
    size_t byte_idx = base_index / 8;
    uint8_t bit_mask = 1 << (base_index % 8);
    if (byte_idx >= p->selection_bytes) return false;
    return (p->selection_bitmap[byte_idx] & bit_mask) != 0;
}

static void pane_select(Pane *p, size_t base_index) {
    if (p->selection_bitmap == NULL) return;
    size_t byte_idx = base_index / 8;
    uint8_t bit_mask = 1 << (base_index % 8);
    if (byte_idx >= p->selection_bytes) return;
    p->selection_bitmap[byte_idx] |= bit_mask;
}

static void pane_unselect(Pane *p, size_t base_index) {
    if (p->selection_bitmap == NULL) return;
    size_t byte_idx = base_index / 8;
    uint8_t bit_mask = 1 << (base_index % 8);
    if (byte_idx >= p->selection_bytes) return;
    p->selection_bitmap[byte_idx] &= ~bit_mask;
}

static size_t pane_count_selected(TUI *tui, Pane *p) {
    size_t count = 0;
    size_t pane_rows = pane_row_count(tui, p);

    for (size_t bi = 0; bi < pane_rows; bi++) {
        if (pane_is_selected(p, bi)) count++;
    }
    return count;
}

static void pane_clear_sort(Pane *p) {
    free(p->sort_index);
    p->sort_index = NULL;
    p->sort_active = false;
    p->sort_len = 0;
    p->cur_row = 0;
    p->view_top = 0;
}

static void clamp_cursor(Pane *p, TUI *tui) {
    size_t row_count = pane_row_count(tui, p);
    if (row_count == 0) {
        p->cur_row = 0;
    } else if (p->cur_row >= row_count) {
        p->cur_row = row_count - 1;
    }

    uint16_t num_cols = pane_num_cols(tui, p);
    if (num_cols == 0) {
        p->cur_col = 0;
    } else if (p->cur_col >= num_cols) {
        p->cur_col = num_cols - 1;
    }
}

// ============================================================================
// Pane Array Management
// ============================================================================

static Pane *panes_add(TUI *tui) {
    if (tui->pane_count >= tui->pane_cap) {
        size_t new_cap = tui->pane_cap ? tui->pane_cap * 2 : 4;
        Pane *new_panes = realloc(tui->panes, new_cap * sizeof(Pane));
        if (!new_panes) return NULL;
        tui->panes = new_panes;
        tui->pane_cap = new_cap;
    }
    return &tui->panes[tui->pane_count++];
}

static void panes_remove(TUI *tui, size_t idx) {
    if (idx >= tui->pane_count) return;
    pane_free(&tui->panes[idx]);
    for (size_t i = idx; i < tui->pane_count - 1; i++) {
        tui->panes[i] = tui->panes[i + 1];
    }
    tui->pane_count--;
    if (tui->active_pane >= tui->pane_count && tui->pane_count > 0) {
        tui->active_pane = tui->pane_count - 1;
    }
}

// ============================================================================
// TUI Helper Functions
// ============================================================================

static void tui_die(const char *msg) {
    write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);
    perror(msg);
    exit(1);
}

static void tui_disable_raw(TUI *tui) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &tui->orig_termios);
    write(STDOUT_FILENO, "\x1b[?25h", 6);
}

static void tui_enable_raw(TUI *tui) {
    if (tcgetattr(STDIN_FILENO, &tui->orig_termios) == -1) tui_die("tcgetattr");

    struct termios raw = tui->orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) tui_die("tcsetattr");
    write(STDOUT_FILENO, "\x1b[?25l", 6);
}

static void tui_get_size(TUI *tui) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        tui->term_cols = 80;
        tui->term_rows = 24;
    } else {
        tui->term_cols = ws.ws_col;
        tui->term_rows = ws.ws_row;
    }
}

// Returns 1 if this pane will draw a header row, 0 otherwise
static int tui_header_rows(TUI *tui, Pane *pane) {
    return (tui->has_header && !pane->is_freq_pane) ? 1 : 0;
}

// Returns number of visible data rows for this pane
static int tui_visible_data_rows(TUI *tui, Pane *pane) {
    int rows = tui->term_rows - 1;          // status line
    rows -= tui_header_rows(tui, pane);     // only if header is actually drawn
    return rows > 0 ? rows : 1;
}

static size_t tui_data_row_count(TUI *tui) {
    if (tui->csv->row_count == 0) return 0;
    return tui->has_header ? tui->csv->row_count - 1 : tui->csv->row_count;
}

static size_t tui_csv_row(TUI *tui, size_t row_id) {
    return tui->has_header ? row_id + 1 : row_id;
}

static CellValue tui_get_cell_value(TUI *tui, size_t csv_row, uint16_t col) {
    CellValue val = {0};
    const CellRef *cell = parsed_csv_get_cell(tui->csv, csv_row, col);
    if (!cell || cell->len == 0) {
        val.is_empty = true;
        return val;
    }

    const char *src = tui->csv->buf.data + cell->offset;
    size_t src_len = cell->len;

    if (!(cell->flags & FLAG_NEEDS_DECODE)) {
        val.str_len = src_len < sizeof(val.str) - 1 ? src_len : sizeof(val.str) - 1;
        memcpy(val.str, src, val.str_len);
    } else {
        val.str_len = cell_decode(tui->csv, cell, val.str, sizeof(val.str) - 1);
        if (val.str_len >= sizeof(val.str)) val.str_len = sizeof(val.str) - 1;
    }
    val.str[val.str_len] = '\0';

    char *endp;
    val.num = strtod(val.str, &endp);
    if (endp > val.str) {
        while (*endp == ' ' || *endp == '\t') endp++;
        val.is_num = (*endp == '\0');
    }
    return val;
}

static void tui_autofit_initial_widths(TUI *tui) {
    if (!tui || tui->num_cols == 0) return;

    // Use current terminal size
    tui_get_size(tui);

    Pane *mainp = &tui->panes[0];

    // How many data rows are visible on screen for main pane
    int visible_rows = tui_visible_data_rows(tui, mainp);
    if (visible_rows < 1) visible_rows = 1;

    for (uint16_t col = 0; col < tui->num_cols; col++) {
        int maxw = 1;

        // header width if present
        if (tui->has_header && tui->csv->row_count > 0) {
            const CellRef *hc = parsed_csv_get_cell(tui->csv, 0, col);
            if (hc) {
                size_t hl = cell_decode(tui->csv, hc, NULL, 0);
                if (hl > MAX_COL_WIDTH) hl = MAX_COL_WIDTH;
                if ((int)hl > maxw) maxw = (int)hl;
            }
        }

        // first screenful of data rows (pane 0 is unsorted initially)
        size_t total_rows = pane_row_count(tui, mainp);
        int rows_to_scan = visible_rows;
        if ((size_t)rows_to_scan > total_rows) rows_to_scan = (int)total_rows;

        for (int r = 0; r < rows_to_scan; r++) {
            size_t row_id = (mainp->row_ids != NULL) ? mainp->row_ids[r] : (size_t)r;
            size_t csv_row = tui_csv_row(tui, row_id);
            const CellRef *cell = parsed_csv_get_cell(tui->csv, csv_row, col);
            if (!cell) continue;

            size_t dl = cell_decode(tui->csv, cell, NULL, 0);
            if (dl > MAX_COL_WIDTH) dl = MAX_COL_WIDTH;
            if ((int)dl > maxw) maxw = (int)dl;
        }

        // Clamp: start tight, but don't exceed DEFAULT_COL_WIDTH on init
        if (maxw < 1) maxw = 1;
        if (maxw > DEFAULT_COL_WIDTH) maxw = DEFAULT_COL_WIDTH;

        tui->col_widths[col] = maxw;
    }
}

// ============================================================================
// Sorting
// ============================================================================

static TUI  *g_sort_tui  = NULL;
static Pane *g_sort_pane = NULL;

static int tui_sort_compare(const void *a, const void *b) {
    size_t base_a = *(const size_t *)a;
    size_t base_b = *(const size_t *)b;

    CellValue va = pane_get_cell_value(g_sort_tui, g_sort_pane, base_a, g_sort_pane->sort_col);
    CellValue vb = pane_get_cell_value(g_sort_tui, g_sort_pane, base_b, g_sort_pane->sort_col);

    // Empty values sort first
    if (va.is_empty && !vb.is_empty) return -1;
    if (!va.is_empty && vb.is_empty) return 1;
    if (va.is_empty && vb.is_empty) {
        return (base_a < base_b) ? -1 : (base_a > base_b) ? 1 : 0;
    }

    // Both numeric
    if (va.is_num && vb.is_num) {
        // INTEGER comparison for freq pane count/percent (no floats)
        if (va.is_int && vb.is_int) {
            if (va.i64 < vb.i64) return -1;
            if (va.i64 > vb.i64) return 1;
        } else {
            // Float comparison for CSV numeric cells (existing behavior)
            if (va.num < vb.num) return -1;
            if (va.num > vb.num) return 1;
        }
        // Stable sort: tie-break by base_index
        return (base_a < base_b) ? -1 : (base_a > base_b) ? 1 : 0;
    }

    // Mixed: numeric sorts before string
    if (va.is_num && !vb.is_num) return -1;
    if (!va.is_num && vb.is_num) return 1;

    // Both string: case-insensitive compare
    int cmp = strcasecmp(va.str, vb.str);
    if (cmp != 0) return cmp;
    return (base_a < base_b) ? -1 : (base_a > base_b) ? 1 : 0;
}

static void pane_sort_by_column(TUI *tui, Pane *pane, uint16_t col, bool ascending) {
    size_t pane_rows = pane_row_count(tui, pane);
    if (pane_rows == 0) return;

    size_t *new_index = realloc(pane->sort_index, pane_rows * sizeof(size_t));
    if (!new_index) return;
    pane->sort_index = new_index;
    pane->sort_len = pane_rows;

    // Initialize as identity permutation
    for (size_t i = 0; i < pane_rows; i++) {
        pane->sort_index[i] = i;
    }

    pane->sort_col = col;
    g_sort_tui = tui;
    g_sort_pane = pane;
    qsort(pane->sort_index, pane_rows, sizeof(size_t), tui_sort_compare);
    g_sort_tui = NULL;
    g_sort_pane = NULL;

    if (!ascending) {
        for (size_t i = 0; i < pane_rows / 2; i++) {
            size_t tmp = pane->sort_index[i];
            pane->sort_index[i] = pane->sort_index[pane_rows - 1 - i];
            pane->sort_index[pane_rows - 1 - i] = tmp;
        }
    }

    pane->sort_active = true;
    pane->sort_ascending = ascending;
    pane->cur_row = 0;
    pane->view_top = 0;
}

// ============================================================================
// Cell Comparison and Navigation
// ============================================================================

static bool pane_cells_equal(TUI *tui, Pane *pane, size_t dr1, size_t dr2, uint16_t col) {
    size_t bi1 = pane_display_to_base_index(pane, dr1);
    size_t bi2 = pane_display_to_base_index(pane, dr2);

    CellValue v1 = pane_get_cell_value(tui, pane, bi1, col);
    CellValue v2 = pane_get_cell_value(tui, pane, bi2, col);

    if (v1.is_empty && v2.is_empty) return true;
    if (v1.is_empty || v2.is_empty) return false;

    // For integers, compare i64; for floats, compare num
    if (v1.is_num != v2.is_num) return false;
    if (v1.is_num) {
        if (v1.is_int && v2.is_int) {
            return v1.i64 == v2.i64;
        }
        return v1.num == v2.num;
    }
    return strcasecmp(v1.str, v2.str) == 0;
}

static void pane_next_distinct(TUI *tui, Pane *pane) {
    size_t pane_rows = pane_row_count(tui, pane);
    if (pane_rows == 0) return;

    for (size_t i = pane->cur_row + 1; i < pane_rows; i++) {
        if (!pane_cells_equal(tui, pane, pane->cur_row, i, pane->cur_col)) {
            pane->cur_row = i;
            return;
        }
    }
}

static void pane_prev_distinct(TUI *tui, Pane *pane) {
    if (pane->cur_row == 0) return;

    for (size_t i = pane->cur_row - 1; ; i--) {
        if (!pane_cells_equal(tui, pane, pane->cur_row, i, pane->cur_col)) {
            pane->cur_row = i;
            return;
        }
        if (i == 0) break;
    }
}

// ============================================================================
// Search
// ============================================================================

static bool pane_cell_matches(TUI *tui, Pane *pane, size_t display_row, uint16_t col) {
    size_t base_index = pane_display_to_base_index(pane, display_row);
    CellValue val = pane_get_cell_value(tui, pane, base_index, col);

    if (val.is_empty) return false;

    // Search against display text (str matches display format exactly)
    return strcasestr(val.str, pane->search_buf) != NULL;
}

static bool pane_search(TUI *tui, Pane *pane, bool forward) {
    if (pane->search_len == 0) return false;

    size_t pane_rows = pane_row_count(tui, pane);
    if (pane_rows == 0) return false;

    uint16_t start_col = pane->search_start_col;
    uint16_t num_cols = pane_num_cols(tui, pane);

    if (forward) {
        for (size_t r = pane->cur_row + 1; r < pane_rows; r++) {
            if (pane_cell_matches(tui, pane, r, start_col)) {
                pane->cur_row = r;
                pane->cur_col = start_col;
                return true;
            }
        }
        for (size_t r = 0; r <= pane->cur_row; r++) {
            if (pane_cell_matches(tui, pane, r, start_col)) {
                pane->cur_row = r;
                pane->cur_col = start_col;
                return true;
            }
        }
        for (uint16_t c = 0; c < num_cols; c++) {
            if (c == start_col) continue;
            for (size_t r = 0; r < pane_rows; r++) {
                if (pane_cell_matches(tui, pane, r, c)) {
                    pane->cur_row = r;
                    pane->cur_col = c;
                    return true;
                }
            }
        }
    } else {
        if (pane->cur_row > 0) {
            for (size_t r = pane->cur_row - 1; ; r--) {
                if (pane_cell_matches(tui, pane, r, start_col)) {
                    pane->cur_row = r;
                    pane->cur_col = start_col;
                    return true;
                }
                if (r == 0) break;
            }
        }
        for (size_t r = pane_rows - 1; r >= pane->cur_row; r--) {
            if (pane_cell_matches(tui, pane, r, start_col)) {
                pane->cur_row = r;
                pane->cur_col = start_col;
                return true;
            }
            if (r == 0) break;
        }
        for (uint16_t c = 0; c < num_cols; c++) {
            if (c == start_col) continue;
            for (size_t r = pane_rows - 1; ; r--) {
                if (pane_cell_matches(tui, pane, r, c)) {
                    pane->cur_row = r;
                    pane->cur_col = c;
                    return true;
                }
                if (r == 0) break;
            }
        }
    }

    return false;
}

// ============================================================================
// Column Expansion
// ============================================================================

static void pane_expand_column(TUI *tui, Pane *pane) {
    if (pane->is_freq_pane) {
        // Only value column (0) is expandable; count (1) and percent (2) are fixed
        if (pane->cur_col != 0) return;

        int max_width = 0;
        int visible_rows = tui_visible_data_rows(tui, pane);
        size_t pane_rows = pane_row_count(tui, pane);

        for (int vr = 0; vr < visible_rows; vr++) {
            size_t display_row = pane->view_top + vr;
            if (display_row >= pane_rows) break;

            size_t base_index = pane_display_to_base_index(pane, display_row);
            if (base_index < pane->freq_row_count) {
                int len = (int)pane->freq_rows[base_index].value_len;
                if (len > max_width) max_width = len;
            }
        }

        if (max_width > MAX_COL_WIDTH) max_width = MAX_COL_WIDTH;
        if (max_width < 1) max_width = 1;
        if (max_width > pane->freq_col_widths[0]) {
            pane->freq_col_widths[0] = max_width;
        }
        return;
    }

    // CSV pane: existing logic
    int max_width = 0;

    // Check header if present
    if (tui->has_header && tui->csv->row_count > 0) {
        const CellRef *cell = parsed_csv_get_cell(tui->csv, 0, pane->cur_col);
        if (cell && (int)cell->len > max_width) max_width = cell->len;
    }

    // Check visible data rows in this pane
    int visible_rows = tui_visible_data_rows(tui, pane);
    size_t pane_rows = pane_row_count(tui, pane);

    for (int vr = 0; vr < visible_rows; vr++) {
        size_t display_row = pane->view_top + vr;
        if (display_row >= pane_rows) break;

        size_t base_index = pane_display_to_base_index(pane, display_row);
        size_t row_id = (pane->row_ids != NULL) ? pane->row_ids[base_index] : base_index;
        size_t csv_row = tui_csv_row(tui, row_id);

        const CellRef *cell = parsed_csv_get_cell(tui->csv, csv_row, pane->cur_col);
        if (cell) {
            char tmp[MAX_COL_WIDTH + 1];
            size_t decoded_len = cell_decode(tui->csv, cell, tmp, MAX_COL_WIDTH);
            if ((int)decoded_len > max_width) max_width = (int)decoded_len;
        }
    }

    if (max_width > MAX_COL_WIDTH) max_width = MAX_COL_WIDTH;
    if (max_width < 1) max_width = 1;
    if (max_width > tui->col_widths[pane->cur_col]) {
        tui->col_widths[pane->cur_col] = max_width;
    }
}

// ============================================================================
// Frequency Table Construction
// ============================================================================

#define FREQ_MAP_INIT_CAP 256
#define FREQ_MAP_MAX_KEY_LEN 65536  // 64KB cap per key

typedef struct FreqEntry {
    char   *key;      // Owned string, NULL = empty slot
    size_t  key_len;
    size_t  count;
    uint32_t hash;
} FreqEntry;

typedef struct FreqMap {
    FreqEntry *entries;
    size_t     capacity;
    size_t     count;
} FreqMap;

static uint32_t freq_hash(const char *key, size_t len) {
    // FNV-1a hash
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)key[i];
        h *= 16777619u;
    }
    return h;
}

static FreqMap freqmap_create(size_t init_cap) {
    FreqMap m = {0};
    m.capacity = init_cap > 0 ? init_cap : FREQ_MAP_INIT_CAP;
    m.entries = calloc(m.capacity, sizeof(FreqEntry));
    return m;
}

// Single cleanup function - always safe to call
static void freqmap_free(FreqMap *m) {
    if (m->entries) {
        for (size_t i = 0; i < m->capacity; i++) {
            free(m->entries[i].key);  // free(NULL) is safe
        }
        free(m->entries);
    }
    m->entries = NULL;
    m->capacity = 0;
    m->count = 0;
}

static bool freqmap_grow(FreqMap *m) {
    size_t new_cap = m->capacity * 2;
    FreqEntry *new_entries = calloc(new_cap, sizeof(FreqEntry));
    if (!new_entries) return false;

    for (size_t i = 0; i < m->capacity; i++) {
        if (m->entries[i].key) {
            uint32_t idx = m->entries[i].hash % new_cap;
            while (new_entries[idx].key) {
                idx = (idx + 1) % new_cap;
            }
            new_entries[idx] = m->entries[i];
        }
    }

    free(m->entries);  // Don't free keys - they moved to new_entries
    m->entries = new_entries;
    m->capacity = new_cap;
    return true;
}

// Increment count for key, inserting if new. Key is copied.
static bool freqmap_increment(FreqMap *m, const char *key, size_t key_len) {
    // Grow if > 70% full
    if (m->count * 10 > m->capacity * 7) {
        if (!freqmap_grow(m)) return false;
    }

    uint32_t hash = freq_hash(key, key_len);
    uint32_t idx = hash % m->capacity;

    while (m->entries[idx].key) {
        if (m->entries[idx].hash == hash &&
                m->entries[idx].key_len == key_len &&
                memcmp(m->entries[idx].key, key, key_len) == 0) {
            // Found existing entry
            m->entries[idx].count++;
            return true;
        }
        idx = (idx + 1) % m->capacity;
    }

    // Insert new entry (copy key)
    m->entries[idx].key = malloc(key_len + 1);
    if (!m->entries[idx].key) return false;
    memcpy(m->entries[idx].key, key, key_len);
    m->entries[idx].key[key_len] = '\0';
    m->entries[idx].key_len = key_len;
    m->entries[idx].count = 1;
    m->entries[idx].hash = hash;
    m->count++;
    return true;
}

// Extract entries to FreqRow array, transferring key ownership
// After this call, freqmap_free() is still safe (keys are NULLed)
static FreqRow *freqmap_extract(FreqMap *m, size_t denom, size_t *out_count) {
    if (m->count == 0) {
        *out_count = 0;
        return NULL;
    }

    FreqRow *rows = malloc(m->count * sizeof(FreqRow));
    if (!rows) {
        *out_count = 0;
        return NULL;
    }

    size_t idx = 0;
    for (size_t i = 0; i < m->capacity; i++) {
        if (m->entries[i].key) {
            rows[idx].value = m->entries[i].key;  // Transfer ownership
            rows[idx].value_len = m->entries[i].key_len;
            rows[idx].count = m->entries[i].count;

            // Compute percent with rounding: (count * 10000 + denom/2) / denom
            // Use 64-bit arithmetic to prevent overflow
            uint64_t numer = (uint64_t)m->entries[i].count * 10000 + denom / 2;
            rows[idx].pct_bp = (int32_t)(numer / denom);

            m->entries[i].key = NULL;  // Prevent double-free in freqmap_free
            idx++;
        }
    }

    *out_count = idx;
    return rows;
}

// Comparator for sorting FreqRow by count descending (default order)
static int freqrow_cmp_count_desc(const void *a, const void *b) {
    const FreqRow *ra = (const FreqRow *)a;
    const FreqRow *rb = (const FreqRow *)b;

    // Descending order: higher count first
    if (ra->count > rb->count) return -1;
    if (ra->count < rb->count) return 1;

    // Tie-break: alphabetical by value (ascending)
    return strcmp(ra->value, rb->value);
}

// Build frequency table for a column
// - Analyzes ALL rows in the pane (ignores selection)
// - Empty/missing values excluded from table AND denominator
// - Uses cell_decode() with NULL for length query (single source of truth)
// - Uses reusable scratch buffer to reduce allocator churn
// - Returns array sorted by count descending (default order)
// Returns: owned FreqRow array (caller must free), or NULL if empty/error
static FreqRow *build_frequency_table(TUI *tui, Pane *src_pane, uint16_t col,
        size_t *out_count) {
    *out_count = 0;

    // Guard: cannot build frequency from frequency pane
    if (src_pane->is_freq_pane) {
        return NULL;
    }

    size_t pane_rows = pane_row_count(tui, src_pane);
    if (pane_rows == 0) {
        return NULL;
    }

    FreqMap map = freqmap_create(FREQ_MAP_INIT_CAP);
    if (!map.entries) {
        return NULL;
    }

    // Reusable scratch buffer to reduce malloc/free churn
    char *scratch = NULL;
    size_t scratch_cap = 0;

    size_t denom = 0;  // Count of cells (denominator for percent)

    // Iterate ALL rows in the pane
    for (size_t bi = 0; bi < pane_rows; bi++) {
        // Derive row_id internally (no pane_display_to_row_id helper)
        size_t row_id = (src_pane->row_ids != NULL) ? src_pane->row_ids[bi] : bi;
        size_t csv_row = tui_csv_row(tui, row_id);
        const CellRef *cell = parsed_csv_get_cell(tui->csv, csv_row, col);

        if (!cell) {
            // Missing column -> NULL
            static const char null_key[] = "<NULL>";
            if (!freqmap_increment(&map, null_key, sizeof(null_key) - 1)) {
                free(scratch);
                freqmap_free(&map);
                return NULL;
            }
            denom++;   // NULLs included in denominator
            continue;
        }

        if (cell->len == 0) {
            static const char blank_key[] = "";
            if (!freqmap_increment(&map, blank_key, sizeof(blank_key) - 1)) {
                free(scratch);
                freqmap_free(&map);
                return NULL;
            }
            denom++;   // blanks included in denominator
            continue;
        }


        // Get exact decoded length using cell_decode with NULL buffer
        size_t decoded_len = cell_decode(tui->csv, cell, NULL, 0);

        // Apply explicit per-key cap
        if (decoded_len > FREQ_MAP_MAX_KEY_LEN) {
            decoded_len = FREQ_MAP_MAX_KEY_LEN;
        }

        // Grow scratch buffer as needed (include space for NUL terminator)
        size_t needed = decoded_len + 1;
        if (needed > scratch_cap) {
            size_t new_cap = needed * 2;
            if (new_cap < 256) new_cap = 256;
            char *new_scratch = realloc(scratch, new_cap);
            if (!new_scratch) {
                free(scratch);
                freqmap_free(&map);
                return NULL;
            }
            scratch = new_scratch;
            scratch_cap = new_cap;
        }

        // Decode into scratch buffer (pass decoded_len + 1 for NUL space)
        size_t actual_len = cell_decode(tui->csv, cell, scratch, decoded_len + 1);
        if (actual_len > decoded_len) actual_len = decoded_len;  // Respect cap
        scratch[actual_len] = '\0';

        // Insert/increment in map
        if (!freqmap_increment(&map, scratch, actual_len)) {
            free(scratch);
            freqmap_free(&map);
            return NULL;
        }

        denom++;
    }

    free(scratch);

    // No non-empty values found
    if (denom == 0 || map.count == 0) {
        freqmap_free(&map);
        return NULL;
    }

    // Extract to FreqRow array (transfers key ownership, NULLs map keys)
    FreqRow *rows = freqmap_extract(&map, denom, out_count);

    // Single cleanup path - safe even after extract (keys are NULL)
    freqmap_free(&map);

    if (!rows) {
        return NULL;
    }

    // Sort by count descending (default order for frequency panes)
    qsort(rows, *out_count, sizeof(FreqRow), freqrow_cmp_count_desc);

    return rows;
}

static void select_rows_in_main_matching_freq_key(TUI *tui, uint16_t source_col,
        const char *key, size_t key_len)
{
    if (!tui || tui->pane_count == 0) return;

    // "Main data view" = pane 0
    Pane *mainp = &tui->panes[0];
    if (mainp->is_freq_pane) return; // should never happen

    // Reusable scratch for decoded cell
    char *scratch = NULL;
    size_t scratch_cap = 0;

    static const char null_key[] = "<NULL>";

    size_t pane_rows = pane_row_count(tui, mainp);
    for (size_t bi = 0; bi < pane_rows; bi++) {
        size_t row_id = (mainp->row_ids != NULL) ? mainp->row_ids[bi] : bi;
        size_t csv_row = tui_csv_row(tui, row_id);
        const CellRef *cell = parsed_csv_get_cell(tui->csv, csv_row, source_col);

        bool match = false;

        if (!cell) {
            match = (key_len == (sizeof(null_key) - 1)) &&
                (memcmp(key, null_key, sizeof(null_key) - 1) == 0);
        } else if (cell->len == 0) {
            match = (key_len == 0);
        } else {
            // Match the same truncation rule used by frequency build
            size_t decoded_len = cell_decode(tui->csv, cell, NULL, 0);
            if (decoded_len > FREQ_MAP_MAX_KEY_LEN) decoded_len = FREQ_MAP_MAX_KEY_LEN;

            if (decoded_len == key_len) {
                size_t needed = decoded_len + 1;
                if (needed > scratch_cap) {
                    size_t new_cap = needed * 2;
                    if (new_cap < 256) new_cap = 256;
                    char *ns = realloc(scratch, new_cap);
                    if (!ns) break;
                    scratch = ns;
                    scratch_cap = new_cap;
                }

                size_t actual_len = cell_decode(tui->csv, cell, scratch, decoded_len + 1);
                if (actual_len > decoded_len) actual_len = decoded_len;
                scratch[actual_len] = '\0';

                match = (actual_len == key_len) && (memcmp(scratch, key, key_len) == 0);
            }
        }

        if (match) {
            // main pane selection domain is base_index == row_id when row_ids==NULL
            // but handle the row_ids!=NULL case anyway
            if (mainp->row_ids == NULL) {
                pane_select(mainp, row_id);
            } else {
                // find base_index for this row_id inside mainp
                for (size_t mbi = 0; mbi < mainp->row_id_count; mbi++) {
                    if (mainp->row_ids[mbi] == row_id) {
                        pane_select(mainp, mbi);
                        break;
                    }
                }
            }
        }
    }

    free(scratch);
}

// ============================================================================
// Rendering
// ============================================================================

// Write cell content to buffer for CSV headers (row 0 direct access)
// This is an intentional exception: headers render directly from CSV row 0
static int write_cell(TUI *tui, char *buf, int buf_len, size_t row, uint16_t col, int width) {
    const CellRef *cell = parsed_csv_get_cell(tui->csv, row, col);

    char decoded[MAX_COL_WIDTH + 1];
    size_t decoded_len = 0;

    if (cell) {
        decoded_len = cell_decode(tui->csv, cell, decoded, MAX_COL_WIDTH);
        if (decoded_len > MAX_COL_WIDTH) decoded_len = MAX_COL_WIDTH;
    }

    int written = 0;
    int display_width = width < MAX_COL_WIDTH ? width : MAX_COL_WIDTH;

    for (int i = 0; i < display_width && written < buf_len - 1; i++) {
        if (i < (int)decoded_len) {
            char c = decoded[i];
            if (c < 32 || c == 127) c = '?';
            buf[written++] = c;
        } else {
            buf[written++] = ' ';
        }
    }

    return written;
}

// Write cell content to buffer for data rows (uses base_index mapping)
// Uses pane_get_cell_text internally; display is capped at MAX_COL_WIDTH
static int write_pane_cell(TUI *tui, Pane *pane, char *buf, int buf_len,
        size_t base_index, uint16_t col, int width) {
    char decoded[MAX_COL_WIDTH + 1];
    size_t decoded_len = pane_get_cell_text(tui, pane, base_index, col,
            decoded, MAX_COL_WIDTH);
    if (decoded_len > MAX_COL_WIDTH) decoded_len = MAX_COL_WIDTH;

    int written = 0;
    int display_width = width < MAX_COL_WIDTH ? width : MAX_COL_WIDTH;

    for (int i = 0; i < display_width && written < buf_len - 1; i++) {
        if (i < (int)decoded_len) {
            char c = decoded[i];
            if (c < 32 || c == 127) c = '?';  // Replace control chars
            buf[written++] = c;
        } else {
            buf[written++] = ' ';  // Pad to width
        }
    }

    return written;
}

static void tui_draw(TUI *tui) {
    char buf[65536];
    int pos = 0;

    Pane *pane = &tui->panes[tui->active_pane];
    uint16_t num_cols = pane_num_cols(tui, pane);

    pos += snprintf(buf + pos, sizeof(buf) - pos, "\x1b[H\x1b[2J");

    int row_on_screen = 0;
    bool has_more_left = pane->view_left > 0;
    bool has_more_right = false;

    // Draw header if present (CSV panes with headers only)
    // NOTE: write_cell() is used here intentionally - headers access CSV row 0 directly
    if (!pane->is_freq_pane && tui->has_header && tui->csv->row_count > 0) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\x1b[1;7m");

        int x = 0;
        buf[pos++] = has_more_left ? '<' : ' '; x++;

        uint16_t last_col_drawn = pane->view_left;
        for (uint16_t c = pane->view_left; c < num_cols; c++) {
            bool is_first = (c == pane->view_left);
            int sep_len = is_first ? 0 : 3;

            if (tui->term_cols - x < sep_len + 1) break;

            if (!is_first) {
                buf[pos++] = ' '; buf[pos++] = '|'; buf[pos++] = ' ';
                x += 3;
            }

            int avail = tui->term_cols - x - 1;
            int w = pane_col_width(tui, pane, c);
            if (w > avail) w = avail;
            if (w < 0) w = 0;

            if (w > 0) {
                // Header uses write_cell() with CSV row 0 directly
                pos += write_cell(tui, buf + pos, sizeof(buf) - pos, 0, c, w);
                x += w;
            }
            last_col_drawn = c;
        }

        has_more_right = (last_col_drawn < num_cols - 1);
        buf[pos++] = has_more_right ? '>' : ' '; x++;

        while (x < tui->term_cols) { buf[pos++] = ' '; x++; }

        pos += snprintf(buf + pos, sizeof(buf) - pos, "\x1b[0m\r\n");
        row_on_screen++;
    }

    // Draw data rows
    int visible_rows = tui_visible_data_rows(tui, pane);
    size_t pane_rows = pane_row_count(tui, pane);

    for (int vr = 0; vr < visible_rows; vr++) {
        size_t display_row = pane->view_top + vr;
        if (display_row >= pane_rows) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "\x1b[K\r\n");
            continue;
        }

        size_t base_index = pane_display_to_base_index(pane, display_row);
        bool is_cur_row = (display_row == pane->cur_row);
        bool row_selected = pane_is_selected(pane, base_index);  // base_index keyed

        int x = 0;

        // Left padding with selection indicator
        if (row_selected) {
            // UTF-8 bullet: E2 80 A2 (3 bytes, 1 terminal column)
            buf[pos++] = '\xe2';
            buf[pos++] = '\x80';
            buf[pos++] = '\xa2';
        } else {
            buf[pos++] = ' ';
        }
        x++;

        for (uint16_t c = pane->view_left; c < num_cols; c++) {
            bool is_first = (c == pane->view_left);
            int sep_len = is_first ? 0 : 3;

            if (tui->term_cols - x < sep_len + 1) break;

            if (!is_first) {
                pos += snprintf(buf + pos, sizeof(buf) - pos, "\x1b[0m");
                buf[pos++] = ' '; buf[pos++] = '|'; buf[pos++] = ' ';
                x += 3;
            }

            int avail = tui->term_cols - x - 1;
            int w = pane_col_width(tui, pane, c);
            if (w > avail) w = avail;
            if (w < 0) w = 0;

            if (w > 0) {
                bool is_cur_cell = is_cur_row && (c == pane->cur_col);

                if (is_cur_cell) {
                    pos += snprintf(buf + pos, sizeof(buf) - pos, "\x1b[7m");
                } else if (is_cur_row) {
                    pos += snprintf(buf + pos, sizeof(buf) - pos, "\x1b[4m");
                }

                // Data rows use write_pane_cell() with base_index
                pos += write_pane_cell(tui, pane, buf + pos, sizeof(buf) - pos,
                        base_index, c, w);

                if (is_cur_cell || is_cur_row) {
                    pos += snprintf(buf + pos, sizeof(buf) - pos, "\x1b[0m");
                }

                x += w;
            }
        }

        pos += snprintf(buf + pos, sizeof(buf) - pos, "\x1b[0m");
        buf[pos++] = ' '; x++;

        pos += snprintf(buf + pos, sizeof(buf) - pos, "\x1b[K\r\n");
    }

    // Status line - render to temp buffer, then truncate to terminal width
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\x1b[7m");
    char status_tmp[512];
    int status_len;
    if (pane->col_search_active) {
        status_len = snprintf(status_tmp, sizeof(status_tmp), "c/%s", pane->col_search_buf);
    } else if (pane->search_active) {
        status_len = snprintf(status_tmp, sizeof(status_tmp), "/%s", pane->search_buf);
    } else if (pane->is_freq_pane) {
        size_t selected = pane_count_selected(tui, pane);
        status_len = snprintf(status_tmp, sizeof(status_tmp),
                " Pane %zu/%zu [Freq col %u]  Sel %zu  Row %zu/%zu  Col %u/3 ",
                tui->active_pane + 1, tui->pane_count,
                pane->freq_source_col + 1,
                selected,
                pane->cur_row + 1, pane_rows,
                pane->cur_col + 1);
    } else {
        size_t selected = pane_count_selected(tui, pane);
        status_len = snprintf(status_tmp, sizeof(status_tmp),
                " Pane %zu/%zu  Sel %zu  Row %zu/%zu  Col %u/%u ",
                tui->active_pane + 1, tui->pane_count,
                selected,
                pane->cur_row + 1, pane_rows,
                pane->cur_col + 1, num_cols);
    }
    if (status_len > tui->term_cols) status_len = tui->term_cols;
    memcpy(buf + pos, status_tmp, status_len);
    pos += status_len;

    for (int i = status_len; i < tui->term_cols; i++) buf[pos++] = ' ';
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\x1b[0m");

    write(STDOUT_FILENO, buf, pos);
}


// ============================================================================
// delimiter detection
// ============================================================================

static size_t scan_line_cols_outside_quotes(const char *s, size_t n, char delim) {
    bool in_quotes = false;
    size_t delims = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '"') {
            if (in_quotes && i + 1 < n && s[i + 1] == '"') {
                i++; // skip escaped quote
            } else {
                in_quotes = !in_quotes;
            }
        } else if (!in_quotes && c == delim) {
            delims++;
        }
    }
    return delims + 1; // columns
}

static bool autodetect_delimiter_first_lines(const Buffer *buf, char *out_delim) {
    static const char CANDS[] = { ',', '|', '\t', ';' };

    const char *p = buf->data;
    size_t len = buf->len;

    size_t lines_seen = 0;
    size_t max_lines = 1000;

    // Stats per candidate
    typedef struct {
        size_t rows_with_delim;
        size_t min_cols;
        size_t max_cols;
    } Stat;

    Stat st[sizeof(CANDS)] = {0};
    for (size_t k = 0; k < sizeof(CANDS); k++) {
        st[k].min_cols = (size_t)-1;
        st[k].max_cols = 0;
        st[k].rows_with_delim = 0;
    }

    size_t i = 0;
    while (i < len && lines_seen < max_lines) {
        // find line end
        size_t line_start = i;
        while (i < len && buf->data[i] != '\n' && buf->data[i] != '\r') i++;
        size_t line_len = i - line_start;

        // skip CRLF / CR
        if (i < len && buf->data[i] == '\r') {
            i++;
            if (i < len && buf->data[i] == '\n') i++;
        } else if (i < len && buf->data[i] == '\n') {
            i++;
        }

        // ignore completely empty lines
        if (line_len == 0) continue;

        const char *line = p + line_start;

        for (size_t k = 0; k < sizeof(CANDS); k++) {
            char d = CANDS[k];

            // count delims outside quotes via col count
            size_t cols = scan_line_cols_outside_quotes(line, line_len, d);
            // If cols==1, delimiter not present outside quotes
            if (cols > 1) {
                st[k].rows_with_delim++;
                if (cols < st[k].min_cols) st[k].min_cols = cols;
                if (cols > st[k].max_cols) st[k].max_cols = cols;
            }
        }

        lines_seen++;
    }

    // choose best
    // score = rows_with_delim*10 - (max_cols - min_cols)
    // require at least 2 rows with delimiter and at least 2 cols somewhere
    int best_k = -1;
    long best_score = -999999;

    for (size_t k = 0; k < sizeof(CANDS); k++) {
        if (st[k].rows_with_delim < 2) continue;
        if (st[k].max_cols < 2) continue;
        if (st[k].min_cols == (size_t)-1) continue;

        long spread = (long)(st[k].max_cols - st[k].min_cols);
        long score = (long)st[k].rows_with_delim * 10L - spread;

        // tie-break priority in CANDS order (',', '|', '\t', ';')
        if (score > best_score) {
            best_score = score;
            best_k = (int)k;
        }
    }

    if (best_k == -1) return false;
    *out_delim = CANDS[best_k];
    return true;
}

// ============================================================================
// Scrolling
// ============================================================================

static void tui_scroll_to_cursor(TUI *tui, Pane *pane) {
    int visible_rows = tui_visible_data_rows(tui, pane);
    size_t pane_rows = pane_row_count(tui, pane);

    // Vertical scrolling
    if (pane->cur_row < pane->view_top) {
        pane->view_top = pane->cur_row;
    } else if (pane->cur_row >= pane->view_top + (size_t)visible_rows) {
        pane->view_top = pane->cur_row - visible_rows + 1;
    }

    // Clamp view_top
    if (pane_rows > 0 && pane->view_top > pane_rows - 1) {
        pane->view_top = pane_rows - 1;
    }

    // Horizontal scrolling
    if (pane->cur_col < pane->view_left) {
        pane->view_left = pane->cur_col;
    } else {
        int x = 1;
        uint16_t num_cols = pane_num_cols(tui, pane);
        for (uint16_t c = pane->view_left; c <= pane->cur_col && c < num_cols; c++) {
            if (c > pane->view_left) x += 3;
            x += pane_col_width(tui, pane, c);
        }
        x += 1;
        while (x > tui->term_cols && pane->view_left < pane->cur_col) {
            x -= pane_col_width(tui, pane, pane->view_left) + 3;
            pane->view_left++;
        }
    }
}

// ============================================================================
// Key Handling
// ============================================================================

static int tui_read_key(void) {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1) return -1;
    }

    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '5': return 'U' - 64;
                        case '6': return 'D' - 64;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return 'k';
                    case 'B': return 'j';
                    case 'C': return 'l';
                    case 'D': return 'h';
                    case 'H': return 'g';
                    case 'F': return 'G';
                }
            }
        }
        return '\x1b';
    }

    return c;
}

static void tui_process_key(TUI *tui, int key) {
    Pane *pane = &tui->panes[tui->active_pane];
    size_t pane_rows = pane_row_count(tui, pane);
    int visible_rows = tui_visible_data_rows(tui, pane);
    int half_page = visible_rows / 2;
    if (half_page < 1) half_page = 1;

    // Unconditional quit on Ctrl+C
    if (key == 0x03) {
        tui->running = false;
        return;
    }

    // Handle search input mode
    if (pane->search_active) {
        if (key == '\x1b') {
            pane->search_active = false;
            pane->search_len = 0;
            pane->search_buf[0] = '\0';
        } else if (key == '\r' || key == '\n') {
            pane->search_active = false;
            pane->search_has_query = (pane->search_len > 0);
            if (pane->search_has_query) {
                pane_search(tui, pane, true);
            }
        } else if (key == 127 || key == '\b') {
            if (pane->search_len > 0) {
                pane->search_len--;
                pane->search_buf[pane->search_len] = '\0';
            }
        } else if (key >= 32 && key < 127) {
            if (pane->search_len < sizeof(pane->search_buf) - 1) {
                pane->search_buf[pane->search_len++] = (char)key;
                pane->search_buf[pane->search_len] = '\0';
            }
        }
        return;
    }

    // Handle column search input mode
    if (pane->col_search_active) {
        if (key == '\x1b') {
            pane->col_search_active = false;
            pane->col_search_len = 0;
            pane->col_search_buf[0] = '\0';
        } else if (key == '\r' || key == '\n') {
            pane->col_search_active = false;
            if (pane->col_search_len > 0 && tui->has_header && !pane->is_freq_pane
                    && tui->csv->row_count > 0) {
                // Search column headers for a case-insensitive substring match
                for (uint16_t c = 0; c < tui->num_cols; c++) {
                    const CellRef *hc = parsed_csv_get_cell(tui->csv, 0, c);
                    if (!hc) continue;
                    char hdr[MAX_COL_WIDTH + 1];
                    size_t hdr_len = cell_decode(tui->csv, hc, hdr, MAX_COL_WIDTH);
                    if (hdr_len > MAX_COL_WIDTH) hdr_len = MAX_COL_WIDTH;
                    hdr[hdr_len] = '\0';
                    if (strcasestr(hdr, pane->col_search_buf) != NULL) {
                        pane->cur_col = c;
                        break;
                    }
                }
            }
            pane->col_search_len = 0;
            pane->col_search_buf[0] = '\0';
        } else if (key == 127 || key == '\b') {
            if (pane->col_search_len > 0) {
                pane->col_search_len--;
                pane->col_search_buf[pane->col_search_len] = '\0';
            }
        } else if (key >= 32 && key < 127) {
            if (pane->col_search_len < sizeof(pane->col_search_buf) - 1) {
                pane->col_search_buf[pane->col_search_len++] = (char)key;
                pane->col_search_buf[pane->col_search_len] = '\0';
            }
        }
        return;
    }

    switch (key) {
        case 'q':
            if (tui->pane_count > 1) {
                panes_remove(tui, tui->active_pane);
                clamp_cursor(&tui->panes[tui->active_pane], tui);
            }
            // else: no-op, cannot close main pane
            break;

        case '\t':  // Tab - cycle pane
            tui->active_pane = (tui->active_pane + 1) % tui->pane_count;
            clamp_cursor(&tui->panes[tui->active_pane], tui);
            break;

        case 'i':  // Select current row + move down
            if (pane_rows == 0) break;
            {
                size_t base_index = pane_display_to_base_index(pane, pane->cur_row);
                pane_select(pane, base_index);

                if (pane->is_freq_pane && base_index < pane->freq_row_count) {
                    FreqRow *fr = &pane->freq_rows[base_index];
                    // Select matching rows in main data view (pane 0)
                    select_rows_in_main_matching_freq_key(tui, pane->freq_source_col, fr->value, fr->value_len);
                }

                if (pane->cur_row < pane_rows - 1) {
                    pane->cur_row++;
                }
            }
            break;

        case 'u':  // Unselect current row
            if (pane_rows == 0) break;
            {
                size_t base_index = pane_display_to_base_index(pane, pane->cur_row);
                pane_unselect(pane, base_index);
            }
            break;

        case 's':  // Split - create new pane from selected rows
            {
                size_t total_rows = pane_row_count(tui, pane);

                // Count selected base_indices
                size_t selected_count = 0;
                for (size_t bi = 0; bi < total_rows; bi++) {
                    if (pane_is_selected(pane, bi)) selected_count++;
                }
                if (selected_count == 0) break;

                if (pane->is_freq_pane) {
                    // Split frequency pane: deep copy selected FreqRows
                    FreqRow *new_freq = malloc(selected_count * sizeof(FreqRow));
                    if (!new_freq) break;

                    size_t idx = 0;
                    for (size_t bi = 0; bi < total_rows; bi++) {
                        if (pane_is_selected(pane, bi)) {
                            FreqRow *src = &pane->freq_rows[bi];
                            new_freq[idx].value = malloc(src->value_len + 1);
                            if (!new_freq[idx].value) {
                                // Cleanup on failure
                                for (size_t j = 0; j < idx; j++) free(new_freq[j].value);
                                free(new_freq);
                                new_freq = NULL;
                                break;
                            }
                            memcpy(new_freq[idx].value, src->value, src->value_len + 1);
                            new_freq[idx].value_len = src->value_len;
                            new_freq[idx].count = src->count;
                            new_freq[idx].pct_bp = src->pct_bp;
                            idx++;
                        }
                    }
                    if (!new_freq) break;

                    Pane *new_pane = panes_add(tui);
                    if (!new_pane) {
                        for (size_t j = 0; j < selected_count; j++) free(new_freq[j].value);
                        free(new_freq);
                        break;
                    }

                    pane_init_freq(new_pane, new_freq, selected_count, pane->freq_source_col, pane->parent_csv_pane);
                    tui->active_pane = tui->pane_count - 1;

                } else {
                    // Split CSV pane: collect row_ids for selected base_indices
                    size_t *new_row_ids = malloc(selected_count * sizeof(size_t));
                    if (!new_row_ids) break;

                    size_t idx = 0;
                    for (size_t bi = 0; bi < total_rows; bi++) {
                        if (pane_is_selected(pane, bi)) {
                            // Derive row_id internally
                            size_t row_id = (pane->row_ids != NULL) ? pane->row_ids[bi] : bi;
                            new_row_ids[idx++] = row_id;
                        }
                    }

                    Pane *new_pane = panes_add(tui);
                    if (!new_pane) {
                        free(new_row_ids);
                        break;
                    }

                    pane_init_csv(new_pane, tui, new_row_ids, selected_count);
                    tui->active_pane = tui->pane_count - 1;
                }
            }
            break;

        case 'f':  // Frequency analysis for current column
                   // No recursion: f is no-op on frequency panes
            if (pane->is_freq_pane) {
                break;
            }
            {
                size_t freq_count = 0;
                FreqRow *freq_rows = build_frequency_table(tui, pane, pane->cur_col, &freq_count);

                // Empty column or allocation failure
                if (!freq_rows || freq_count == 0) {
                    break;
                }

                Pane *new_pane = panes_add(tui);
                if (!new_pane) {
                    for (size_t i = 0; i < freq_count; i++) free(freq_rows[i].value);
                    free(freq_rows);
                    break;
                }

                pane_init_freq(new_pane, freq_rows, freq_count, pane->cur_col, (int)tui->active_pane);
                tui->active_pane = tui->pane_count - 1;
            }
            break;

        case 'j':  // down
            if (pane_rows > 0 && pane->cur_row < pane_rows - 1) pane->cur_row++;
            break;

        case 'k':  // up
            if (pane->cur_row > 0) pane->cur_row--;
            break;

        case 'h':  // left
            if (pane->cur_col > 0) pane->cur_col--;
            break;

        case 'l':  // right
            {
                uint16_t max_col = pane_num_cols(tui, pane);
                if (max_col > 0 && pane->cur_col < max_col - 1) pane->cur_col++;
            }
            break;

        case 'g':  // top
            pane->cur_row = 0;
            break;

        case 'G':  // bottom
            if (pane_rows > 0) pane->cur_row = pane_rows - 1;
            break;

        case '0':  // first column
            pane->cur_col = 0;
            break;

        case '$':  // last column
            {
                uint16_t max_col = pane_num_cols(tui, pane);
                if (max_col > 0) pane->cur_col = max_col - 1;
            }
            break;

        case 'D' - 64:  // Ctrl+D - half page down
            if (pane_rows > 0) {
                pane->cur_row += half_page;
                if (pane->cur_row >= pane_rows) pane->cur_row = pane_rows - 1;
            }
            break;

        case 'U' - 64:  // Ctrl+U - half page up
            if (pane->cur_row >= (size_t)half_page) {
                pane->cur_row -= half_page;
            } else {
                pane->cur_row = 0;
            }
            break;

        case '_':  // expand column
            pane_expand_column(tui, pane);
            break;

        case '[':  // sort descending
            pane_sort_by_column(tui, pane, pane->cur_col, false);
            break;

        case ']':  // sort ascending
            pane_sort_by_column(tui, pane, pane->cur_col, true);
            break;

        case '=':  // clear sort
            pane_clear_sort(pane);
            break;

        case '<':  // prev distinct
            pane_prev_distinct(tui, pane);
            break;

        case '>':  // next distinct
            pane_next_distinct(tui, pane);
            break;

        case 'c':  // column search
            if (!pane->is_freq_pane && tui->has_header) {
                pane->col_search_active = true;
                pane->col_search_len = 0;
                pane->col_search_buf[0] = '\0';
            }
            break;

        case '/':  // enter search mode
            pane->search_active = true;
            pane->search_len = 0;
            pane->search_buf[0] = '\0';
            pane->search_start_col = pane->cur_col;
            break;

        case 'n':  // next match
            if (pane->search_has_query) {
                pane_search(tui, pane, true);
            }
            break;

        case 'N':  // previous match
            if (pane->search_has_query) {
                pane_search(tui, pane, false);
            }
            break;
    }
}

// ============================================================================
// Main Loop
// ============================================================================

static void tui_run(TUI *tui) {
    tui->running = true;

    while (tui->running) {
        tui_get_size(tui);
        Pane *active = &tui->panes[tui->active_pane];
        tui_scroll_to_cursor(tui, active);
        tui_draw(tui);

        int key = tui_read_key();
        if (key != -1) {
            tui_process_key(tui, key);
        }
    }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char *argv[]) {
    bool has_header = true;
    const char *filepath = NULL;
    bool user_delim_set = false;
    char delimiter = ',';

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0) {
            has_header = false;
        } else if (strcmp(argv[i], "-d") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: -d requires an argument\n");
                return 1;
            }
            const char *arg = argv[++i];
            if (strcmp(arg, "\\t") == 0) delimiter = '\t';
            else if (arg[0] != '\0' && arg[1] == '\0') delimiter = arg[0];
            else {
                fprintf(stderr, "error: -d must be a single character or \\t\n");
                return 1;
            }
            user_delim_set = true;
        } else if (argv[i][0] != '-') {
            filepath = argv[i];
        }
    }

    if (!filepath) {
        fprintf(stderr, "usage: %s [-n] <file.csv>\n", argv[0]);
        fprintf(stderr, "  -n  no header row\n");
        return 1;
    }

    Buffer buf = buffer_load(filepath);
    if (buf.data == NULL && buf.len > 0) return 1;

    ParsedCSV csv = parsed_csv_init(buf);
    if (csv.cells == NULL || csv.row_start == NULL) {
        fprintf(stderr, "out of memory\n");
        if (buf.data) munmap(buf.data, buf.len);
        return 1;
    }

    char delim_to_use = ',';

    if (user_delim_set) {
        delim_to_use = delimiter;
    } else {
        if (!autodetect_delimiter_first_lines(&buf, &delim_to_use)) {
            fprintf(stderr,
                    "could not detect delimiter; specify one with -d \",\" (or -d \"|\", -d \"\\\\t\", -d \";\")\n");
            parsed_csv_free(&csv);
            return 1;
        }
    }

    ParseResult result = parsed_csv_parse(&csv, delim_to_use);
    if (result != PARSE_OK) {
        const char *err;
        switch (result) {
            case PARSE_OOM: err = "out of memory"; break;
            case PARSE_INVALID: err = "invalid CSV format"; break;
            case PARSE_OVERFLOW: err = "overflow"; break;
            default: err = "unknown error"; break;
        }
        fprintf(stderr, "parse failed: %s\n", err);
        parsed_csv_free(&csv);
        return 1;
    }

    if (csv.row_count == 0) {
        fprintf(stderr, "empty file\n");
        parsed_csv_free(&csv);
        return 0;
    }

    // Initialize TUI
    TUI tui = {0};
    tui.csv = &csv;
    tui.has_header = has_header;
    tui.num_cols = csv.max_cols;

    // Initialize column widths
    tui.col_widths = malloc(tui.num_cols * sizeof(int));
    if (!tui.col_widths) {
        fprintf(stderr, "out of memory\n");
        parsed_csv_free(&csv);
        return 1;
    }
    for (uint16_t i = 0; i < tui.num_cols; i++) {
        tui.col_widths[i] = DEFAULT_COL_WIDTH;
    }

    // Initialize panes
    tui.pane_cap = 4;
    tui.panes = malloc(tui.pane_cap * sizeof(Pane));
    if (!tui.panes) {
        fprintf(stderr, "out of memory\n");
        free(tui.col_widths);
        parsed_csv_free(&csv);
        return 1;
    }

    // Create main pane
    pane_init_csv(&tui.panes[0], &tui, NULL, 0);
    tui.pane_count = 1;
    tui.active_pane = 0;

    tui_get_size(&tui);
    tui_autofit_initial_widths(&tui);

    tui_enable_raw(&tui);
    tui_run(&tui);
    tui_disable_raw(&tui);

    // Clear screen on exit
    write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);

    // Cleanup panes
    for (size_t i = 0; i < tui.pane_count; i++) {
        pane_free(&tui.panes[i]);
    }
    free(tui.panes);
    free(tui.col_widths);
    parsed_csv_free(&csv);

    return 0;
}
