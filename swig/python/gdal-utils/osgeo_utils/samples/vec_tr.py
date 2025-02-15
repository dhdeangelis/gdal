#!/usr/bin/env python3
###############################################################################
#
# Project:  OGR Python samples
# Purpose:  Apply a transformation to all OGR geometries.
# Author:   Frank Warmerdam, warmerdam@pobox.com
#
###############################################################################
# Copyright (c) 2006, Frank Warmerdam <warmerdam@pobox.com>
#
# SPDX-License-Identifier: MIT
###############################################################################

import sys

from osgeo import ogr

#############################################################################


def TransformPoint(xyz):

    x = xyz[0]
    y = xyz[1]
    z = xyz[2]

    x = x + 1000

    return (x, y, z)


#############################################################################


def WalkAndTransform(geom):

    if geom.GetGeometryCount() > 0:
        for i in range(geom.GetGeometryCount()):
            old_geom = geom.GetGeometryRef(i)
            new_geom = WalkAndTransform(old_geom)
            if new_geom is not old_geom:
                geom.SetGeometryDirectly(new_geom)
        return geom

    for i in range(geom.GetPointCount()):
        xyz = (geom.GetX(i), geom.GetY(i), geom.GetZ(i))

        xyz = TransformPoint(xyz)

        geom.SetPoint(i, xyz[0], xyz[1], xyz[2])

    return geom


#############################################################################


def Usage():
    print("Usage: vec_tr.py infile outfile [layer]")
    print("")
    return 2


def main(argv=sys.argv):
    infile = None
    outfile = None
    layer_name = None

    for arg in argv[1:]:
        if infile is None:
            infile = arg

        elif outfile is None:
            outfile = arg

        elif layer_name is None:
            layer_name = arg

        else:
            return Usage()

    if outfile is None:
        return Usage()

    #############################################################################
    # Open the datasource to operate on.

    in_ds = ogr.Open(infile, update=0)

    if layer_name is not None:
        in_layer = in_ds.GetLayerByName(layer_name)
    else:
        in_layer = in_ds.GetLayer(0)

    in_defn = in_layer.GetLayerDefn()

    #############################################################################
    # Create output file with similar information.

    shp_driver = ogr.GetDriverByName("ESRI Shapefile")
    shp_driver.DeleteDataSource(outfile)

    shp_ds = shp_driver.CreateDataSource(outfile)

    shp_layer = shp_ds.CreateLayer(
        in_defn.GetName(), geom_type=in_defn.GetGeomType(), srs=in_layer.GetSpatialRef()
    )

    in_field_count = in_defn.GetFieldCount()

    for fld_index in range(in_field_count):
        src_fd = in_defn.GetFieldDefn(fld_index)

        fd = ogr.FieldDefn(src_fd.GetName(), src_fd.GetType())
        fd.SetWidth(src_fd.GetWidth())
        fd.SetPrecision(src_fd.GetPrecision())
        shp_layer.CreateField(fd)

    #############################################################################
    # Process all features in input layer.

    in_feat = in_layer.GetNextFeature()
    while in_feat is not None:

        geom = in_feat.GetGeometryRef().Clone()

        geom = WalkAndTransform(geom)

        out_feat = ogr.Feature(feature_def=shp_layer.GetLayerDefn())
        out_feat.SetFrom(in_feat)
        out_feat.SetGeometryDirectly(geom)

        shp_layer.CreateFeature(out_feat)
        out_feat.Destroy()

        in_feat.Destroy()
        in_feat = in_layer.GetNextFeature()

    #############################################################################
    # Cleanup

    shp_ds.Destroy()
    in_ds.Destroy()

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
