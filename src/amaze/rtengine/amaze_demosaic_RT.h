#pragma once

#define NOMINMAX
#ifndef _SSIZE_T_DEFINED_
#define _SSIZE_T_DEFINED_
/* Traditionally, bionic's ssize_t was "long int". This caused GCC to emit warnings when you
 * pass a ssize_t to a printf-style function. The correct type is __kernel_ssize_t, which is
 * "int", which isn't an ABI change for C code (because they're the same size) but is an ABI
 * change for C++ because "int" and "long int" mangle to "i" and "l" respectively. So until
 * we can fix the ABI, this change should not be propagated to the NDK. http://b/8253769. */
typedef int ssize_t;
#endif

#ifdef __AVX2__
//AVX2
#define ENABLE_AVX
#elif defined ( __AVX__ )
//AVX
#define ENABLE_AVX
#define __AVX__
#elif (defined(_M_AMD64) || defined(_M_X64))
//SSE2 x64
#define __SSE2__
#elif _M_IX86_FP == 2
//SSE2 x32
#define __SSE2__
#elif _M_IX86_FP == 1
//SSE x32
#else
//nothing
#endif

#include "array2D.h"
#include "LUT.h"

namespace
{
    unsigned fc(const unsigned int cfa[2][2], int r, int c) {
        return cfa[r & 1][c & 1];
    }
}

namespace rtengine
{

    class RawImage {
    public:
        unsigned  filters;
        unsigned FC(unsigned row, unsigned col) const
        {
            return (filters >> ((((row) << 1 & 14) + ((col) & 1)) << 1) & 3);
        }
    };

    class RawImageSource {
    public:
        //void ahd_demosaic();
        void amaze_demosaic_RT(int winx, int winy, int winw, int winh, const array2D<float>& rawData, array2D<float>& red, array2D<float>& green, array2D<float>& blue, size_t chunkSize = 1, bool measure = false);//Emil's code for AMaZE
        double initialGain = 1;
        int W, H;
        //ImageMatrices imatrices;
        void RawImageSource::border_interpolate(int winw, int winh, int lborders, const array2D<float>& rawData, array2D<float>& red, array2D<float>& green, array2D<float>& blue)
        {
            int bord = lborders;
            int width = winw;
            int height = winh;

            for (int i = 0; i < height; i++) {

                float sum[6];

                for (int j = 0; j < bord; j++) { //first few columns
                    for (int c = 0; c < 6; c++) {
                        sum[c] = 0;
                    }

                    for (int i1 = i - 1; i1 < i + 2; i1++)
                        for (int j1 = j - 1; j1 < j + 2; j1++) {
                            if ((i1 > -1) && (i1 < height) && (j1 > -1)) {
                                int c = FC(i1, j1);
                                sum[c] += rawData[i1][j1];
                                sum[c + 3]++;
                            }
                        }

                    int c = FC(i, j);

                    if (c == 1) {
                        red[i][j] = sum[0] / sum[3];
                        green[i][j] = rawData[i][j];
                        blue[i][j] = sum[2] / sum[5];
                    }
                    else {
                        green[i][j] = sum[1] / sum[4];

                        if (c == 0) {
                            red[i][j] = rawData[i][j];
                            blue[i][j] = sum[2] / sum[5];
                        }
                        else {
                            red[i][j] = sum[0] / sum[3];
                            blue[i][j] = rawData[i][j];
                        }
                    }
                }//j

                for (int j = width - bord; j < width; j++) { //last few columns
                    for (int c = 0; c < 6; c++) {
                        sum[c] = 0;
                    }

                    for (int i1 = i - 1; i1 < i + 2; i1++)
                        for (int j1 = j - 1; j1 < j + 2; j1++) {
                            if ((i1 > -1) && (i1 < height) && (j1 < width)) {
                                int c = FC(i1, j1);
                                sum[c] += rawData[i1][j1];
                                sum[c + 3]++;
                            }
                        }

                    int c = FC(i, j);

                    if (c == 1) {
                        red[i][j] = sum[0] / sum[3];
                        green[i][j] = rawData[i][j];
                        blue[i][j] = sum[2] / sum[5];
                    }
                    else {
                        green[i][j] = sum[1] / sum[4];

                        if (c == 0) {
                            red[i][j] = rawData[i][j];
                            blue[i][j] = sum[2] / sum[5];
                        }
                        else {
                            red[i][j] = sum[0] / sum[3];
                            blue[i][j] = rawData[i][j];
                        }
                    }
                }//j
            }//i

            for (int i = 0; i < bord; i++) {

                float sum[6];

                for (int j = bord; j < width - bord; j++) { //first few rows
                    for (int c = 0; c < 6; c++) {
                        sum[c] = 0;
                    }

                    for (int i1 = i - 1; i1 < i + 2; i1++)
                        for (int j1 = j - 1; j1 < j + 2; j1++) {
                            if ((i1 > -1) && (i1 < height) && (j1 > -1)) {
                                int c = FC(i1, j1);
                                sum[c] += rawData[i1][j1];
                                sum[c + 3]++;
                            }
                        }

                    int c = FC(i, j);

                    if (c == 1) {
                        red[i][j] = sum[0] / sum[3];
                        green[i][j] = rawData[i][j];
                        blue[i][j] = sum[2] / sum[5];
                    }
                    else {
                        green[i][j] = sum[1] / sum[4];

                        if (c == 0) {
                            red[i][j] = rawData[i][j];
                            blue[i][j] = sum[2] / sum[5];
                        }
                        else {
                            red[i][j] = sum[0] / sum[3];
                            blue[i][j] = rawData[i][j];
                        }
                    }
                }//j
            }

            for (int i = height - bord; i < height; i++) {

                float sum[6];

                for (int j = bord; j < width - bord; j++) { //last few rows
                    for (int c = 0; c < 6; c++) {
                        sum[c] = 0;
                    }

                    for (int i1 = i - 1; i1 < i + 2; i1++)
                        for (int j1 = j - 1; j1 < j + 2; j1++) {
                            if ((i1 > -1) && (i1 < height) && (j1 < width)) {
                                int c = FC(i1, j1);
                                sum[c] += rawData[i1][j1];
                                sum[c + 3]++;
                            }
                        }

                    int c = FC(i, j);

                    if (c == 1) {
                        red[i][j] = sum[0] / sum[3];
                        green[i][j] = rawData[i][j];
                        blue[i][j] = sum[2] / sum[5];
                    }
                    else {
                        green[i][j] = sum[1] / sum[4];

                        if (c == 0) {
                            red[i][j] = rawData[i][j];
                            blue[i][j] = sum[2] / sum[5];
                        }
                        else {
                            red[i][j] = sum[0] / sum[3];
                            blue[i][j] = rawData[i][j];
                        }
                    }
                }//j
            }

        }
        int border;
        unsigned RawImageSource::FC(int row, int col) const
        {
            return ri->FC(row, col);
        }
        RawImage* ri;  // Copy of raw pixels, NOT corrected for initial gain, blackpoint etc.
    };
}