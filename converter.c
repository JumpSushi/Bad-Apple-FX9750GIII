#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// Content dimensions (5:4) within 128x64 screen; borders added at playback
#define WIDTH 80
#define HEIGHT 64
#define FRAME_BYTES (WIDTH * HEIGHT / 8)

// Border classification with lookahead stability: if a per-frame column majority
// differs from current state, we only adopt it if it persists across the next
// LOOKAHEAD frames (or until video end if fewer remain). Change takes effect at
// the first differing frame (no visual latency) if stable.
#define LOOKAHEAD 15

// CRC16-CCITT (poly 0x1021) implementation
static unsigned short crc16_ccitt(const unsigned char *data, int len, unsigned short seed)
{
    unsigned short crc = seed; // Often 0xFFFF; we'll use 0xFFFF
    for(int i=0;i<len;i++) {
        crc ^= (unsigned short)data[i] << 8;
        for(int b=0;b<8;b++) {
            if(crc & 0x8000) crc = (crc << 1) ^ 0x1021; else crc <<= 1;
        }
    }
    return crc;
}

static int popcount8(unsigned char x) {
    // builtin OK on host
    return __builtin_popcount((unsigned)x);
}

// Compute per-frame column majorities (without stabilization)
static void compute_majorities(const unsigned char *frames_inv, int frame_count,
                               unsigned char *left_majority, unsigned char *right_majority)
{
    int bpr = WIDTH/8; // 10
    for(int f=0; f<frame_count; f++) {
        const unsigned char *frame = frames_inv + f*FRAME_BYTES;
        int left_white=0, right_white=0;
        for(int y=0;y<HEIGHT;y++) {
            const unsigned char *row = frame + y*bpr;
            unsigned char left_byte = row[0];
            unsigned char right_byte = row[bpr-1];
            if(left_byte & 0x80) left_white++;
            if(right_byte & 0x01) right_white++;
        }
        left_majority[f] = (left_white > HEIGHT/2) ? 1 : 0;
        right_majority[f] = (right_white > HEIGHT/2) ? 1 : 0;
    }
}

// Stabilize with lookahead rule described above
static void stabilize_majorities(int frame_count, const unsigned char *maj_in,
                                 unsigned char *state_out, int lookahead)
{
    int current = maj_in[0];
    state_out[0] = current;
    for(int f=1; f<frame_count; f++) {
        int m = maj_in[f];
        if(m != current) {
            int stable = 1;
            for(int k=1; k<lookahead && (f+k) < frame_count; k++) {
                if(maj_in[f+k] != m) { stable = 0; break; }
            }
            // Require stability across available frames (either full lookahead or until end)
            if(stable) current = m;
        }
        state_out[f] = current;
    }
}

// Function to apply Run-Length Encoding with delta & stabilized borders
void rle_compress(FILE *in, FILE *out) {
    // Load entire input into memory
    fseek(in, 0, SEEK_END);
    long fsize = ftell(in);
    if(fsize < 0 || fsize % FRAME_BYTES != 0) {
        fprintf(stderr, "Input size not a multiple of frame size.\n");
        return;
    }
    int frame_count = (int)(fsize / FRAME_BYTES);
    rewind(in);
    unsigned char *raw_all = malloc(fsize);
    if(!raw_all) { fprintf(stderr, "malloc failed raw_all\n"); return; }
    if(fread(raw_all, 1, fsize, in) != (size_t)fsize) { fprintf(stderr, "Failed to read all frames\n"); free(raw_all); return; }

    // Inverted frames
    unsigned char *inv_all = malloc(fsize);
    if(!inv_all) { fprintf(stderr, "malloc failed inv_all\n"); free(raw_all); return; }
    for(long i=0;i<fsize;i++) inv_all[i] = (unsigned char)~raw_all[i];

    // Majority arrays and stabilized state
    unsigned char *left_maj = malloc(frame_count);
    unsigned char *right_maj = malloc(frame_count);
    unsigned char *left_state = malloc(frame_count);
    unsigned char *right_state = malloc(frame_count);
    if(!left_maj||!right_maj||!left_state||!right_state){ fprintf(stderr,"malloc failed majority arrays\n"); goto cleanup; }

    compute_majorities(inv_all, frame_count, left_maj, right_maj);
    stabilize_majorities(frame_count, left_maj, left_state, LOOKAHEAD);
    stabilize_majorities(frame_count, right_maj, right_state, LOOKAHEAD);

    unsigned char *prev = malloc(FRAME_BYTES);
    if(!prev) { fprintf(stderr, "malloc failed prev\n"); goto cleanup; }
    bool have_prev=false;
    unsigned char *delta = malloc(FRAME_BYTES);
    if(!delta) { fprintf(stderr, "malloc failed delta\n"); goto cleanup; }

    for(int f=0; f<frame_count; f++) {
        const unsigned char *inv = inv_all + f*FRAME_BYTES;
        unsigned char header = 0;
        if(left_state[f]) header |= 0x01;
        if(right_state[f]) header |= 0x02;

        bool keyframe = !have_prev;
        int changed=0;
        if(have_prev) {
            for(int i=0;i<FRAME_BYTES;i++) {
                unsigned char d = (unsigned char)(inv[i] ^ prev[i]);
                delta[i] = d;
                if(d) changed++;
            }
            if(changed > (FRAME_BYTES * 3)/4) keyframe = true;
        }
        if(keyframe) header |= 0x04;
        fputc(header,out);

        const unsigned char *encode_buf = keyframe ? inv : delta;
        for(int i=0;i<FRAME_BYTES;) {
            unsigned char current = encode_buf[i];
            int count=1;
            while(i+count<FRAME_BYTES && encode_buf[i+count]==current && count<255) count++;
            fputc(count,out);
            fputc(current,out);
            i+=count;
        }
        unsigned short crc = crc16_ccitt(inv, FRAME_BYTES, 0xFFFF);
        fputc(crc & 0xFF, out);
        fputc((crc >> 8) & 0xFF, out);

        memcpy(prev, inv, FRAME_BYTES);
        have_prev = true;
    }
    printf("Processed %d frames.\n", frame_count);

cleanup:
    if(raw_all) free(raw_all);
    if(inv_all) free(inv_all);
    if(left_maj) free(left_maj);
    if(right_maj) free(right_maj);
    if(left_state) free(left_state);
    if(right_state) free(right_state);
    // prev, delta freed implicitly at process end or free here
    ;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input_raw_file> <output_bin_file>\n", argv[0]);
        return 1;
    }

    char *input_filename = argv[1];
    char *output_filename = argv[2];

    FILE *in = fopen(input_filename, "rb");
    if (!in) {
        perror("Failed to open input file");
        return 1;
    }

    FILE *out = fopen(output_filename, "wb");
    if (!out) {
        perror("Failed to open output file");
        fclose(in);
        return 1;
    }

    rle_compress(in, out);

    fclose(in);
    fclose(out);

    printf("Compression complete: %s -> %s\n", input_filename, output_filename);

    return 0;
}
