/**
 * @file ui_input.c
 * @brief D-pad character picker view for composing prompts.
 */

#include "ui_input.h"
#include "uart_bridge.h"
#include <gui/canvas.h>
#include <input/input.h>
#include <stdlib.h>
#include <string.h>

#define TAG "ui_input"

// ---------------------------------------------------------------------------
// Character set definition (4 rows)
// ---------------------------------------------------------------------------

static const char* const CHARSET[] = {
    "0123456789",                                  // Row 0: digits
    "abcdefghijklmnopqrstuvwxyz ",                 // Row 1: lowercase + space
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ",                  // Row 2: uppercase
    ".,!?@#$%&*()-_+=/:;'\"<>[]\\",               // Row 3: symbols
};
static const int CHARSET_ROWS = 4;

static int charset_row_len(int row) {
    return (int)strlen(CHARSET[row]);
}

// ---------------------------------------------------------------------------
// View model
// ---------------------------------------------------------------------------

typedef struct {
    char   text[FC_MAX_INPUT_LEN + 1]; ///< Input string
    size_t text_len;
    int    cursor;       ///< Cursor position in text (0..text_len)
    int    char_row;     ///< Current character set row (0..3)
    int    char_col;     ///< Current column within char_row
} InputModel;

// ---------------------------------------------------------------------------
// Draw callback
// ---------------------------------------------------------------------------

static void input_draw(Canvas* canvas, void* model_ptr) {
    InputModel* m = (InputModel*)model_ptr;

    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);

    // Header
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "Enter prompt:");
    canvas_draw_line(canvas, 0, 11, 127, 11);

    // Input text display with cursor underline
    // Show up to 21 chars around the cursor
    canvas_set_font(canvas, FontSecondary);

    int display_start = m->cursor - FC_DISPLAY_LINE_CHARS + 1;
    if(display_start < 0) display_start = 0;
    if(display_start > (int)m->text_len) display_start = (int)m->text_len;

    char display_buf[FC_DISPLAY_LINE_CHARS + 2];
    int display_len = (int)m->text_len - display_start;
    if(display_len > FC_DISPLAY_LINE_CHARS) display_len = FC_DISPLAY_LINE_CHARS;
    if(display_len < 0) display_len = 0;

    memcpy(display_buf, m->text + display_start, display_len);
    display_buf[display_len] = '\0';
    canvas_draw_str(canvas, 0, 25, display_buf);

    // Draw cursor underline
    int cursor_x = (m->cursor - display_start) * 6; // ~6px per char in FontSecondary
    if(cursor_x >= 0 && cursor_x <= 126) {
        canvas_draw_line(canvas, cursor_x, 26, cursor_x + 5, 26);
    }

    // Current character indicator
    char cur_char = CHARSET[m->char_row][m->char_col];
    char picker_buf[24];
    // Show row indicator and current character
    const char* row_names[] = {"0-9", "a-z", "A-Z", "sym"};
    snprintf(picker_buf, sizeof(picker_buf), "[%s] '%c'",
             row_names[m->char_row], cur_char == ' ' ? '_' : cur_char);
    canvas_draw_str(canvas, 0, 42, picker_buf);

    // Hint line
    canvas_draw_line(canvas, 0, 55, 127, 55);
    canvas_draw_str(canvas, 0, 63, "\x18\x19=chr \x1a\x1b=cur OK=send");
}

// ---------------------------------------------------------------------------
// Input callback
// ---------------------------------------------------------------------------

