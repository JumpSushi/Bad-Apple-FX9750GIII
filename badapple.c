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

static unsigned char frame_buffer[CONTENT_FRAME_SIZE];  // Inverted frame bytes (ready for VRAM)
static unsigned char prev_frame[CONTENT_FRAME_SIZE];
static bool have_prev = false;
static int video_file = -1;
static volatile int frame_flag = 0; // set by timer callback each frame
// Border color state (0 = black, 1 = white, -1 = unset)
static int left_border_white = 1;  // default white
static int right_border_white = 1; // default white

// Frame format:
//   header: bit0=left white, bit1=right white, bit2=keyframe (1=keyframe, 0=delta)
//   RLE-encoded stream of either full inverted frame (keyframe) or XOR delta vs previous frame
//   CRC16-CCITT (poly 0x1021, seed 0xFFFF) over the full reconstructed inverted frame (little-endian stored)

// Helper to read a single character from a file
int get_char() {
    unsigned char c;
    if (BFile_Read(video_file, &c, 1, -1) == 1) {
        return c;
    }
    return -1; // Error or EOF
}

// Decompress a frame: first header byte for borders, then RLE stream
// CRC16-CCITT
static unsigned short crc16_ccitt(const unsigned char *data, int len, unsigned short seed)
{
    unsigned short crc = seed;
    for(int i=0;i<len;i++) {
        crc ^= (unsigned short)data[i] << 8;
        for(int b=0;b<8;b++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    }
    return crc;
}

int decompress_frame() {
    int header = get_char();
    if(header < 0) return 0;
    left_border_white = (header & 0x01) ? 1 : 0;
    right_border_white = (header & 0x02) ? 1 : 0;
    bool keyframe = (header & 0x04) != 0;

    // Decode RLE into frame_buffer (will contain either full frame or delta)
    int out_index = 0;
    while(out_index < CONTENT_FRAME_SIZE) {
        int count_char = get_char(); if(count_char < 0) return 0;
        int value_char = get_char(); if(value_char < 0) return 0;
        unsigned char count = (unsigned char)count_char;
        unsigned char value = (unsigned char)value_char;
        for(int i=0;i<count;i++) {
            if(out_index >= CONTENT_FRAME_SIZE) return 0; // malformed
            frame_buffer[out_index++] = value;
        }
    }
    // Apply XOR delta if not keyframe
    if(!keyframe) {
        if(!have_prev) return 0; // corrupt stream
        for(int i=0;i<CONTENT_FRAME_SIZE;i++) frame_buffer[i] ^= prev_frame[i];
    }
    // Read CRC16
    int crc_lo = get_char(); if(crc_lo < 0) return 0;
    int crc_hi = get_char(); if(crc_hi < 0) return 0;
    unsigned short stored_crc = (unsigned short)(crc_lo | (crc_hi<<8));
    unsigned short calc_crc = crc16_ccitt(frame_buffer, CONTENT_FRAME_SIZE, 0xFFFF);
    if(calc_crc != stored_crc) {
        return 0; // CRC mismatch signals end/error
    }
    memcpy(prev_frame, frame_buffer, CONTENT_FRAME_SIZE);
    have_prev = true;
    return 1;
}

// Draw frame: use existing border_state; invert bytes when copying; center correctly
void draw_frame() {
    extern uint32_t *gint_vram; // 1024 bytes (128x64/8)
    uint8_t *vram = (uint8_t *)gint_vram;
    uint8_t left_byte = left_border_white ? 0xFF : 0x00;
    uint8_t right_byte = right_border_white ? 0xFF : 0x00;
    for(int y=0; y<HEIGHT; y++) {
        uint8_t *row = vram + y * ROW_BYTES;
        // Left border
    for(int b=0;b<LEFT_BORDER_BYTES;b++) row[b] = left_byte;
    // Content already inverted in file (1 bits = white)
        uint8_t const *src = frame_buffer + y*CONTENT_BYTES;
        uint8_t *dst = row + LEFT_BORDER_BYTES;
    for(int b=0;b<CONTENT_BYTES;b++) dst[b] = src[b];
        // Right border
    for(int b=0;b<RIGHT_BORDER_BYTES;b++) row[LEFT_BORDER_BYTES + CONTENT_BYTES + b] = right_byte;
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
