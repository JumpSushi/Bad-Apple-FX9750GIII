#include <gint/display.h>
#include <gint/bfile.h>
#include <gint/timer.h>
#include <gint/keyboard.h>
#include <gint/defs/call.h>
#include <gint/keycodes.h>
#include <string.h>
#include <stdbool.h>

// Visible screen is 128x64. Content uses original Bad Apple 5:4 aspect -> 80x64.
// We center it, leaving 24-pixel (3-byte) side borders that adapt per frame.
#define CONTENT_WIDTH 80
#define HEIGHT 64
#define CONTENT_FRAME_SIZE (CONTENT_WIDTH * HEIGHT / 8) // 640 bytes
#define FRAME_DELAY_MS 66 // ~15 fps

#define ROW_BYTES (128/8)                 // 16 bytes per row in VRAM
#define TOTAL_BORDER_BYTES ((128-CONTENT_WIDTH)/8) // Total bytes not used by content (48px -> 6 bytes)
#define LEFT_BORDER_BYTES (TOTAL_BORDER_BYTES/2)   // 3 bytes (24 px)
#define RIGHT_BORDER_BYTES (TOTAL_BORDER_BYTES - LEFT_BORDER_BYTES) // 3 bytes
#define CONTENT_BYTES (CONTENT_WIDTH/8)     // 10 bytes

static unsigned char frame_buffer[CONTENT_FRAME_SIZE];  // Raw (non-inverted) content bytes
static int video_file = -1;
static volatile int frame_flag = 0; // set by timer callback each frame
// Border color state (0 = black, 1 = white, -1 = unset)
static int border_state = -1;
static int to_white_streak = 0;
static int to_black_streak = 0;
static int white_bits_last = 0; // white bit count after inversion for last frame

// Tunable hysteresis parameters
#define WHITE_THRESHOLD 0.58f   // need >= 58% white bits (post inversion) to lean white
#define BLACK_THRESHOLD 0.42f   // need <= 42% white bits to lean black
#define HYSTERESIS_FRAMES 4      // consecutive frames required before switching

// Helper to read a single character from a file
int get_char() {
    unsigned char c;
    if (BFile_Read(video_file, &c, 1, -1) == 1) {
        return c;
    }
    return -1; // Error or EOF
}

// Popcount lookup (8-bit)
static unsigned char const popcount8[256] = {
    #define B2(n) n, n+1, n+1, n+2
    #define B4(n) B2(n), B2(n+1), B2(n+1), B2(n+2)
    #define B6(n) B4(n), B4(n+1), B4(n+1), B4(n+2)
    B6(0), B6(1), B6(1), B6(2)
    #undef B2
    #undef B4
    #undef B6
};

// Decompress a frame and compute white bit ratio *after inversion* on the fly
int decompress_frame() {
    int out_index = 0;
    int white_bits = 0; // counts bits of ~byte => 8 - popcount(original)
    while (out_index < CONTENT_FRAME_SIZE) {
        int count_char = get_char();
        if (count_char < 0) goto eof;
        int value_char = get_char();
        if (value_char < 0) goto eof;
        unsigned char count = (unsigned char)count_char;
        unsigned char value = (unsigned char)value_char;
        unsigned char inv_bits = 8 - popcount8[value];
        // Fill output and accumulate whiteness
        for(int i=0;i<count;i++) {
            if(out_index >= CONTENT_FRAME_SIZE) goto corrupt;
            frame_buffer[out_index++] = value;
            white_bits += inv_bits;
        }
    }
    // Apply hysteresis decision using white_bits/total_bits
    white_bits_last = white_bits;
    {
        int total_bits = CONTENT_WIDTH * HEIGHT; // 5120
        float white_ratio = (float)white_bits_last / (float)total_bits;
        if(border_state < 0) {
            border_state = (white_ratio >= 0.5f) ? 1 : 0;
        } else {
            if(border_state == 0) { // currently black
                if(white_ratio >= WHITE_THRESHOLD) {
                    to_white_streak++; to_black_streak = 0; if(to_white_streak >= HYSTERESIS_FRAMES){ border_state=1; to_white_streak=0; }
                } else if(white_ratio <= BLACK_THRESHOLD) { to_black_streak++; to_white_streak=0; }
                else { to_white_streak=to_black_streak=0; }
            } else { // currently white
                if(white_ratio <= BLACK_THRESHOLD) {
                    to_black_streak++; to_white_streak=0; if(to_black_streak >= HYSTERESIS_FRAMES){ border_state=0; to_black_streak=0; }
                } else if(white_ratio >= WHITE_THRESHOLD) { to_white_streak++; to_black_streak=0; }
                else { to_white_streak=to_black_streak=0; }
            }
        }
    }
    return 1;

eof:
    return 0;
corrupt:
    return 0;
}