static bool input_handler(InputEvent* event, void* ctx) {
    FlipperClawApp* app = (FlipperClawApp*)ctx;
    furi_assert(app);

    if(event->type != InputTypeShort && event->type != InputTypeRepeat) {
        return false;
    }

    bool consumed = false;

    with_view_model(app->view_input, InputModel*, m, {
        int row_len = charset_row_len(m->char_row);

        switch(event->key) {
        case InputKeyUp:
            // Cycle up through character set at this cursor position
            m->char_col--;
            if(m->char_col < 0) {
                m->char_row = (m->char_row + CHARSET_ROWS - 1) % CHARSET_ROWS;
                m->char_col = charset_row_len(m->char_row) - 1;
            }
            // Insert/overwrite char at cursor
            if(m->cursor < (int)m->text_len) {
                m->text[m->cursor] = CHARSET[m->char_row][m->char_col];
            }
            consumed = true;
            break;

        case InputKeyDown:
            m->char_col++;
            if(m->char_col >= row_len) {
                m->char_row = (m->char_row + 1) % CHARSET_ROWS;
                m->char_col = 0;
            }
            if(m->cursor < (int)m->text_len) {
                m->text[m->cursor] = CHARSET[m->char_row][m->char_col];
            }
            consumed = true;
            break;

        case InputKeyRight:
            // Append current char if at end, then advance cursor
            if(m->cursor >= (int)m->text_len && m->text_len < FC_MAX_INPUT_LEN) {
                m->text[m->text_len] = CHARSET[m->char_row][m->char_col];
                m->text_len++;
                m->text[m->text_len] = '\0';
            }
            if(m->cursor < (int)m->text_len) m->cursor++;
            // Reset char picker to start of row 1 (lowercase)
            m->char_row = 1; m->char_col = 0;
            consumed = true;
            break;

        case InputKeyLeft:
            if(m->cursor > 0) {
                m->cursor--;
                // Sync picker to character at new cursor position
                char c = m->text[m->cursor];
                for(int r = 0; r < CHARSET_ROWS; r++) {
                    const char* p = strchr(CHARSET[r], c);
                    if(p) { m->char_row = r; m->char_col = (int)(p - CHARSET[r]); break; }
                }
            }
            consumed = true;
            break;

        default:
            break;
        }
    }, consumed);

    if(consumed) return true;

    // OK — send prompt
    if(event->key == InputKeyOk && event->type == InputTypeShort) {
        char prompt[FC_MAX_INPUT_LEN + 1];
        ui_input_get_text(app->view_input, prompt, sizeof(prompt));

        if(strlen(prompt) > 0) {
            // Update chat preview
            ui_chat_set_prompt_preview(app->view_chat, prompt);
            ui_chat_clear(app->view_chat);
            ui_chat_set_waiting(app->view_chat, true);
            app->waiting_response = true;

            // Send over UART
            uart_send_prompt(app, prompt);
            ui_input_clear(app->view_input);
        }

        view_dispatcher_switch_to_view(app->view_dispatcher, ViewIdChat);
        return true;
    }

    // BACK — cancel
    if(event->key == InputKeyBack) {
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewIdChat);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------

View* ui_input_alloc(FlipperClawApp* app) {
    View* view = view_alloc();
    view_set_context(view, app);
    view_allocate_model(view, ViewModelTypeLocking, sizeof(InputModel));
    view_set_draw_callback(view, input_draw);
    view_set_input_callback(view, input_handler);

    with_view_model(view, InputModel*, m, {
        memset(m, 0, sizeof(InputModel));
        m->char_row = 1; // start on lowercase
        m->char_col = 0;
    }, false);

    return view;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ui_input_clear(View* view) {
    furi_assert(view);
    with_view_model(view, InputModel*, m, {
        memset(m->text, 0, sizeof(m->text));
        m->text_len = 0;
        m->cursor   = 0;
        m->char_row = 1;
        m->char_col = 0;
    }, true);
}

void ui_input_get_text(View* view, char* buf, size_t buf_size) {
    furi_assert(view);
    furi_assert(buf);
    with_view_model(view, InputModel*, m, {
        size_t copy = m->text_len < buf_size - 1 ? m->text_len : buf_size - 1;
        memcpy(buf, m->text, copy);
        buf[copy] = '\0';
    }, false);
}
