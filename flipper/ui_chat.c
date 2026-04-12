/**
 * @file ui_chat.c
 * @brief Chat view — streaming response display with word-wrap and scrolling.
 */

#include "ui_chat.h"
#include <gui/canvas.h>
#include <gui/elements.h>
#include <input/input.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define TAG "ui_chat"

// ---------------------------------------------------------------------------
// View model
// ---------------------------------------------------------------------------

typedef struct {
    char     response[FC_MAX_RESPONSE_LEN]; ///< Full accumulated response text
    size_t   response_len;
    char     prompt_preview[FC_DISPLAY_LINE_CHARS + 1];
    int      scroll_offset;    ///< Lines scrolled from bottom (0 = show end)
    bool     waiting;          ///< Show "thinking..." animation
    bool     uart_ok;          ///< Connection indicator
    char     status_msg[64];   ///< Current status text
    uint32_t tick;             ///< For dot animation
} ChatModel;

// ---------------------------------------------------------------------------
// Word-wrap helper — count total wrapped lines in text
// ---------------------------------------------------------------------------

/** Fill wrapped_lines[max_lines] with pointers into text (line start positions).
 *  Returns total number of wrapped lines. */
static int wrap_text(const char* text, size_t text_len,
                     int line_chars,
                     int* line_starts,  // byte offsets into text
                     int* line_lengths, // byte lengths of each line
                     int max_lines) {
    int total = 0;
    int pos = 0;
    int len = (int)text_len;

    while(pos < len && total < max_lines) {
        int remaining = len - pos;
        if(remaining <= 0) break;

        // Find end of this line
        int line_end = pos + line_chars;
        if(line_end >= len) {
            // Last fragment — take everything
            if(line_starts)  line_starts[total]  = pos;
            if(line_lengths) line_lengths[total]  = remaining;
            total++;
            break;
        }

        // Try to break at a space before line_end
        int break_at = -1;
        for(int i = line_end; i > pos; i--) {
            if(text[i] == ' ' || text[i] == '\n') {
                break_at = i;
                break;
            }
        }

        int line_len;
        int next_pos;
        if(break_at > pos) {
            line_len = break_at - pos;
            next_pos = break_at + 1; // skip the space
        } else {
            // No space found — hard break at line_chars
            line_len = line_chars;
            next_pos = pos + line_chars;
        }

        // Also break at embedded newline
        for(int i = pos; i < pos + line_len; i++) {
            if(text[i] == '\n') {
                line_len = i - pos;
                next_pos = i + 1;
                break;
            }
        }

        if(line_starts)  line_starts[total]  = pos;
        if(line_lengths) line_lengths[total]  = line_len;
        total++;
        pos = next_pos;
    }
    return total;
}

// ---------------------------------------------------------------------------
// Draw callback
// ---------------------------------------------------------------------------

static void chat_draw(Canvas* canvas, void* model_ptr) {
    ChatModel* m = (ChatModel*)model_ptr;

    canvas_clear(canvas);

    // --- Header (y=0..10) ---
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "FlipperClaw");

    // Connection dot: filled=connected, empty=disconnected (top-right)
    if(m->uart_ok) {
        canvas_draw_disc(canvas, 124, 5, 3);
    } else {
        canvas_draw_circle(canvas, 124, 5, 3);
    }

    // --- Divider (y=11) ---
    canvas_draw_line(canvas, 0, 11, 127, 11);

    // --- Prompt preview (y=13..20) ---
    canvas_set_font(canvas, FontSecondary);
    if(m->prompt_preview[0]) {
        canvas_draw_str(canvas, 0, 20, m->prompt_preview);
    }

    // --- Response area (y=22..54, 4 visible lines) ---
    // Build wrap table
    static int   line_starts[256];
    static int   line_lengths[256];
    int total_lines = wrap_text(m->response, m->response_len,
                                FC_DISPLAY_LINE_CHARS,
                                line_starts, line_lengths, 256);

    // Compute first visible line (from scroll_offset, clamped)
    int max_scroll = total_lines - FC_DISPLAY_VISIBLE_LINES;
    if(max_scroll < 0) max_scroll = 0;
    int scroll = m->scroll_offset;
    if(scroll > max_scroll) scroll = max_scroll;
    int first_line = total_lines - FC_DISPLAY_VISIBLE_LINES - scroll;
    if(first_line < 0) first_line = 0;

    for(int i = 0; i < FC_DISPLAY_VISIBLE_LINES; i++) {
        int li = first_line + i;
        if(li >= total_lines) break;

        char line_buf[FC_DISPLAY_LINE_CHARS + 2];
        int copy_len = line_lengths[li];
        if(copy_len > FC_DISPLAY_LINE_CHARS) copy_len = FC_DISPLAY_LINE_CHARS;
        memcpy(line_buf, m->response + line_starts[li], copy_len);
        line_buf[copy_len] = '\0';

        canvas_draw_str(canvas, 0, 22 + i * 8, line_buf);
    }

    // --- Footer (y=56..63) ---
    canvas_draw_line(canvas, 0, 55, 127, 55);
    if(m->waiting) {
        // Animated "thinking..." — cycle 1/2/3 dots using tick
        uint8_t dots = (m->tick / 4) % 3 + 1;
        char anim[16];
        snprintf(anim, sizeof(anim), "thinking%.*s", dots, "...");
        canvas_draw_str(canvas, 0, 63, anim);
    } else if(m->status_msg[0]) {
        canvas_draw_str(canvas, 0, 63, m->status_msg);
    } else {
        canvas_draw_str(canvas, 0, 63, "\x18\x19 OK=new");  // ↑↓ symbols
    }
}

