/******************************************************************************
 *
 * Project:  GDAL
 * Purpose:  Algorithm to apply a transformer to geolocation style bands.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2012, Frank Warmerdam
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "gdal_alg.h"

#include <cstring>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_progress.h"
#include "gdal.h"
#include "gdal_alg_priv.h"
#include "gdal_priv.h"

/************************************************************************/
/*                     GDALTransformGeolocations()                      */
/************************************************************************/

/**
 * Transform locations held in bands.
 *
 * The X/Y and possibly Z values in the identified bands are transformed
 * using a spatial transformer.  The changed values are written back to the
 * source bands so they need to be updateable.
 *
 * @param hXBand the band containing the X locations (usually long/easting).
 * @param hYBand the band containing the Y locations (usually lat/northing).
 * @param hZBand the band containing the Z locations (may be NULL).
 * @param pfnTransformer the transformer function.
 * @param pTransformArg the callback data for the transformer function.
 * @param pfnProgress callback for reporting algorithm progress matching the
 * GDALProgressFunc() semantics.  May be NULL.
 * @param pProgressArg callback argument passed to pfnProgress.
 * @param papszOptions list of name/value options - none currently supported.
 *
 * @return CE_None on success or CE_Failure if an error occurs.
 */

CPLErr GDALTransformGeolocations(GDALRasterBandH hXBand, GDALRasterBandH hYBand,
                                 GDALRasterBandH hZBand,
                                 GDALTransformerFunc pfnTransformer,
                                 void *pTransformArg,
                                 GDALProgressFunc pfnProgress,
                                 void *pProgressArg,
                                 CPL_UNUSED char **papszOptions)

{
    VALIDATE_POINTER1(hXBand, "GDALTransformGeolocations", CE_Failure);
    VALIDATE_POINTER1(hYBand, "GDALTransformGeolocations", CE_Failure);

    if (pfnProgress == nullptr)
        pfnProgress = GDALDummyProgress;

    /* -------------------------------------------------------------------- */
    /*      Ensure the bands are matching in size.                          */
    /* -------------------------------------------------------------------- */
    GDALRasterBand *poXBand = reinterpret_cast<GDALRasterBand *>(hXBand);
    GDALRasterBand *poYBand = reinterpret_cast<GDALRasterBand *>(hYBand);
    GDALRasterBand *poZBand = reinterpret_cast<GDALRasterBand *>(hZBand);
    const int nXSize = poXBand->GetXSize();
    const int nYSize = poXBand->GetYSize();

    if (nXSize != poYBand->GetXSize() || nYSize != poYBand->GetYSize() ||
        (poZBand != nullptr && nXSize != poZBand->GetXSize()) ||
        (poZBand != nullptr && nYSize != poZBand->GetYSize()))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Size of X, Y and/or Z bands do not match.");
        return CE_Failure;
    }

    /* -------------------------------------------------------------------- */
    /*      Allocate a buffer large enough to hold one whole row.           */
    /* -------------------------------------------------------------------- */
    std::unique_ptr<double, VSIFreeReleaser> padfX(
        static_cast<double *>(VSI_MALLOC2_VERBOSE(sizeof(double), nXSize)));
    std::unique_ptr<double, VSIFreeReleaser> padfY(
        static_cast<double *>(VSI_MALLOC2_VERBOSE(sizeof(double), nXSize)));
    std::unique_ptr<double, VSIFreeReleaser> padfZ(
        static_cast<double *>(VSI_MALLOC2_VERBOSE(sizeof(double), nXSize)));
    std::unique_ptr<int, VSIFreeReleaser> panSuccess(
        static_cast<int *>(VSI_MALLOC2_VERBOSE(sizeof(int), nXSize)));
    CPLErr eErr = padfX && padfY && padfZ && panSuccess ? CE_None : CE_Failure;

    pfnProgress(0.0, "", pProgressArg);
    for (int iLine = 0; eErr == CE_None && iLine < nYSize; iLine++)
    {
        eErr = poXBand->RasterIO(GF_Read, 0, iLine, nXSize, 1, padfX.get(),
                                 nXSize, 1, GDT_Float64, 0, 0, nullptr);
        if (eErr == CE_None)
            eErr = poYBand->RasterIO(GF_Read, 0, iLine, nXSize, 1, padfY.get(),
                                     nXSize, 1, GDT_Float64, 0, 0, nullptr);
        if (eErr == CE_None && poZBand != nullptr)
            eErr = poZBand->RasterIO(GF_Read, 0, iLine, nXSize, 1, padfZ.get(),
                                     nXSize, 1, GDT_Float64, 0, 0, nullptr);
        else
            memset(padfZ.get(), 0, sizeof(double) * nXSize);

        if (eErr == CE_None)
        {
            pfnTransformer(pTransformArg, FALSE, nXSize, padfX.get(),
                           padfY.get(), padfZ.get(), panSuccess.get());
        }

        if (eErr == CE_None)
            eErr = poXBand->RasterIO(GF_Write, 0, iLine, nXSize, 1, padfX.get(),
                                     nXSize, 1, GDT_Float64, 0, 0, nullptr);
        if (eErr == CE_None)
            eErr = poYBand->RasterIO(GF_Write, 0, iLine, nXSize, 1, padfY.get(),
                                     nXSize, 1, GDT_Float64, 0, 0, nullptr);
        if (eErr == CE_None && poZBand != nullptr)
            eErr = poZBand->RasterIO(GF_Write, 0, iLine, nXSize, 1, padfZ.get(),
                                     nXSize, 1, GDT_Float64, 0, 0, nullptr);

        if (eErr == CE_None)
            pfnProgress((iLine + 1) / static_cast<double>(nYSize), "",
                        pProgressArg);
    }

    return eErr;
}
