#include <gint/display.h>
#include <gint/bfile.h>
#include <gint/timer.h>
#include <gint/keyboard.h>
#include <gint/defs/call.h>
#include <string.h>

#define WIDTH 128
#define HEIGHT 64
#define FRAME_SIZE (WIDTH * HEIGHT / 8)
#define FRAME_DELAY_MS 66 // For ~15 fps

static unsigned char frame_buffer[FRAME_SIZE];
static int video_file = -1;
static volatile int frame_flag = 0; // set by timer callback each frame

// Helper to read a single character from a file
int get_char() {
    unsigned char c;
    if (BFile_Read(video_file, &c, 1, -1) == 1) {
        return c;
    }
    return -1; // Error or EOF
}

// Decompresses one frame of RLE data from the file into frame_buffer
int decompress_frame() {
    int out_index = 0;
    while (out_index < FRAME_SIZE) {
        int count_char = get_char();
        if (count_char < 0) return 0; // End of file or error
        int value_char = get_char();
        if (value_char < 0) return 0; // End of file or error

        unsigned char count = (unsigned char)count_char;
        unsigned char value = (unsigned char)value_char;

        for (int i = 0; i < count; i++) {
            if (out_index < FRAME_SIZE) {
                frame_buffer[out_index++] = value;
            } else {
                // Frame data is larger than buffer, indicates corruption or error
                return 0;
            }
        }
    }
    return 1; // Success
}

// Draws the current frame_buffer to the VRAM
void draw_frame() {
    // gint_vram is a uint32_t*; treat it as byte buffer
    extern uint32_t *gint_vram;
    memcpy((void *)gint_vram, frame_buffer, FRAME_SIZE);
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

    // Set up a repeating timer that just sets frame_flag every FRAME_DELAY_MS
    int timer_id = timer_configure(TIMER_ANY, FRAME_DELAY_MS * 1000, GINT_CALL_SET(&frame_flag));
    if(timer_id >= 0) timer_start(timer_id);

    while (1) {
        if (decompress_frame()) {
            draw_frame();
        } else {
            // End of video, loop or exit
            BFile_Seek(video_file, 0); // Loop by seeking to start
            continue;
        }

        // Wait for timer flag; poll keyboard events to allow early exit
        while(!frame_flag) {
            key_event_t ev = pollevent();
            if(ev.type == KEYEV_DOWN) {
                // Any key exits
                frame_flag = 0;
                goto end;
            }
        }
        frame_flag = 0; // consume flag
    }

end:
    // Cleanup
    BFile_Close(video_file);
    if(timer_id >= 0) timer_stop(timer_id);

    return 0;
}
