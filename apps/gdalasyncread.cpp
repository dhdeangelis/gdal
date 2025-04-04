/******************************************************************************
 *
 * Project:  GDAL Utilities
 * Purpose:  GDAL Async Image Reader, primarily for testing async api.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2010, Frank Warmerdam
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_vsi.h"
#include "cpl_conv.h"
#include "cpl_string.h"
#include "gdal_version.h"
#include "gdal_priv.h"
#include "ogr_spatialref.h"

/* ******************************************************************** */
/*                               Usage()                                */
/* ******************************************************************** */

static void Usage()

{
    printf("Usage: gdalasyncread [--help] [--help-general]\n"
           "       [-ot {Byte/Int16/UInt16/UInt32/Int32/Float32/Float64/\n"
           "             CInt16/CInt32/CFloat32/CFloat64}]\n"
           "       [-of <format>] [-b <band>]\n"
           "       [-outsize <xsize>[%%] <ysize>[%%]]\n"
           "       [-srcwin <xoff> <yoff> <xsize> <ysize>]\n"
           "       [-co <NAME>=<VALUE>]... [-ao <NAME>=<VALUE>]...\n"
           "       [-to <timeout>] [-multi]\n"
           "       <src_dataset> <dst_dataset>\n\n");

    printf("%s\n\n", GDALVersionInfo("--version"));
    printf("The following format drivers are enabled and support writing:\n");
    for (int iDr = 0; iDr < GDALGetDriverCount(); iDr++)
    {
        GDALDriverH hDriver = GDALGetDriver(iDr);

        if (GDALGetMetadataItem(hDriver, GDAL_DCAP_CREATE, nullptr) !=
                nullptr ||
            GDALGetMetadataItem(hDriver, GDAL_DCAP_CREATECOPY, nullptr) !=
                nullptr)
        {
            printf("  %s: %s\n", GDALGetDriverShortName(hDriver),
                   GDALGetDriverLongName(hDriver));
        }
    }
}

/************************************************************************/
/*                                main()                                */
/************************************************************************/

int main(int argc, char **argv)

