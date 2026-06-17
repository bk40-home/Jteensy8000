/* Audio Library for Teensy
 * Copyright (c) 2025, Kris Bishop, bishopkris40@hotmail.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
 /*
 * Singleton class designed to share SPI status between
 * multiple TFT display instances.
 * 
 * There needs to be a copy of this header in every display
 * driver library, but they MUST be identical
 */
#if !defined(_DISPLAY_SHARED_SPI_STATUS_H_)
#define _DISPLAY_SHARED_SPI_STATUS_H_

class DisplaySharedSPIStatus 
{
    static const int NUM_SPI = 3;
    DisplaySharedSPIStatus() {} // Constructor is private
  public:
    static DisplaySharedSPIStatus& getInstance()
    {
        static DisplaySharedSPIStatus instance; // Guaranteed to be destroyed.
                              // Instantiated on first use.
        return instance;
    }

    // Delete the methods we don't want.
    DisplaySharedSPIStatus(DisplaySharedSPIStatus const&) = delete;
    void operator=(DisplaySharedSPIStatus const&)  = delete;

    // SPI port status to be shared across instances
    struct status_s 
    {
        bool _begin_done = false;
        volatile uint8_t _dma_state = 0;
        uint8_t _pending_rx_count = 0;
        uint32_t _spi_tcr_current = 0; 
        DMAChannel DMAch{false};
    } status[NUM_SPI];
    status_s& operator[](uint8_t idx) 
        { return status[idx]; }
};
#endif // !defined(_DISPLAY_SHARED_SPI_STATUS_H_)

