#!/usr/bin/env python3
# ******************************************************************************
#
#  Name:     gdalproximity
#  Project:  GDAL Python Interface
#  Purpose:  Application for computing raster proximity maps.
#  Author:   Frank Warmerdam, warmerdam@pobox.com
#
# ******************************************************************************
#  Copyright (c) 2008, Frank Warmerdam
#  Copyright (c) 2009-2011, Even Rouault <even dot rouault at spatialys.com>
#  Copyright (c) 2021, Idan Miara <idan@miara.com>
#
# SPDX-License-Identifier: MIT
# ******************************************************************************

import sys
from typing import Optional, Sequence

from osgeo import gdal
from osgeo_utils.auxiliary.util import GetOutputDriverFor, enable_gdal_exceptions


def Usage(isError):
    f = sys.stderr if isError else sys.stdout
    print(
        """
Usage: gdal_proximity [--help] [--help-general]
                  <srcfile> <dstfile> [-srcband <n>] [-dstband <n>]
                  [-of <format>] [-co <name>=<value>]...
                  [-ot {Byte|UInt16|UInt32|Float32|etc}]
                  [-values <n>,<n>,<n>] [-distunits {PIXEL|GEO}]
                  [-maxdist <n>] [-nodata <n>] [-use_input_nodata {YES|NO}]
                  [-fixed-buf-val <n>] [-q] """,
        file=f,
    )
    return 2 if isError else 0


@enable_gdal_exceptions
def main(argv=sys.argv):

    driver_name = None
    creation_options = []
    alg_options = []
    src_filename = None
    src_band_n = 1
    dst_filename = None
    dst_band_n = 1
    creation_type = "Float32"
    quiet = False

    argv = gdal.GeneralCmdLineProcessor(argv)
    if argv is None:
        return 0

    # Parse command line arguments.
    i = 1
    while i < len(argv):
        arg = argv[i]

        if arg == "--help":
            return Usage(isError=False)

        elif arg == "-of" or arg == "-f":
            i = i + 1
            driver_name = argv[i]

        elif arg == "-co":
            i = i + 1
            creation_options.append(argv[i])

        elif arg == "-ot":
            i = i + 1
            creation_type = argv[i]

        elif arg == "-maxdist":
            i = i + 1
            alg_options.append("MAXDIST=" + argv[i])

        elif arg == "-values":
            i = i + 1
            alg_options.append("VALUES=" + argv[i])

        elif arg == "-distunits":
            i = i + 1
            alg_options.append("DISTUNITS=" + argv[i])

        elif arg == "-nodata":
            i = i + 1
            alg_options.append("NODATA=" + argv[i])

        elif arg == "-use_input_nodata":
            i = i + 1
            alg_options.append("USE_INPUT_NODATA=" + argv[i])

        elif arg == "-fixed-buf-val":
            i = i + 1
            alg_options.append("FIXED_BUF_VAL=" + argv[i])

        elif arg == "-srcband":
            i = i + 1
            src_band_n = int(argv[i])

        elif arg == "-dstband":
            i = i + 1
            dst_band_n = int(argv[i])

        elif arg == "-q" or arg == "-quiet":
            quiet = True

        elif arg[0] == "-":
            sys.stderr.write("Unrecognized option : %s\n" % argv[i])
            return Usage(isError=True)

        elif src_filename is None:
            src_filename = argv[i]

        elif dst_filename is None:
            dst_filename = argv[i]

        else:
            return Usage(isError=True)

        i = i + 1

    if src_filename is None:
        sys.stderr.write("Missing <srcfile>\n")
        return Usage(isError=True)

    if dst_filename is None:
        sys.stderr.write("Missing <dstfile>\n")
        return Usage(isError=True)

    return gdal_proximity(
        src_filename=src_filename,
        src_band_n=src_band_n,
        dst_filename=dst_filename,
        dst_band_n=dst_band_n,
        driver_name=driver_name,
        creation_type=creation_type,
        creation_options=creation_options,
        alg_options=alg_options,
        quiet=quiet,
    )


def gdal_proximity(
    src_filename: Optional[str] = None,
    src_band_n: int = 1,
    dst_filename: Optional[str] = None,
    dst_band_n: int = 1,
    driver_name: Optional[str] = None,
    creation_type: str = "Float32",
    creation_options: Optional[Sequence[str]] = None,
    alg_options: Optional[Sequence[str]] = None,
    quiet: bool = False,
):

    # =============================================================================
    #    Open source file
    # =============================================================================
    creation_options = creation_options or []
    alg_options = alg_options or []
    src_ds = gdal.Open(src_filename)

    if src_ds is None:
        print("Unable to open %s" % src_filename)
        return 1

    srcband = src_ds.GetRasterBand(src_band_n)

    # =============================================================================
    #       Try opening the destination file as an existing file.
    # =============================================================================

    try:
        driver_name = gdal.IdentifyDriver(dst_filename)
        if driver_name is not None:
            dst_ds = gdal.Open(dst_filename, gdal.GA_Update)
            dstband = dst_ds.GetRasterBand(dst_band_n)
        else:
            dst_ds = None
    except Exception:
        dst_ds = None

    # =============================================================================
    #     Create output file.
    # =============================================================================
    if dst_ds is None:
        if driver_name is None:
            driver_name = GetOutputDriverFor(dst_filename)

        drv = gdal.GetDriverByName(driver_name)
        dst_ds = drv.Create(
            dst_filename,
            src_ds.RasterXSize,
            src_ds.RasterYSize,
            1,
            gdal.GetDataTypeByName(creation_type),
            creation_options,
        )

        dst_ds.SetGeoTransform(src_ds.GetGeoTransform())
        dst_ds.SetProjection(src_ds.GetProjectionRef())

        dstband = dst_ds.GetRasterBand(1)

    # =============================================================================
    #    Invoke algorithm.
    # =============================================================================

    if quiet:
        prog_func = None
    else:
        prog_func = gdal.TermProgress_nocb

    gdal.ComputeProximity(srcband, dstband, alg_options, callback=prog_func)

    srcband = None
    dstband = None
    src_ds = None
    dst_ds = None


if __name__ == "__main__":
    sys.exit(main(sys.argv))
