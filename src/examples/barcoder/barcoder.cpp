#include "barcoder.hh"

void Barcoder::barcodeFrameUpper(RGBPixel *frame_bytes, uint64_t frameno) {
    for (int i = 0; i < m_total_bits; i++) 
        writeBit(frame_bytes, 
                 (i % m_rowBits) * m_code_size, 
                 (i / m_rowBits) * m_code_size, 
                 frameno & (1 << i));
}
void Barcoder::barcodeFrameLower(RGBPixel *frame_bytes, uint64_t frameno) {
    for (int i = 0; i < m_total_bits; i++) 
        writeBit(frame_bytes, 
                 (m_width - (m_code_size * m_rowBits)) + (i % m_rowBits) * m_code_size, 
                 (m_height - (m_code_size * m_rowBits)) + (i / m_rowBits) * m_code_size, 
                 frameno & (1 << i));
}

void Barcoder::barcodeFrame(RGBPixel *frame_bytes) {
    barcodeFrameUpper(frame_bytes, m_next_frameno);
    barcodeFrameLower(frame_bytes, m_next_frameno);
    m_next_frameno++;
}

void Barcoder::writeBit(RGBPixel* frame_bytes, int x, int y, bool set) {
    for (int i = y; i < y + m_code_size; i++) 
        for (int j = x; j < x + m_code_size; j++)
            frame_bytes[i * m_width + j] = set ? Black : White;

}

int main() {
    std::ifstream infile("input-video.raw", std::ios::in|std::ios::binary);
    std::ofstream outfile("barcoded-video.raw", std::ios::out|std::ios::binary);
    Barcoder br(infile);
    outfile << br;
    infile.close();
    outfile.close();
    return 0;
}