// Draw frame: use existing border_state; invert bytes when copying; center correctly
void draw_frame() {
    extern uint32_t *gint_vram; // 1024 bytes (128x64/8)
    uint8_t *vram = (uint8_t *)gint_vram;
    uint8_t border_byte = (border_state == 1) ? 0xFF : 0x00;
    for(int y=0; y<HEIGHT; y++) {
        uint8_t *row = vram + y * ROW_BYTES;
        // Left border
        for(int b=0;b<LEFT_BORDER_BYTES;b++) row[b] = border_byte;
        // Content (invert while copying)
        uint8_t const *src = frame_buffer + y*CONTENT_BYTES;
        uint8_t *dst = row + LEFT_BORDER_BYTES;
        for(int b=0;b<CONTENT_BYTES;b++) dst[b] = (uint8_t)~src[b];
        // Right border
        for(int b=0;b<RIGHT_BORDER_BYTES;b++) row[LEFT_BORDER_BYTES + CONTENT_BYTES + b] = border_byte;
    }
    dupdate();
}

int main(void) {
    // Open the video file
    unsigned short path[] = {'\\', '\\', 'f', 'l', 's', '0', '\\', 'b', 'a', 'd', 'a', 'p', 'p', 'l', 'e', '.', 'b', 'i', 'n', 0};
    video_file = BFile_Open(path, BFile_ReadOnly);

    if (video_file < 0) {
        dclear(C_WHITE);
        dprint(5, 5, C_BLACK, "Error: badapple.bin not found.");
        dprint(5, 15, C_BLACK, "Copy it to fls0.");
        dupdate();
        getkey();
        return 1;
    }

    // Playback state
    bool loop_enabled = true;
    bool paused = false;

    // Set up a repeating timer that just sets frame_flag every FRAME_DELAY_MS
    int timer_id = timer_configure(TIMER_ANY, FRAME_DELAY_MS * 1000, GINT_CALL_SET(&frame_flag));
    if(timer_id >= 0) timer_start(timer_id);

    while (1) {
        if(!paused) {
            if (decompress_frame()) {
                draw_frame();
            } else {
                // End of video
                if(loop_enabled) {
                    BFile_Seek(video_file, 0);
                    continue;
                } else {
                    break;
                }
            }
        }

        // Wait for next frame interval (or remain paused) with event handling
        while(true) {
            key_event_t ev = pollevent();
            if(ev.type == KEYEV_DOWN) {
                if(ev.key == KEY_F1) {
                    paused = !paused;
                    if(paused) {
                        // Indicate pause (simple overlay)
                        dprint(2,2,C_BLACK,"PAUSE");
                        dupdate();
                    }
                } else if(ev.key == KEY_F2) {
                    loop_enabled = !loop_enabled;
                } else if(ev.key == KEY_EXIT) {
                    goto end;
                }
            }
            if(paused) continue; // Stay inside paused loop until unpaused
            if(frame_flag) { frame_flag = 0; break; }
        }
    }

end:
    // Cleanup
    BFile_Close(video_file);
    if(timer_id >= 0) timer_stop(timer_id);

    return 0;
}