// ---------------------------------------------------------------------------
// Input callback — scroll and navigation
// ---------------------------------------------------------------------------

static bool chat_input(InputEvent* event, void* ctx) {
    FlipperClawApp* app = (FlipperClawApp*)ctx;
    furi_assert(app);

    if(event->type != InputTypeShort && event->type != InputTypeLong) {
        return false;
    }

    if(event->key == InputKeyUp) {
        with_view_model(
            app->view_chat, ChatModel*, model,
            { model->scroll_offset++; },
            true
        );
        return true;
    }

    if(event->key == InputKeyDown) {
        with_view_model(
            app->view_chat, ChatModel*, model,
            { if(model->scroll_offset > 0) model->scroll_offset--; },
            true
        );
        return true;
    }

    if(event->key == InputKeyOk && event->type == InputTypeShort) {
        // Switch to input view
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewIdInput);
        return true;
    }

    if(event->key == InputKeyOk && event->type == InputTypeLong) {
        // Long-press OK → status view
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewIdStatus);
        return true;
    }

    if(event->key == InputKeyBack) {
        // Exit the app
        view_dispatcher_stop(app->view_dispatcher);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Tick callback — drives "thinking..." animation
// ---------------------------------------------------------------------------

static void chat_tick(void* ctx) {
    FlipperClawApp* app = (FlipperClawApp*)ctx;
    with_view_model(
        app->view_chat, ChatModel*, model,
        { model->tick++; },
        model->waiting   // only redraw if waiting (animation active)
    );
}

// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------

View* ui_chat_alloc(FlipperClawApp* app) {
    View* view = view_alloc();
    view_set_context(view, app);
    view_allocate_model(view, ViewModelTypeLocking, sizeof(ChatModel));
    view_set_draw_callback(view, chat_draw);
    view_set_input_callback(view, chat_input);
    view_set_tick_callback(view, chat_tick, 250);  // 4 Hz tick for animation

    with_view_model(view, ChatModel*, m, {
        memset(m, 0, sizeof(ChatModel));
        m->uart_ok = false;
    }, false);

    return view;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ui_chat_append(View* view, const char* text) {
    furi_assert(view);
    furi_assert(text);
    size_t tlen = strlen(text);
    with_view_model(view, ChatModel*, m, {
        size_t space = FC_MAX_RESPONSE_LEN - 1 - m->response_len;
        if(space > 0) {
            size_t copy = tlen < space ? tlen : space;
            memcpy(m->response + m->response_len, text, copy);
            m->response_len += copy;
            m->response[m->response_len] = '\0';
        }
        m->scroll_offset = 0; // auto-scroll to end on new content
    }, true);
}

void ui_chat_set_done(View* view) {
    furi_assert(view);
    with_view_model(view, ChatModel*, m, {
        m->waiting = false;
        m->status_msg[0] = '\0';
    }, true);
}

void ui_chat_set_waiting(View* view, bool waiting) {
    furi_assert(view);
    with_view_model(view, ChatModel*, m, {
        m->waiting = waiting;
        if(waiting) strncpy(m->status_msg, "thinking...", sizeof(m->status_msg) - 1);
    }, true);
}

void ui_chat_set_uart_ok(View* view, bool connected) {
    furi_assert(view);
    with_view_model(view, ChatModel*, m, { m->uart_ok = connected; }, true);
}

void ui_chat_set_status(View* view, const char* text) {
    furi_assert(view);
    furi_assert(text);
    with_view_model(view, ChatModel*, m, {
        strncpy(m->status_msg, text, sizeof(m->status_msg) - 1);
        m->status_msg[sizeof(m->status_msg) - 1] = '\0';
    }, true);
}

void ui_chat_set_prompt_preview(View* view, const char* text) {
    furi_assert(view);
    furi_assert(text);
    with_view_model(view, ChatModel*, m, {
        strncpy(m->prompt_preview, text, FC_DISPLAY_LINE_CHARS);
        m->prompt_preview[FC_DISPLAY_LINE_CHARS] = '\0';
    }, true);
}

void ui_chat_clear(View* view) {
    furi_assert(view);
    with_view_model(view, ChatModel*, m, {
        memset(m->response, 0, sizeof(m->response));
        m->response_len  = 0;
        m->scroll_offset = 0;
        m->waiting       = false;
        m->status_msg[0] = '\0';
    }, true);
}