{
    GDALDatasetH hSrcDS, hDstDS;
    GDALDataset *poSrcDS, *poDstDS = nullptr;
    int i;
    int nRasterXSize, nRasterYSize;
    const char *pszSource = nullptr, *pszDest = nullptr, *pszFormat = "GTiff";
    GDALDriverH hDriver;
    int *panBandList = nullptr, nBandCount = 0, bDefBands = TRUE;
    GDALDataType eOutputType = GDT_Unknown;
    int nOXSize = 0, nOYSize = 0;
    char **papszCreateOptions = nullptr;
    char **papszAsyncOptions = nullptr;
    int anSrcWin[4];
    int bQuiet = FALSE;
    GDALProgressFunc pfnProgress = GDALTermProgress;
    int iSrcFileArg = -1, iDstFileArg = -1;
    int bMulti = FALSE;
    double dfTimeout = -1.0;
    const char *pszOXSize = nullptr, *pszOYSize = nullptr;

    anSrcWin[0] = 0;
    anSrcWin[1] = 0;
    anSrcWin[2] = 0;
    anSrcWin[3] = 0;

    /* Check strict compilation and runtime library version as we use C++ API */
    if (!GDAL_CHECK_VERSION(argv[0]))
        exit(1);

    /* -------------------------------------------------------------------- */
    /*      Register standard GDAL drivers, and process generic GDAL        */
    /*      command options.                                                */
    /* -------------------------------------------------------------------- */
    GDALAllRegister();
    argc = GDALGeneralCmdLineProcessor(argc, &argv, 0);
    if (argc < 1)
        exit(-argc);

    /* -------------------------------------------------------------------- */
    /*      Handle command line arguments.                                  */
    /* -------------------------------------------------------------------- */
    for (i = 1; i < argc; i++)
    {
        if (EQUAL(argv[i], "--utility_version"))
        {
            printf("%s was compiled against GDAL %s and is running against "
                   "GDAL %s\n",
                   argv[0], GDAL_RELEASE_NAME, GDALVersionInfo("RELEASE_NAME"));
            return 0;
        }
        else if (EQUAL(argv[i], "--help"))
        {
            Usage();
        }
        else if ((EQUAL(argv[i], "-of") || EQUAL(argv[i], "-f")) &&
                 i < argc - 1)
            pszFormat = argv[++i];

        else if (EQUAL(argv[i], "-quiet"))
        {
            bQuiet = TRUE;
            pfnProgress = GDALDummyProgress;
        }

        else if (EQUAL(argv[i], "-ot") && i < argc - 1)
        {
            for (int iType = 1; iType < GDT_TypeCount; iType++)
            {
                if (GDALGetDataTypeName((GDALDataType)iType) != nullptr &&
                    EQUAL(GDALGetDataTypeName((GDALDataType)iType),
                          argv[i + 1]))
                {
                    eOutputType = (GDALDataType)iType;
                }
            }

            if (eOutputType == GDT_Unknown)
            {
                printf("Unknown output pixel type: %s\n", argv[i + 1]);
                Usage();
                GDALDestroyDriverManager();
                exit(2);
            }
            i++;
        }

        else if (EQUAL(argv[i], "-b") && i < argc - 1)
        {
            if (atoi(argv[i + 1]) < 1)
            {
                printf("Unrecognizable band number (%s).\n", argv[i + 1]);
                Usage();
                GDALDestroyDriverManager();
                exit(2);
            }

            nBandCount++;
            panBandList = static_cast<int *>(
                CPLRealloc(panBandList, sizeof(int) * nBandCount));
            panBandList[nBandCount - 1] = atoi(argv[++i]);

            if (panBandList[nBandCount - 1] != nBandCount)
                bDefBands = FALSE;
        }
        else if (EQUAL(argv[i], "-co") && i < argc - 1)
        {
            papszCreateOptions = CSLAddString(papszCreateOptions, argv[++i]);
        }

        else if (EQUAL(argv[i], "-ao") && i < argc - 1)
        {
            papszAsyncOptions = CSLAddString(papszAsyncOptions, argv[++i]);
        }

        else if (EQUAL(argv[i], "-to") && i < argc - 1)
        {
            dfTimeout = CPLAtof(argv[++i]);
        }

        else if (EQUAL(argv[i], "-outsize") && i < argc - 2)
        {
            pszOXSize = argv[++i];
            pszOYSize = argv[++i];
        }

        else if (EQUAL(argv[i], "-srcwin") && i < argc - 4)
        {
            anSrcWin[0] = atoi(argv[++i]);
            anSrcWin[1] = atoi(argv[++i]);
            anSrcWin[2] = atoi(argv[++i]);
            anSrcWin[3] = atoi(argv[++i]);
        }

        else if (EQUAL(argv[i], "-multi"))
        {
            bMulti = TRUE;
        }
        else if (argv[i][0] == '-')
        {
            printf("Option %s incomplete, or not recognised.\n\n", argv[i]);
            Usage();
            GDALDestroyDriverManager();
            exit(2);
        }
        else if (pszSource == nullptr)
        {
            iSrcFileArg = i;
            pszSource = argv[i];
        }
        else if (pszDest == nullptr)
        {
            pszDest = argv[i];
            iDstFileArg = i;
        }

        else
        {
            printf("Too many command options.\n\n");
            Usage();
            GDALDestroyDriverManager();
            exit(2);
        }
    }

    if (pszDest == nullptr)
    {
        Usage();
        GDALDestroyDriverManager();
        exit(10);
    }

    if (strcmp(pszSource, pszDest) == 0)
    {
        fprintf(stderr, "Source and destination datasets must be different.\n");
        GDALDestroyDriverManager();
        exit(1);
    }

    /* -------------------------------------------------------------------- */
    /*      Attempt to open source file.                                    */
    /* -------------------------------------------------------------------- */

    hSrcDS = GDALOpenShared(pszSource, GA_ReadOnly);
    poSrcDS = (GDALDataset *)hSrcDS;

    if (hSrcDS == nullptr)
    {
        fprintf(stderr, "GDALOpen failed - %d\n%s\n", CPLGetLastErrorNo(),
                CPLGetLastErrorMsg());
        GDALDestroyDriverManager();
        exit(1);
    }

    /* -------------------------------------------------------------------- */
    /*      Collect some information from the source file.                  */
    /* -------------------------------------------------------------------- */
    nRasterXSize = GDALGetRasterXSize(hSrcDS);
    nRasterYSize = GDALGetRasterYSize(hSrcDS);

    if (!bQuiet)
        printf("Input file size is %d, %d\n", nRasterXSize, nRasterYSize);

    if (anSrcWin[2] == 0 && anSrcWin[3] == 0)
    {
        anSrcWin[2] = nRasterXSize;
        anSrcWin[3] = nRasterYSize;
    }

    /* -------------------------------------------------------------------- */
    /*      Establish output size.                                          */
    /* -------------------------------------------------------------------- */
    if (pszOXSize == nullptr)
    {
        nOXSize = anSrcWin[2];
        nOYSize = anSrcWin[3];
    }
    else
    {
        nOXSize = static_cast<int>((pszOXSize[strlen(pszOXSize) - 1] == '%'
                                        ? CPLAtof(pszOXSize) / 100 * anSrcWin[2]
                                        : atoi(pszOXSize)));
        nOYSize = static_cast<int>((pszOYSize[strlen(pszOYSize) - 1] == '%'
                                        ? CPLAtof(pszOYSize) / 100 * anSrcWin[3]
                                        : atoi(pszOYSize)));
    }

    /* -------------------------------------------------------------------- */
    /*      Build band list to translate                                    */
    /* -------------------------------------------------------------------- */
    if (nBandCount == 0)
    {
        nBandCount = GDALGetRasterCount(hSrcDS);
        if (nBandCount == 0)
        {
            fprintf(stderr,
                    "Input file has no bands, and so cannot be translated.\n");
            GDALDestroyDriverManager();
            exit(1);
        }

        panBandList = static_cast<int *>(CPLMalloc(sizeof(int) * nBandCount));
        for (i = 0; i < nBandCount; i++)
            panBandList[i] = i + 1;
    }
    else
    {
        for (i = 0; i < nBandCount; i++)
        {
            if (panBandList[i] < 1 ||
                panBandList[i] > GDALGetRasterCount(hSrcDS))
            {
                fprintf(
                    stderr,
                    "Band %d requested, but only bands 1 to %d available.\n",
                    panBandList[i], GDALGetRasterCount(hSrcDS));
                GDALDestroyDriverManager();
                exit(2);
            }
        }

        if (nBandCount != GDALGetRasterCount(hSrcDS))
            bDefBands = FALSE;
    }

    /* -------------------------------------------------------------------- */
    /*      Verify source window.                                           */
    /* -------------------------------------------------------------------- */
    if (anSrcWin[0] < 0 || anSrcWin[1] < 0 || anSrcWin[2] <= 0 ||
        anSrcWin[3] <= 0 ||
        anSrcWin[0] + anSrcWin[2] > GDALGetRasterXSize(hSrcDS) ||
        anSrcWin[1] + anSrcWin[3] > GDALGetRasterYSize(hSrcDS))
    {
        fprintf(stderr,
                "-srcwin %d %d %d %d falls outside raster size of %dx%d\n"
                "or is otherwise illegal.\n",
                anSrcWin[0], anSrcWin[1], anSrcWin[2], anSrcWin[3],
                GDALGetRasterXSize(hSrcDS), GDALGetRasterYSize(hSrcDS));
        exit(1);
    }

    /* -------------------------------------------------------------------- */
    /*      Find the output driver.                                         */
    /* -------------------------------------------------------------------- */
    hDriver = GDALGetDriverByName(pszFormat);

    if (hDriver == nullptr)
    {
        printf("Output driver `%s' not recognised.\n", pszFormat);
    }
    else if (GDALGetMetadataItem(hDriver, GDAL_DCAP_CREATE, nullptr) == nullptr)
    {
        printf("Output driver '%s' does not support direct creation.\n",
               pszFormat);
        hDriver = nullptr;
    }

    if (hDriver == nullptr)
    {
        printf("The following format drivers are enabled and support "
               "writing:\n");
        for (int iDr = 0; iDr < GDALGetDriverCount(); iDr++)
        {
            GDALDriverH hDriver = GDALGetDriver(iDr);

            if (GDALGetMetadataItem(hDriver, GDAL_DCAP_CREATE, nullptr) !=
                nullptr)
            {
                printf("  %s: %s\n", GDALGetDriverShortName(hDriver),
                       GDALGetDriverLongName(hDriver));
            }
        }
        printf("\n");
        Usage();

        GDALClose(hSrcDS);
        CPLFree(panBandList);
        GDALDestroyDriverManager();
        CSLDestroy(argv);
        CSLDestroy(papszCreateOptions);
        exit(1);
    }

    /* -------------------------------------------------------------------- */
    /*      Establish the pixel data type to use.                           */
    /* -------------------------------------------------------------------- */
    if (eOutputType == GDT_Unknown)
        eOutputType = poSrcDS->GetRasterBand(1)->GetRasterDataType();

    /* -------------------------------------------------------------------- */
    /*      Allocate one big buffer for the whole imagery area to           */
    /*      transfer.                                                       */
    /* -------------------------------------------------------------------- */
    const int nBytesPerPixel =
        nBandCount * GDALGetDataTypeSizeBytes(eOutputType);
    void *pImage = VSIMalloc3(nOXSize, nOYSize, nBytesPerPixel);

    if (pImage == nullptr)
    {
        printf("Unable to allocate %dx%dx%d byte window buffer.\n", nOXSize,
               nOYSize, nBytesPerPixel);
        exit(1);
    }

    /* -------------------------------------------------------------------- */
    /*      Establish view window                                           */
    /* -------------------------------------------------------------------- */
    int nPixelSpace = nBytesPerPixel;
    int nLineSpace = nBytesPerPixel * nOXSize;
    int nBandSpace = nBytesPerPixel / nBandCount;

    GDALAsyncReader *poAsyncReq = poSrcDS->BeginAsyncReader(
        anSrcWin[0], anSrcWin[1], anSrcWin[2], anSrcWin[3], pImage, nOXSize,
        nOYSize, eOutputType, nBandCount, panBandList, nPixelSpace, nLineSpace,
        nBandSpace, papszAsyncOptions);

    if (poAsyncReq == nullptr)
        exit(1);

    /* -------------------------------------------------------------------- */
    /*      Process until done or an error.                                 */
    /* -------------------------------------------------------------------- */
    GDALAsyncStatusType eAStatus;
    CPLErr eErr = CE_None;
    int iMultiCounter = 0;

    hDstDS = nullptr;

    do
    {
        /* ====================================================================
         */
        /*      Create the output file, and initialize if needed. */
        /* ====================================================================
         */
        if (hDstDS == nullptr)
        {
            CPLString osOutFilename = pszDest;

            if (bMulti)
                osOutFilename.Printf("%s_%d", pszDest, iMultiCounter++);

            hDstDS = GDALCreate(hDriver, osOutFilename, nOXSize, nOYSize,
                                nBandCount, eOutputType, papszCreateOptions);
            if (hDstDS == nullptr)
            {
                exit(1);
            }

            poDstDS = (GDALDataset *)hDstDS;

            /* --------------------------------------------------------------------
             */
            /*      Copy georeferencing. */
            /* --------------------------------------------------------------------
             */
            double adfGeoTransform[6];

            if (poSrcDS->GetGeoTransform(adfGeoTransform) == CE_None)
            {
                adfGeoTransform[0] += anSrcWin[0] * adfGeoTransform[1] +
                                      anSrcWin[1] * adfGeoTransform[2];
                adfGeoTransform[3] += anSrcWin[0] * adfGeoTransform[4] +
                                      anSrcWin[1] * adfGeoTransform[5];

                adfGeoTransform[1] *= anSrcWin[2] / (double)nOXSize;
                adfGeoTransform[2] *= anSrcWin[3] / (double)nOYSize;
                adfGeoTransform[4] *= anSrcWin[2] / (double)nOXSize;
                adfGeoTransform[5] *= anSrcWin[3] / (double)nOYSize;

                poDstDS->SetGeoTransform(adfGeoTransform);
            }

            poDstDS->SetProjection(poSrcDS->GetProjectionRef());

            /* --------------------------------------------------------------------
             */
            /*      Transfer generally applicable metadata. */
            /* --------------------------------------------------------------------
             */
            poDstDS->SetMetadata(poSrcDS->GetMetadata());
        }

        /* ====================================================================
         */
        /*      Fetch an update and write it to the output file. */
        /* ====================================================================
         */

        int nUpXOff;
        int nUpYOff;
        int nUpXSize;
        int nUpYSize;

        eAStatus = poAsyncReq->GetNextUpdatedRegion(
            dfTimeout, &nUpXOff, &nUpYOff, &nUpXSize, &nUpYSize);

        if (eAStatus != GARIO_UPDATE && eAStatus != GARIO_COMPLETE)
            continue;

        if (!bQuiet)
        {
            printf("Got %dx%d @ (%d,%d)\n", nUpXSize, nUpYSize, nUpXOff,
                   nUpYOff);
        }

        poAsyncReq->LockBuffer();
        eErr = poDstDS->RasterIO(
            GF_Write, nUpXOff, nUpYOff, nUpXSize, nUpYSize,
            ((GByte *)pImage) + nUpXOff * nPixelSpace + nUpYOff * nLineSpace,
            nUpXSize, nUpYSize, eOutputType, nBandCount, nullptr, nPixelSpace,
            nLineSpace, nBandSpace, nullptr);
        poAsyncReq->UnlockBuffer();

        /* --------------------------------------------------------------------
         */
        /*      In multi mode we will close this file and reopen another for */
        /*      the next request. */
        /* --------------------------------------------------------------------
         */
        if (bMulti)
        {
            GDALClose(hDstDS);
            hDstDS = nullptr;
        }
        else
        {
            GDALFlushCache(hDstDS);
        }
    } while (eAStatus != GARIO_ERROR && eAStatus != GARIO_COMPLETE &&
             eErr == CE_None);

    poSrcDS->EndAsyncReader(poAsyncReq);

    /* -------------------------------------------------------------------- */
    /*      Cleanup.                                                        */
    /* -------------------------------------------------------------------- */
    VSIFree(pImage);

    if (hDstDS)
        GDALClose(hDstDS);

    GDALClose(hSrcDS);

    CPLFree(panBandList);

    CSLDestroy(argv);
    CSLDestroy(papszCreateOptions);
    CSLDestroy(papszAsyncOptions);

    GDALDumpOpenDatasets(stderr);
    GDALDestroyDriverManager();
}
