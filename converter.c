#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Define the screen dimensions for the calculator
#define WIDTH 128
#define HEIGHT 64

// Function to apply Run-Length Encoding
void rle_compress(FILE *in, FILE *out) {
    unsigned char buffer[WIDTH * HEIGHT / 8];
    int frame_count = 0;

    while (fread(buffer, 1, sizeof(buffer), in) == sizeof(buffer)) {
        frame_count++;
        unsigned char *data = buffer;
        int size = sizeof(buffer);
        
        for (int i = 0; i < size; ) {
            unsigned char current_byte = data[i];
            int count = 1;
            while (i + count < size && data[i + count] == current_byte && count < 255) {
                count++;
            }
            fputc(count, out);
            fputc(current_byte, out);
            i += count;
        }
    }
    printf("Processed %d frames.\n", frame_count);
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
