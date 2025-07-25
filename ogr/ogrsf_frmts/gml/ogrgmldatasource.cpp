/******************************************************************************
 *
 * Project:  OGR
 * Purpose:  Implements OGRGMLDataSource class.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2002, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2007-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 *
 ******************************************************************************
 * Contributor: Alessandro Furieri, a.furieri@lqt.it
 * Portions of this module implementing GML_SKIP_RESOLVE_ELEMS HUGE
 * Developed for Faunalia ( http://www.faunalia.it) with funding from
 * Regione Toscana - Settore SISTEMA INFORMATIVO TERRITORIALE ED AMBIENTALE
 *
 ****************************************************************************/

#include "cpl_port.h"
#include "ogr_gml.h"

#include <algorithm>
#include <vector>

#include "cpl_conv.h"
#include "cpl_http.h"
#include "cpl_string.h"
#include "cpl_vsi_error.h"
#include "gmlreaderp.h"
#include "gmlregistry.h"
#include "gmlutils.h"
#include "ogr_p.h"
#include "ogr_schema_override.h"
#include "parsexsd.h"
#include "memdataset.h"

/************************************************************************/
/*                   ReplaceSpaceByPct20IfNeeded()                      */
/************************************************************************/

static CPLString ReplaceSpaceByPct20IfNeeded(const char *pszURL)
{
    // Replace ' ' by '%20'.
    CPLString osRet = pszURL;
    const char *pszNeedle = strstr(pszURL, "; ");
    if (pszNeedle)
    {
        char *pszTmp = static_cast<char *>(CPLMalloc(strlen(pszURL) + 2 + 1));
        const int nBeforeNeedle = static_cast<int>(pszNeedle - pszURL);
        memcpy(pszTmp, pszURL, nBeforeNeedle);
        strcpy(pszTmp + nBeforeNeedle, ";%20");
        strcpy(pszTmp + nBeforeNeedle + strlen(";%20"),
               pszNeedle + strlen("; "));
        osRet = pszTmp;
        CPLFree(pszTmp);
    }

    return osRet;
}

/************************************************************************/
/*                         OGRGMLDataSource()                         */
/************************************************************************/

OGRGMLDataSource::OGRGMLDataSource()
    : papoLayers(nullptr), nLayers(0), papszCreateOptions(nullptr),
      fpOutput(nullptr), bFpOutputIsNonSeekable(false),
      bFpOutputSingleFile(false), bBBOX3D(false), nBoundedByLocation(-1),
      nSchemaInsertLocation(-1), bIsOutputGML3(false),
      bIsOutputGML3Deegree(false), bIsOutputGML32(false),
      eSRSNameFormat(SRSNAME_SHORT), bWriteSpaceIndentation(true),
      poReader(nullptr), bOutIsTempFile(false), bExposeGMLId(false),
      bExposeFid(false), bIsWFS(false), bUseGlobalSRSName(false),
      m_bInvertAxisOrderIfLatLong(false), m_bConsiderEPSGAsURN(false),
      m_eSwapCoordinates(GML_SWAP_AUTO), m_bGetSecondaryGeometryOption(false),
      eReadMode(STANDARD), poStoredGMLFeature(nullptr),
      poLastReadLayer(nullptr), bEmptyAsNull(true)
{
}

/************************************************************************/
/*                          ~OGRGMLDataSource()                         */
/************************************************************************/

OGRGMLDataSource::~OGRGMLDataSource()
{
    OGRGMLDataSource::Close();
}

/************************************************************************/
/*                                 Close()                              */
/************************************************************************/

CPLErr OGRGMLDataSource::Close()
{
    CPLErr eErr = CE_None;
    if (nOpenFlags != OPEN_FLAGS_CLOSED)
    {
        if (fpOutput && !m_bWriteError)
        {
            if (nLayers == 0)
                WriteTopElements();

            const char *pszPrefix = GetAppPrefix();
            if (GMLFeatureCollection())
                PrintLine(fpOutput, "</gml:FeatureCollection>");
            else if (RemoveAppPrefix())
                PrintLine(fpOutput, "</FeatureCollection>");
            else
                PrintLine(fpOutput, "</%s:FeatureCollection>", pszPrefix);

            if (bFpOutputIsNonSeekable)
            {
                VSIFCloseL(fpOutput);
                fpOutput = nullptr;
            }

            InsertHeader();

            if (!bFpOutputIsNonSeekable && nBoundedByLocation != -1 &&
                VSIFSeekL(fpOutput, nBoundedByLocation, SEEK_SET) == 0)
            {
                if (m_bWriteGlobalSRS && sBoundingRect.IsInit() &&
                    IsGML3Output())
                {
                    bool bCoordSwap = false;
                    char *pszSRSName =
                        m_poWriteGlobalSRS
                            ? GML_GetSRSName(m_poWriteGlobalSRS.get(),
                                             eSRSNameFormat, &bCoordSwap)
                            : CPLStrdup("");
                    char szLowerCorner[75] = {};
                    char szUpperCorner[75] = {};

                    OGRWktOptions coordOpts;

                    if (OGRGMLDataSource::GetLayerCount() == 1)
                    {
                        OGRLayer *poLayer = OGRGMLDataSource::GetLayer(0);
                        if (poLayer->GetLayerDefn()->GetGeomFieldCount() == 1)
                        {
                            const auto &oCoordPrec =
                                poLayer->GetLayerDefn()
                                    ->GetGeomFieldDefn(0)
                                    ->GetCoordinatePrecision();
                            if (oCoordPrec.dfXYResolution !=
                                OGRGeomCoordinatePrecision::UNKNOWN)
                            {
                                coordOpts.format = OGRWktFormat::F;
                                coordOpts.xyPrecision =
                                    OGRGeomCoordinatePrecision::
                                        ResolutionToPrecision(
                                            oCoordPrec.dfXYResolution);
                            }
                            if (oCoordPrec.dfZResolution !=
                                OGRGeomCoordinatePrecision::UNKNOWN)
                            {
                                coordOpts.format = OGRWktFormat::F;
                                coordOpts.zPrecision =
                                    OGRGeomCoordinatePrecision::
                                        ResolutionToPrecision(
                                            oCoordPrec.dfZResolution);
                            }
                        }
                    }

                    std::string wkt;
                    if (bCoordSwap)
                    {
                        wkt = OGRMakeWktCoordinate(
                            sBoundingRect.MinY, sBoundingRect.MinX,
                            sBoundingRect.MinZ, bBBOX3D ? 3 : 2, coordOpts);
                        memcpy(szLowerCorner, wkt.data(), wkt.size() + 1);

                        wkt = OGRMakeWktCoordinate(
                            sBoundingRect.MaxY, sBoundingRect.MaxX,
                            sBoundingRect.MaxZ, bBBOX3D ? 3 : 2, coordOpts);
                        memcpy(szUpperCorner, wkt.data(), wkt.size() + 1);
                    }
                    else
                    {
                        wkt = OGRMakeWktCoordinate(
                            sBoundingRect.MinX, sBoundingRect.MinY,
                            sBoundingRect.MinZ, bBBOX3D ? 3 : 2, coordOpts);
                        memcpy(szLowerCorner, wkt.data(), wkt.size() + 1);

                        wkt = OGRMakeWktCoordinate(
                            sBoundingRect.MaxX, sBoundingRect.MaxY,
                            sBoundingRect.MaxZ, (bBBOX3D) ? 3 : 2, coordOpts);
                        memcpy(szUpperCorner, wkt.data(), wkt.size() + 1);
                    }
                    if (bWriteSpaceIndentation)
                        VSIFPrintfL(fpOutput, "  ");
                    PrintLine(
                        fpOutput,
                        "<gml:boundedBy><gml:Envelope%s%s><gml:lowerCorner>%s"
                        "</gml:lowerCorner><gml:upperCorner>%s</"
                        "gml:upperCorner>"
                        "</gml:Envelope></gml:boundedBy>",
                        bBBOX3D ? " srsDimension=\"3\"" : "", pszSRSName,
                        szLowerCorner, szUpperCorner);
                    CPLFree(pszSRSName);
                }
                else if (m_bWriteGlobalSRS && sBoundingRect.IsInit())
                {
                    if (bWriteSpaceIndentation)
                        VSIFPrintfL(fpOutput, "  ");
                    PrintLine(fpOutput, "<gml:boundedBy>");
                    if (bWriteSpaceIndentation)
                        VSIFPrintfL(fpOutput, "    ");
                    PrintLine(fpOutput, "<gml:Box>");
                    if (bWriteSpaceIndentation)
                        VSIFPrintfL(fpOutput, "      ");
                    VSIFPrintfL(fpOutput,
                                "<gml:coord><gml:X>%.16g</gml:X>"
                                "<gml:Y>%.16g</gml:Y>",
                                sBoundingRect.MinX, sBoundingRect.MinY);
                    if (bBBOX3D)
                        VSIFPrintfL(fpOutput, "<gml:Z>%.16g</gml:Z>",
                                    sBoundingRect.MinZ);
                    PrintLine(fpOutput, "</gml:coord>");
                    if (bWriteSpaceIndentation)
                        VSIFPrintfL(fpOutput, "      ");
                    VSIFPrintfL(fpOutput,
                                "<gml:coord><gml:X>%.16g</gml:X>"
                                "<gml:Y>%.16g</gml:Y>",
                                sBoundingRect.MaxX, sBoundingRect.MaxY);
                    if (bBBOX3D)
                        VSIFPrintfL(fpOutput, "<gml:Z>%.16g</gml:Z>",
                                    sBoundingRect.MaxZ);
                    PrintLine(fpOutput, "</gml:coord>");
                    if (bWriteSpaceIndentation)
                        VSIFPrintfL(fpOutput, "    ");
                    PrintLine(fpOutput, "</gml:Box>");
                    if (bWriteSpaceIndentation)
                        VSIFPrintfL(fpOutput, "  ");
                    PrintLine(fpOutput, "</gml:boundedBy>");
                }
                else
                {
                    if (bWriteSpaceIndentation)
                        VSIFPrintfL(fpOutput, "  ");
                    if (IsGML3Output())
                        PrintLine(
                            fpOutput,
                            "<gml:boundedBy><gml:Null /></gml:boundedBy>");
                    else
                        PrintLine(fpOutput, "<gml:boundedBy><gml:null>missing"
                                            "</gml:null></gml:boundedBy>");
                }
            }
        }

        if (fpOutput)
            VSIFCloseL(fpOutput);
        fpOutput = nullptr;

        CSLDestroy(papszCreateOptions);
        papszCreateOptions = nullptr;

        for (int i = 0; i < nLayers; i++)
            delete papoLayers[i];
        CPLFree(papoLayers);
        papoLayers = nullptr;
        nLayers = 0;

        if (poReader)
        {
            if (bOutIsTempFile)
                VSIUnlink(poReader->GetSourceFileName());
            delete poReader;
            poReader = nullptr;
        }

        delete poStoredGMLFeature;
        poStoredGMLFeature = nullptr;

        if (m_bUnlinkXSDFilename)
        {
            VSIUnlink(osXSDFilename);
            m_bUnlinkXSDFilename = false;
        }

        if (m_bWriteError)
            eErr = CE_Failure;
    }
    return eErr;
}

/************************************************************************/
/*                            CheckHeader()                             */
/************************************************************************/

bool OGRGMLDataSource::CheckHeader(const char *pszStr)
{
    if (strstr(pszStr, "<wfs:FeatureCollection ") != nullptr)
        return true;

    if (strstr(pszStr, "opengis.net/gml") == nullptr &&
        strstr(pszStr, "<csw:GetRecordsResponse") == nullptr)
    {
        return false;
    }

    // Ignore kml files
    if (strstr(pszStr, "<kml") != nullptr)
    {
        return false;
    }

    // Ignore .xsd schemas.
    if (strstr(pszStr, "<schema") != nullptr ||
        strstr(pszStr, "<xs:schema") != nullptr ||
        strstr(pszStr, "<xsd:schema") != nullptr)
    {
        return false;
    }

    // Ignore GeoRSS documents. They will be recognized by the GeoRSS driver.
    if (strstr(pszStr, "<rss") != nullptr &&
        strstr(pszStr, "xmlns:georss") != nullptr)
    {
        return false;
    }

    // Ignore OpenJUMP .jml documents.
    // They will be recognized by the OpenJUMP driver.
    if (strstr(pszStr, "<JCSDataFile") != nullptr)
    {
        return false;
    }

    // Ignore OGR WFS xml description files, or WFS Capabilities results.
    if (strstr(pszStr, "<OGRWFSDataSource>") != nullptr ||
        strstr(pszStr, "<wfs:WFS_Capabilities") != nullptr)
    {
        return false;
    }

    // Ignore WMTS capabilities results.
    if (strstr(pszStr, "http://www.opengis.net/wmts/1.0") != nullptr)
    {
        return false;
    }

    return true;
}

/************************************************************************/
/*                          ExtractSRSName()                            */
/************************************************************************/

static bool ExtractSRSName(const char *pszXML, char *szSRSName,
                           size_t sizeof_szSRSName)
{
    szSRSName[0] = '\0';

    const char *pszSRSName = strstr(pszXML, "srsName=\"");
    if (pszSRSName != nullptr)
    {
        pszSRSName += 9;
        const char *pszEndQuote = strchr(pszSRSName, '"');
        if (pszEndQuote != nullptr &&
            static_cast<size_t>(pszEndQuote - pszSRSName) < sizeof_szSRSName)
        {
            memcpy(szSRSName, pszSRSName, pszEndQuote - pszSRSName);
            szSRSName[pszEndQuote - pszSRSName] = '\0';
            return true;
        }
    }
    return false;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

bool OGRGMLDataSource::Open(GDALOpenInfo *poOpenInfo)

{
    // Extract XSD filename from connection string if present.
    osFilename = poOpenInfo->pszFilename;
    const char *pszXSDFilenameTmp = strstr(poOpenInfo->pszFilename, ",xsd=");
    if (pszXSDFilenameTmp != nullptr)
    {
        osFilename.resize(pszXSDFilenameTmp - poOpenInfo->pszFilename);
        osXSDFilename = pszXSDFilenameTmp + strlen(",xsd=");
    }
    else
    {
        osXSDFilename =
            CSLFetchNameValueDef(poOpenInfo->papszOpenOptions, "XSD", "");
    }

    const char *pszFilename = osFilename.c_str();

    // Open the source file.
    VSILFILE *fpToClose = nullptr;
    VSILFILE *fp = nullptr;
    if (poOpenInfo->fpL != nullptr)
    {
        fp = poOpenInfo->fpL;
        VSIFSeekL(fp, 0, SEEK_SET);
    }
    else
    {
        fp = VSIFOpenL(pszFilename, "r");
        if (fp == nullptr)
            return false;
        fpToClose = fp;
    }

    // Load a header chunk and check for signs it is GML.
    char szHeader[4096] = {};
    size_t nRead = VSIFReadL(szHeader, 1, sizeof(szHeader) - 1, fp);
    if (nRead == 0)
    {
        if (fpToClose)
            VSIFCloseL(fpToClose);
        return false;
    }
    szHeader[nRead] = '\0';

    CPLString osWithVsiGzip;

    // Might be a OS-Mastermap gzipped GML, so let be nice and try to open
    // it transparently with /vsigzip/.
    if (static_cast<GByte>(szHeader[0]) == 0x1f &&
        static_cast<GByte>(szHeader[1]) == 0x8b &&
        EQUAL(CPLGetExtensionSafe(pszFilename).c_str(), "gz") &&
        !STARTS_WITH(pszFilename, "/vsigzip/"))
    {
        if (fpToClose)
            VSIFCloseL(fpToClose);
        fpToClose = nullptr;
        osWithVsiGzip = "/vsigzip/";
        osWithVsiGzip += pszFilename;

        pszFilename = osWithVsiGzip;

        fp = fpToClose = VSIFOpenL(pszFilename, "r");
        if (fp == nullptr)
            return false;

        nRead = VSIFReadL(szHeader, 1, sizeof(szHeader) - 1, fp);
        if (nRead == 0)
        {
            VSIFCloseL(fpToClose);
            return false;
        }
        szHeader[nRead] = '\0';
    }

    // Check for a UTF-8 BOM and skip if found.

    // TODO: BOM is variable-length parameter and depends on encoding. */
    // Add BOM detection for other encodings.

    // Used to skip to actual beginning of XML data
    char *szPtr = szHeader;

    if ((static_cast<unsigned char>(szHeader[0]) == 0xEF) &&
        (static_cast<unsigned char>(szHeader[1]) == 0xBB) &&
        (static_cast<unsigned char>(szHeader[2]) == 0xBF))
    {
        szPtr += 3;
    }

    bool bExpatCompatibleEncoding = false;

    const char *pszEncoding = strstr(szPtr, "encoding=");
    if (pszEncoding)
        bExpatCompatibleEncoding =
            (pszEncoding[9] == '\'' || pszEncoding[9] == '"') &&
            (STARTS_WITH_CI(pszEncoding + 10, "UTF-8") ||
             STARTS_WITH_CI(pszEncoding + 10, "ISO-8859-15") ||
             (STARTS_WITH_CI(pszEncoding + 10, "ISO-8859-1") &&
              pszEncoding[20] == pszEncoding[9]));
    else
        bExpatCompatibleEncoding = true;  // utf-8 is the default.

    const bool bHas3D = strstr(szPtr, "srsDimension=\"3\"") != nullptr ||
                        strstr(szPtr, "<gml:Z>") != nullptr;

    // Here, we expect the opening chevrons of GML tree root element.
    if (szPtr[0] != '<' || !CheckHeader(szPtr))
    {
        if (fpToClose)
            VSIFCloseL(fpToClose);
        return false;
    }

    // Now we definitely own the file descriptor.
    if (fp == poOpenInfo->fpL)
        poOpenInfo->fpL = nullptr;

    // Small optimization: if we parse a <wfs:FeatureCollection> and
    // that numberOfFeatures is set, we can use it to set the FeatureCount
    // but *ONLY* if there's just one class.
    const char *pszFeatureCollection = strstr(szPtr, "wfs:FeatureCollection");
    if (pszFeatureCollection == nullptr)
        // GML 3.2.1 output.
        pszFeatureCollection = strstr(szPtr, "gml:FeatureCollection");
    if (pszFeatureCollection == nullptr)
    {
        // Deegree WFS 1.0.0 output.
        pszFeatureCollection = strstr(szPtr, "<FeatureCollection");
        if (pszFeatureCollection &&
            strstr(szPtr, "xmlns:wfs=\"http://www.opengis.net/wfs\"") ==
                nullptr)
            pszFeatureCollection = nullptr;
    }

    GIntBig nNumberOfFeatures = 0;
    if (pszFeatureCollection)
    {
        bExposeGMLId = true;
        bIsWFS = true;
        const char *pszNumberOfFeatures = strstr(szPtr, "numberOfFeatures=");
        if (pszNumberOfFeatures)
        {
            pszNumberOfFeatures += 17;
            char ch = pszNumberOfFeatures[0];
            if ((ch == '\'' || ch == '"') &&
                strchr(pszNumberOfFeatures + 1, ch) != nullptr)
            {
                nNumberOfFeatures = CPLAtoGIntBig(pszNumberOfFeatures + 1);
            }
        }
        else if ((pszNumberOfFeatures = strstr(szPtr, "numberReturned=")) !=
                 nullptr)
        {
            // WFS 2.0.0
            pszNumberOfFeatures += 15;
            char ch = pszNumberOfFeatures[0];
            if ((ch == '\'' || ch == '"') &&
                strchr(pszNumberOfFeatures + 1, ch) != nullptr)
            {
                // 'unknown' might be a valid value in a corrected version of
                // WFS 2.0 but it will also evaluate to 0, that is considered as
                // unknown, so nothing particular to do.
                nNumberOfFeatures = CPLAtoGIntBig(pszNumberOfFeatures + 1);
            }
        }
    }
    else if (STARTS_WITH(pszFilename, "/vsimem/") &&
             strstr(pszFilename, "_ogr_wfs_"))
    {
        // http://regis.intergraph.com/wfs/dcmetro/request.asp? returns a
        // <G:FeatureCollection> Who knows what servers can return?  When
        // in the context of the WFS driver always expose the gml:id to avoid
        // later crashes.
        bExposeGMLId = true;
        bIsWFS = true;
    }
    else
    {
        bExposeGMLId = strstr(szPtr, " gml:id=\"") != nullptr ||
                       strstr(szPtr, " gml:id='") != nullptr;
        bExposeFid = strstr(szPtr, " fid=\"") != nullptr ||
                     strstr(szPtr, " fid='") != nullptr;
    }

    const char *pszExposeGMLId =
        CSLFetchNameValueDef(poOpenInfo->papszOpenOptions, "EXPOSE_GML_ID",
                             CPLGetConfigOption("GML_EXPOSE_GML_ID", nullptr));
    if (pszExposeGMLId)
        bExposeGMLId = CPLTestBool(pszExposeGMLId);

    const char *pszExposeFid =
        CSLFetchNameValueDef(poOpenInfo->papszOpenOptions, "EXPOSE_FID",
                             CPLGetConfigOption("GML_EXPOSE_FID", nullptr));

    if (pszExposeFid)
        bExposeFid = CPLTestBool(pszExposeFid);

    const bool bHintConsiderEPSGAsURN =
        strstr(szPtr, "xmlns:fme=\"http://www.safe.com/gml/fme\"") != nullptr;

    char szSRSName[128] = {};

    // MTKGML.
    if (strstr(szPtr, "<Maastotiedot") != nullptr)
    {
        if (strstr(szPtr,
                   "http://xml.nls.fi/XML/Namespace/"
                   "Maastotietojarjestelma/SiirtotiedostonMalli/2011-02") ==
            nullptr)
            CPLDebug("GML", "Warning: a MTKGML file was detected, "
                            "but its namespace is unknown");
        bUseGlobalSRSName = true;
        if (!ExtractSRSName(szPtr, szSRSName, sizeof(szSRSName)))
            strcpy(szSRSName, "EPSG:3067");
    }

    const char *pszSchemaLocation = strstr(szPtr, "schemaLocation=");
    if (pszSchemaLocation)
        pszSchemaLocation += strlen("schemaLocation=");

    bool bCheckAuxFile = true;
    if (STARTS_WITH(pszFilename, "/vsicurl_streaming/"))
        bCheckAuxFile = false;
    else if (STARTS_WITH(pszFilename, "/vsicurl/") &&
             (strstr(pszFilename, "?SERVICE=") ||
              strstr(pszFilename, "&SERVICE=")))
        bCheckAuxFile = false;

    bool bIsWFSJointLayer = bIsWFS && strstr(szPtr, "<wfs:Tuple>");
    if (bIsWFSJointLayer)
        bExposeGMLId = false;

    // We assume now that it is GML.  Instantiate a GMLReader on it.
    const char *pszReadMode =
        CSLFetchNameValueDef(poOpenInfo->papszOpenOptions, "READ_MODE",
                             CPLGetConfigOption("GML_READ_MODE", "AUTO"));
    if (EQUAL(pszReadMode, "AUTO"))
        pszReadMode = nullptr;
    if (pszReadMode == nullptr || EQUAL(pszReadMode, "STANDARD"))
        eReadMode = STANDARD;
    else if (EQUAL(pszReadMode, "SEQUENTIAL_LAYERS"))
        eReadMode = SEQUENTIAL_LAYERS;
    else if (EQUAL(pszReadMode, "INTERLEAVED_LAYERS"))
        eReadMode = INTERLEAVED_LAYERS;
    else
    {
        CPLDebug("GML",
                 "Unrecognized value for GML_READ_MODE configuration option.");
    }

    m_bInvertAxisOrderIfLatLong = CPLTestBool(CSLFetchNameValueDef(
        poOpenInfo->papszOpenOptions, "INVERT_AXIS_ORDER_IF_LAT_LONG",
        CPLGetConfigOption("GML_INVERT_AXIS_ORDER_IF_LAT_LONG", "YES")));

    const char *pszConsiderEPSGAsURN = CSLFetchNameValueDef(
        poOpenInfo->papszOpenOptions, "CONSIDER_EPSG_AS_URN",
        CPLGetConfigOption("GML_CONSIDER_EPSG_AS_URN", "AUTO"));
    if (!EQUAL(pszConsiderEPSGAsURN, "AUTO"))
        m_bConsiderEPSGAsURN = CPLTestBool(pszConsiderEPSGAsURN);
    else if (bHintConsiderEPSGAsURN)
    {
        // GML produced by FME (at least CanVec GML) seem to honour EPSG axis
        // ordering.
        CPLDebug("GML", "FME-produced GML --> "
                        "consider that GML_CONSIDER_EPSG_AS_URN is set to YES");
        m_bConsiderEPSGAsURN = true;
    }
    else
    {
        m_bConsiderEPSGAsURN = false;
    }

    const char *pszSwapCoordinates = CSLFetchNameValueDef(
        poOpenInfo->papszOpenOptions, "SWAP_COORDINATES",
        CPLGetConfigOption("GML_SWAP_COORDINATES", "AUTO"));
    m_eSwapCoordinates = EQUAL(pszSwapCoordinates, "AUTO") ? GML_SWAP_AUTO
                         : CPLTestBool(pszSwapCoordinates) ? GML_SWAP_YES
                                                           : GML_SWAP_NO;

    m_bGetSecondaryGeometryOption =
        CPLTestBool(CPLGetConfigOption("GML_GET_SECONDARY_GEOM", "NO"));

    // EXPAT is faster than Xerces, so when it is safe to use it, use it!
    // The only interest of Xerces is for rare encodings that Expat doesn't
    // handle, but UTF-8 is well handled by Expat.
    bool bUseExpatParserPreferably = bExpatCompatibleEncoding;

    // Override default choice.
    const char *pszGMLParser = CPLGetConfigOption("GML_PARSER", nullptr);
    if (pszGMLParser)
    {
        if (EQUAL(pszGMLParser, "EXPAT"))
            bUseExpatParserPreferably = true;
        else if (EQUAL(pszGMLParser, "XERCES"))
            bUseExpatParserPreferably = false;
    }

    poReader =
        CreateGMLReader(bUseExpatParserPreferably, m_bInvertAxisOrderIfLatLong,
                        m_bConsiderEPSGAsURN, m_eSwapCoordinates,
                        m_bGetSecondaryGeometryOption);
    if (poReader == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "File %s appears to be GML but the GML reader can't\n"
                 "be instantiated, likely because Xerces or Expat support was\n"
                 "not configured in.",
                 pszFilename);
        VSIFCloseL(fp);
        return false;
    }

    poReader->SetSourceFile(pszFilename);
    auto poGMLReader = cpl::down_cast<GMLReader *>(poReader);
    poGMLReader->SetIsWFSJointLayer(bIsWFSJointLayer);
    bEmptyAsNull =
        CPLFetchBool(poOpenInfo->papszOpenOptions, "EMPTY_AS_NULL", true);
    poGMLReader->SetEmptyAsNull(bEmptyAsNull);
    poGMLReader->SetReportAllAttributes(CPLFetchBool(
        poOpenInfo->papszOpenOptions, "GML_ATTRIBUTES_TO_OGR_FIELDS",
        CPLTestBool(CPLGetConfigOption("GML_ATTRIBUTES_TO_OGR_FIELDS", "NO"))));
    poGMLReader->SetUseBBOX(
        CPLFetchBool(poOpenInfo->papszOpenOptions, "USE_BBOX", false));

    // Find <gml:description>, <gml:name> and <gml:boundedBy> and if it is
    // a standalone geometry
    // Also look for <gml:description>, <gml:identifier> and <gml:name> inside
    // a feature
    FindAndParseTopElements(fp);

    if (m_poStandaloneGeom)
    {
        papoLayers = static_cast<OGRLayer **>(CPLMalloc(sizeof(OGRLayer *)));
        nLayers = 1;
        auto poLayer = new OGRMemLayer(
            "geometry",
            m_oStandaloneGeomSRS.IsEmpty() ? nullptr : &m_oStandaloneGeomSRS,
            m_poStandaloneGeom->getGeometryType());
        papoLayers[0] = poLayer;
        OGRFeature *poFeature = new OGRFeature(poLayer->GetLayerDefn());
        poFeature->SetGeometryDirectly(m_poStandaloneGeom.release());
        CPL_IGNORE_RET_VAL(poLayer->CreateFeature(poFeature));
        delete poFeature;
        poLayer->SetUpdatable(false);
        VSIFCloseL(fp);
        return true;
    }

    if (szSRSName[0] != '\0')
        poReader->SetGlobalSRSName(szSRSName);

    const bool bIsWFSFromServer =
        CPLString(pszFilename).ifind("SERVICE=WFS") != std::string::npos;

    // Resolve the xlinks in the source file and save it with the
    // extension ".resolved.gml". The source file will to set to that.
    char *pszXlinkResolvedFilename = nullptr;
    const char *pszOption = CPLGetConfigOption("GML_SAVE_RESOLVED_TO", nullptr);
    bool bResolve = true;
    bool bHugeFile = false;
    if (bIsWFSFromServer ||
        (pszOption != nullptr && STARTS_WITH_CI(pszOption, "SAME")))
    {
        // "SAME" will overwrite the existing gml file.
        pszXlinkResolvedFilename = CPLStrdup(pszFilename);
    }
    else if (pszOption != nullptr && CPLStrnlen(pszOption, 5) >= 5 &&
             STARTS_WITH_CI(pszOption - 4 + strlen(pszOption), ".gml"))
    {
        // Any string ending with ".gml" will try and write to it.
        pszXlinkResolvedFilename = CPLStrdup(pszOption);
    }
    else
    {
        // When no option is given or is not recognised,
        // use the same file name with the extension changed to .resolved.gml
        pszXlinkResolvedFilename = CPLStrdup(
            CPLResetExtensionSafe(pszFilename, "resolved.gml").c_str());

        // Check if the file already exists.
        VSIStatBufL sResStatBuf, sGMLStatBuf;
        if (bCheckAuxFile &&
            VSIStatL(pszXlinkResolvedFilename, &sResStatBuf) == 0)
        {
            if (VSIStatL(pszFilename, &sGMLStatBuf) == 0 &&
                sGMLStatBuf.st_mtime > sResStatBuf.st_mtime)
            {
                CPLDebug("GML",
                         "Found %s but ignoring because it appears\n"
                         "be older than the associated GML file.",
                         pszXlinkResolvedFilename);
            }
            else
            {
                poReader->SetSourceFile(pszXlinkResolvedFilename);
                bResolve = false;
            }
        }
    }

    const char *pszSkipOption = CSLFetchNameValueDef(
        poOpenInfo->papszOpenOptions, "SKIP_RESOLVE_ELEMS",
        CPLGetConfigOption("GML_SKIP_RESOLVE_ELEMS", "ALL"));

    char **papszSkip = nullptr;
    if (EQUAL(pszSkipOption, "ALL"))
        bResolve = false;
    else if (EQUAL(pszSkipOption, "HUGE"))
        // Exactly as NONE, but intended for HUGE files
        bHugeFile = true;
    else if (!EQUAL(pszSkipOption, "NONE"))  // Use this to resolve everything.
        papszSkip = CSLTokenizeString2(
            pszSkipOption, ",", CSLT_STRIPLEADSPACES | CSLT_STRIPENDSPACES);
    bool bHaveSchema = false;
    bool bSchemaDone = false;

    // Is some GML Feature Schema (.gfs) TEMPLATE required?
    const char *pszGFSTemplateName =
        CSLFetchNameValueDef(poOpenInfo->papszOpenOptions, "GFS_TEMPLATE",
                             CPLGetConfigOption("GML_GFS_TEMPLATE", nullptr));
    if (pszGFSTemplateName != nullptr)
    {
        // Attempt to load the GFS TEMPLATE.
        bHaveSchema = poReader->LoadClasses(pszGFSTemplateName);
    }

    if (bResolve)
    {
        if (bHugeFile)
        {
            bSchemaDone = true;
            bool bSqliteIsTempFile =
                CPLTestBool(CPLGetConfigOption("GML_HUGE_TEMPFILE", "YES"));
            int iSqliteCacheMB =
                atoi(CPLGetConfigOption("OGR_SQLITE_CACHE", "0"));
            if (poReader->HugeFileResolver(pszXlinkResolvedFilename,
                                           bSqliteIsTempFile,
                                           iSqliteCacheMB) == false)
            {
                // Assume an error has been reported.
                VSIFCloseL(fp);
                CPLFree(pszXlinkResolvedFilename);
                return false;
            }
        }
        else
        {
            poReader->ResolveXlinks(pszXlinkResolvedFilename, &bOutIsTempFile,
                                    papszSkip);
        }
    }

    CPLFree(pszXlinkResolvedFilename);
    pszXlinkResolvedFilename = nullptr;
    CSLDestroy(papszSkip);
    papszSkip = nullptr;

    // If the source filename for the reader is still the GML filename, then
    // we can directly provide the file pointer. Otherwise close it.
    if (strcmp(poReader->GetSourceFileName(), pszFilename) == 0)
        poReader->SetFP(fp);
    else
        VSIFCloseL(fp);
    fp = nullptr;

    // Is a prescan required?
    if (bHaveSchema && !bSchemaDone)
    {
        // We must detect which layers are actually present in the .gml
        // and how many features they have.
        if (!poReader->PrescanForTemplate())
        {
            // Assume an error has been reported.
            return false;
        }
    }

    CPLString osGFSFilename;
    if (!bIsWFSFromServer)
    {
        osGFSFilename = CPLResetExtensionSafe(pszFilename, "gfs");
        if (STARTS_WITH(osGFSFilename, "/vsigzip/"))
            osGFSFilename = osGFSFilename.substr(strlen("/vsigzip/"));
    }

    // Can we find a GML Feature Schema (.gfs) for the input file?
    if (!osGFSFilename.empty() && !bHaveSchema && !bSchemaDone &&
        osXSDFilename.empty())
    {
        VSIStatBufL sGFSStatBuf;
        if (bCheckAuxFile && VSIStatL(osGFSFilename, &sGFSStatBuf) == 0)
        {
            VSIStatBufL sGMLStatBuf;
            if (VSIStatL(pszFilename, &sGMLStatBuf) == 0 &&
                sGMLStatBuf.st_mtime > sGFSStatBuf.st_mtime)
            {
                CPLDebug("GML",
                         "Found %s but ignoring because it appears\n"
                         "be older than the associated GML file.",
                         osGFSFilename.c_str());
            }
            else
            {
                bHaveSchema = poReader->LoadClasses(osGFSFilename);
                if (bHaveSchema)
                {
                    const std::string osXSDFilenameTmp =
                        CPLResetExtensionSafe(pszFilename, "xsd");
                    if (VSIStatExL(osXSDFilenameTmp.c_str(), &sGMLStatBuf,
                                   VSI_STAT_EXISTS_FLAG) == 0)
                    {
                        CPLDebug("GML", "Using %s file, ignoring %s",
                                 osGFSFilename.c_str(),
                                 osXSDFilenameTmp.c_str());
                    }
                }
            }
        }
    }

    // Can we find an xsd which might conform to the GML3 Level 0
    // profile?  We really ought to look for it based on the rules
    // schemaLocation in the GML feature collection but for now we
    // just hopes it is in the same director with the same name.

    bool bHasFoundXSD = false;

    if (!bHaveSchema)
    {
        char **papszTypeNames = nullptr;

        VSIStatBufL sXSDStatBuf;
        if (osXSDFilename.empty())
        {
            osXSDFilename = CPLResetExtensionSafe(pszFilename, "xsd");
            if (bCheckAuxFile && VSIStatExL(osXSDFilename, &sXSDStatBuf,
                                            VSI_STAT_EXISTS_FLAG) == 0)
            {
                bHasFoundXSD = true;
            }
        }
        else
        {
            if (STARTS_WITH(osXSDFilename, "http://") ||
                STARTS_WITH(osXSDFilename, "https://") ||
                VSIStatExL(osXSDFilename, &sXSDStatBuf, VSI_STAT_EXISTS_FLAG) ==
                    0)
            {
                bHasFoundXSD = true;
            }
        }

        // If not found, try if there is a schema in the gml_registry.xml
        // that might match a declared namespace and featuretype.
        if (!bHasFoundXSD)
        {
            GMLRegistry oRegistry(
                CSLFetchNameValueDef(poOpenInfo->papszOpenOptions, "REGISTRY",
                                     CPLGetConfigOption("GML_REGISTRY", "")));
            if (oRegistry.Parse())
            {
                CPLString osHeader(szHeader);
                for (size_t iNS = 0; iNS < oRegistry.aoNamespaces.size(); iNS++)
                {
                    GMLRegistryNamespace &oNamespace =
                        oRegistry.aoNamespaces[iNS];
                    // When namespace is omitted or fit with case sensitive
                    // match for name space prefix, then go next to find feature
                    // match.
                    //
                    // Case sensitive comparison since below test that also
                    // uses the namespace prefix is case sensitive.
                    if (!oNamespace.osPrefix.empty() &&
                        osHeader.find(CPLSPrintf(
                            "xmlns:%s", oNamespace.osPrefix.c_str())) ==
                            std::string::npos)
                    {
                        // namespace does not match with one of registry
                        // definition. go to next entry.
                        continue;
                    }

                    const char *pszURIToFind =
                        CPLSPrintf("\"%s\"", oNamespace.osURI.c_str());
                    if (strstr(szHeader, pszURIToFind) != nullptr)
                    {
                        if (oNamespace.bUseGlobalSRSName)
                            bUseGlobalSRSName = true;

                        for (size_t iTypename = 0;
                             iTypename < oNamespace.aoFeatureTypes.size();
                             iTypename++)
                        {
                            const char *pszElementToFind = nullptr;

                            GMLRegistryFeatureType &oFeatureType =
                                oNamespace.aoFeatureTypes[iTypename];

                            if (!oNamespace.osPrefix.empty())
                            {
                                if (!oFeatureType.osElementValue.empty())
                                    pszElementToFind = CPLSPrintf(
                                        "%s:%s>%s", oNamespace.osPrefix.c_str(),
                                        oFeatureType.osElementName.c_str(),
                                        oFeatureType.osElementValue.c_str());
                                else
                                    pszElementToFind = CPLSPrintf(
                                        "%s:%s", oNamespace.osPrefix.c_str(),
                                        oFeatureType.osElementName.c_str());
                            }
                            else
                            {
                                if (!oFeatureType.osElementValue.empty())
                                    pszElementToFind = CPLSPrintf(
                                        "%s>%s",
                                        oFeatureType.osElementName.c_str(),
                                        oFeatureType.osElementValue.c_str());
                                else
                                    pszElementToFind = CPLSPrintf(
                                        "<%s",
                                        oFeatureType.osElementName.c_str());
                            }

                            // Case sensitive test since in a CadastralParcel
                            // feature there is a property basicPropertyUnit
                            // xlink, not to be confused with a top-level
                            // BasicPropertyUnit feature.
                            if (osHeader.find(pszElementToFind) !=
                                std::string::npos)
                            {
                                if (!oFeatureType.osSchemaLocation.empty())
                                {
                                    osXSDFilename =
                                        oFeatureType.osSchemaLocation;
                                    if (STARTS_WITH(osXSDFilename, "http://") ||
                                        STARTS_WITH(osXSDFilename,
                                                    "https://") ||
                                        VSIStatExL(osXSDFilename, &sXSDStatBuf,
                                                   VSI_STAT_EXISTS_FLAG) == 0)
                                    {
                                        bHasFoundXSD = true;
                                        bHaveSchema = true;
                                        CPLDebug(
                                            "GML",
                                            "Found %s for %s:%s in registry",
                                            osXSDFilename.c_str(),
                                            oNamespace.osPrefix.c_str(),
                                            oFeatureType.osElementName.c_str());
                                    }
                                    else
                                    {
                                        CPLDebug("GML", "Cannot open %s",
                                                 osXSDFilename.c_str());
                                    }
                                }
                                else
                                {
                                    bHaveSchema = poReader->LoadClasses(
                                        oFeatureType.osGFSSchemaLocation);
                                    if (bHaveSchema)
                                    {
                                        CPLDebug(
                                            "GML",
                                            "Found %s for %s:%s in registry",
                                            oFeatureType.osGFSSchemaLocation
                                                .c_str(),
                                            oNamespace.osPrefix.c_str(),
                                            oFeatureType.osElementName.c_str());
                                    }
                                }
                                break;
                            }
                        }
                        break;
                    }
                }
            }
        }

        /* For WFS, try to fetch the application schema */
        if (bIsWFS && !bHaveSchema && pszSchemaLocation != nullptr &&
            (pszSchemaLocation[0] == '\'' || pszSchemaLocation[0] == '"') &&
            strchr(pszSchemaLocation + 1, pszSchemaLocation[0]) != nullptr)
        {
            char *pszSchemaLocationTmp1 = CPLStrdup(pszSchemaLocation + 1);
            int nTruncLen = static_cast<int>(
                strchr(pszSchemaLocation + 1, pszSchemaLocation[0]) -
                (pszSchemaLocation + 1));
            pszSchemaLocationTmp1[nTruncLen] = '\0';
            char *pszSchemaLocationTmp2 =
                CPLUnescapeString(pszSchemaLocationTmp1, nullptr, CPLES_XML);
            CPLString osEscaped =
                ReplaceSpaceByPct20IfNeeded(pszSchemaLocationTmp2);
            CPLFree(pszSchemaLocationTmp2);
            pszSchemaLocationTmp2 = CPLStrdup(osEscaped);
            if (pszSchemaLocationTmp2)
            {
                // pszSchemaLocationTmp2 is of the form:
                // http://namespace1 http://namespace1_schema_location
                // http://namespace2 http://namespace1_schema_location2 So we
                // try to find http://namespace1_schema_location that contains
                // hints that it is the WFS application */ schema, i.e. if it
                // contains typename= and request=DescribeFeatureType.
                char **papszTokens =
                    CSLTokenizeString2(pszSchemaLocationTmp2, " \r\n", 0);
                int nTokens = CSLCount(papszTokens);
                if ((nTokens % 2) == 0)
                {
                    for (int i = 0; i < nTokens; i += 2)
                    {
                        const char *pszEscapedURL = papszTokens[i + 1];
                        char *pszLocation = CPLUnescapeString(
                            pszEscapedURL, nullptr, CPLES_URL);
                        CPLString osLocation = pszLocation;
                        CPLFree(pszLocation);
                        if (osLocation.ifind("typename=") !=
                                std::string::npos &&
                            osLocation.ifind("request=DescribeFeatureType") !=
                                std::string::npos)
                        {
                            CPLString osTypeName =
                                CPLURLGetValue(osLocation, "typename");
                            papszTypeNames =
                                CSLTokenizeString2(osTypeName, ",", 0);

                            // Old non-documented way
                            const char *pszGML_DOWNLOAD_WFS_SCHEMA =
                                CPLGetConfigOption("GML_DOWNLOAD_WFS_SCHEMA",
                                                   nullptr);
                            if (pszGML_DOWNLOAD_WFS_SCHEMA)
                            {
                                CPLError(
                                    CE_Warning, CPLE_AppDefined,
                                    "Configuration option "
                                    "GML_DOWNLOAD_WFS_SCHEMA is deprecated. "
                                    "Please use GML_DOWNLOAD_SCHEMA instead of "
                                    "the DOWNLOAD_SCHEMA open option");
                            }
                            else
                            {
                                pszGML_DOWNLOAD_WFS_SCHEMA = "YES";
                            }
                            if (!bHasFoundXSD && CPLHTTPEnabled() &&
                                CPLFetchBool(poOpenInfo->papszOpenOptions,
                                             "DOWNLOAD_SCHEMA",
                                             CPLTestBool(CPLGetConfigOption(
                                                 "GML_DOWNLOAD_SCHEMA",
                                                 pszGML_DOWNLOAD_WFS_SCHEMA))))
                            {
                                CPLHTTPResult *psResult =
                                    CPLHTTPFetch(pszEscapedURL, nullptr);
                                if (psResult)
                                {
                                    if (psResult->nStatus == 0 &&
                                        psResult->pabyData != nullptr)
                                    {
                                        bHasFoundXSD = true;
                                        m_bUnlinkXSDFilename = true;
                                        osXSDFilename =
                                            VSIMemGenerateHiddenFilename(
                                                "tmp_ogr_gml.xsd");
                                        VSILFILE *fpMem = VSIFileFromMemBuffer(
                                            osXSDFilename, psResult->pabyData,
                                            psResult->nDataLen, TRUE);
                                        VSIFCloseL(fpMem);
                                        psResult->pabyData = nullptr;
                                    }
                                    CPLHTTPDestroyResult(psResult);
                                }
                            }
                            break;
                        }
                    }
                }
                CSLDestroy(papszTokens);
            }
            CPLFree(pszSchemaLocationTmp2);
            CPLFree(pszSchemaLocationTmp1);
        }

        bool bHasFeatureProperties = false;
        if (bHasFoundXSD)
        {
            std::vector<GMLFeatureClass *> aosClasses;
            bool bUseSchemaImports = CPLFetchBool(
                poOpenInfo->papszOpenOptions, "USE_SCHEMA_IMPORT",
                CPLTestBool(CPLGetConfigOption("GML_USE_SCHEMA_IMPORT", "NO")));
            bool bFullyUnderstood = false;
            bHaveSchema = GMLParseXSD(osXSDFilename, bUseSchemaImports,
                                      aosClasses, bFullyUnderstood);

            if (bHaveSchema && !bFullyUnderstood && bIsWFSJointLayer)
            {
                CPLDebug("GML", "Schema found, but only partially understood. "
                                "Cannot be used in a WFS join context");

                std::vector<GMLFeatureClass *>::const_iterator oIter =
                    aosClasses.begin();
                std::vector<GMLFeatureClass *>::const_iterator oEndIter =
                    aosClasses.end();
                while (oIter != oEndIter)
                {
                    GMLFeatureClass *poClass = *oIter;

                    delete poClass;
                    ++oIter;
                }
                aosClasses.resize(0);
                bHaveSchema = false;
            }

            if (bHaveSchema)
            {
                CPLDebug("GML", "Using %s", osXSDFilename.c_str());
                std::vector<GMLFeatureClass *>::const_iterator oIter =
                    aosClasses.begin();
                std::vector<GMLFeatureClass *>::const_iterator oEndIter =
                    aosClasses.end();
                while (oIter != oEndIter)
                {
                    GMLFeatureClass *poClass = *oIter;

                    if (poClass->HasFeatureProperties())
                    {
                        bHasFeatureProperties = true;
                        break;
                    }
                    ++oIter;
                }

                oIter = aosClasses.begin();
                while (oIter != oEndIter)
                {
                    GMLFeatureClass *poClass = *oIter;
                    ++oIter;

                    // We have no way of knowing if the geometry type is 25D
                    // when examining the xsd only, so if there was a hint
                    // it is, we force to 25D.
                    if (bHas3D && poClass->GetGeometryPropertyCount() == 1)
                    {
                        poClass->GetGeometryProperty(0)->SetType(
                            wkbSetZ(static_cast<OGRwkbGeometryType>(
                                poClass->GetGeometryProperty(0)->GetType())));
                    }

                    bool bAddClass = true;
                    // If typenames are declared, only register the matching
                    // classes, in case the XSD contains more layers, but not if
                    // feature classes contain feature properties, in which case
                    // we will have embedded features that will be reported as
                    // top-level features.
                    if (papszTypeNames != nullptr && !bHasFeatureProperties)
                    {
                        bAddClass = false;
                        char **papszIter = papszTypeNames;
                        while (*papszIter && !bAddClass)
                        {
                            const char *pszTypeName = *papszIter;
                            if (strcmp(pszTypeName, poClass->GetName()) == 0)
                                bAddClass = true;
                            papszIter++;
                        }

                        // Retry by removing prefixes.
                        if (!bAddClass)
                        {
                            papszIter = papszTypeNames;
                            while (*papszIter && !bAddClass)
                            {
                                const char *pszTypeName = *papszIter;
                                const char *pszColon = strchr(pszTypeName, ':');
                                if (pszColon)
                                {
                                    pszTypeName = pszColon + 1;
                                    if (strcmp(pszTypeName,
                                               poClass->GetName()) == 0)
                                    {
                                        poClass->SetName(pszTypeName);
                                        bAddClass = true;
                                    }
                                }
                                papszIter++;
                            }
                        }
                    }

                    if (bAddClass &&
                        poReader->GetClass(poClass->GetName()) == nullptr)
                    {
                        poReader->AddClass(poClass);
                    }
                    else
                        delete poClass;
                }

                poReader->SetClassListLocked(true);
            }
        }

        if (bHaveSchema && bIsWFS)
        {
            if (bIsWFSJointLayer)
            {
                BuildJointClassFromXSD();
            }

            // For WFS, we can assume sequential layers.
            if (poReader->GetClassCount() > 1 && pszReadMode == nullptr &&
                !bHasFeatureProperties)
            {
                CPLDebug("GML",
                         "WFS output. Using SEQUENTIAL_LAYERS read mode");
                eReadMode = SEQUENTIAL_LAYERS;
            }
            // Sometimes the returned schema contains only <xs:include> that we
            // don't resolve so ignore it.
            else if (poReader->GetClassCount() == 0)
                bHaveSchema = false;
        }

        CSLDestroy(papszTypeNames);
    }

    // Force a first pass to establish the schema.  Eventually we will have
    // mechanisms for remembering the schema and related information.
    const char *pszForceSRSDetection =
        CSLFetchNameValue(poOpenInfo->papszOpenOptions, "FORCE_SRS_DETECTION");
    if (!bHaveSchema ||
        (pszForceSRSDetection && CPLTestBool(pszForceSRSDetection)))
    {
        bool bOnlyDetectSRS = bHaveSchema;
        if (!poReader->PrescanForSchema(true, bOnlyDetectSRS))
        {
            // Assume an error was reported.
            return false;
        }
        if (!bHaveSchema)
        {
            if (bIsWFSJointLayer && poReader->GetClassCount() == 1)
            {
                BuildJointClassFromScannedSchema();
            }

            if (bHasFoundXSD)
            {
                CPLDebug("GML", "Generating %s file, ignoring %s",
                         osGFSFilename.c_str(), osXSDFilename.c_str());
            }
        }
    }

    if (poReader->GetClassCount() > 1 && poReader->IsSequentialLayers() &&
        pszReadMode == nullptr)
    {
        CPLDebug("GML",
                 "Layers are monoblock. Using SEQUENTIAL_LAYERS read mode");
        eReadMode = SEQUENTIAL_LAYERS;
    }

    if (!DealWithOgrSchemaOpenOption(poOpenInfo))
    {
        return false;
    }

    // Save the schema file if possible.  Don't make a fuss if we
    // can't.  It could be read-only directory or something.
    const char *pszWriteGFS =
        CSLFetchNameValueDef(poOpenInfo->papszOpenOptions, "WRITE_GFS", "AUTO");
    bool bWriteGFS = false;
    if (EQUAL(pszWriteGFS, "AUTO"))
    {
        if (!bHaveSchema && !poReader->HasStoppedParsing() &&
            VSIIsLocal(pszFilename) &&
            VSISupportsSequentialWrite(pszFilename, false))
        {
            VSIStatBufL sGFSStatBuf;
            if (VSIStatExL(osGFSFilename, &sGFSStatBuf, VSI_STAT_EXISTS_FLAG) !=
                0)
            {
                bWriteGFS = true;
            }
            else
            {
                CPLDebug("GML", "Not saving %s file: already exists.",
                         osGFSFilename.c_str());
            }
        }
    }
    else if (CPLTestBool(pszWriteGFS))
    {
        if (bHaveSchema || !poReader->HasStoppedParsing())
        {
            bWriteGFS = true;
        }
        else
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "GFS file saving asked, but cannot be done");
        }
    }

    if (bWriteGFS)
    {
        if (!poReader->SaveClasses(osGFSFilename))
        {
            if (CPLTestBool(pszWriteGFS))
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "GFS file saving asked, but failed");
            }
            else
            {
                CPLDebug("GML", "Not saving %s file: can't be created.",
                         osGFSFilename.c_str());
            }
        }
    }

    // Translate the GMLFeatureClasses into layers.
    papoLayers = static_cast<OGRLayer **>(
        CPLCalloc(sizeof(OGRLayer *), poReader->GetClassCount()));
    nLayers = 0;

    if (poReader->GetClassCount() == 1 && nNumberOfFeatures != 0)
    {
        GMLFeatureClass *poClass = poReader->GetClass(0);
        GIntBig nFeatureCount = poClass->GetFeatureCount();
        if (nFeatureCount < 0)
        {
            poClass->SetFeatureCount(nNumberOfFeatures);
        }
        else if (nFeatureCount != nNumberOfFeatures)
        {
            CPLDebug("GML", "Feature count in header, "
                            "and actual feature count don't match");
        }
    }

    if (bIsWFS && poReader->GetClassCount() == 1)
        bUseGlobalSRSName = true;

    while (nLayers < poReader->GetClassCount())
    {
        papoLayers[nLayers] = TranslateGMLSchema(poReader->GetClass(nLayers));
        nLayers++;
    }

    // Warn if we have geometry columns without known CRS due to only using
    // the .xsd
    if (bHaveSchema && pszForceSRSDetection == nullptr)
    {
        bool bExitLoop = false;
        for (int i = 0; !bExitLoop && i < nLayers; ++i)
        {
            const auto poLayer = papoLayers[i];
            const auto poLayerDefn = poLayer->GetLayerDefn();
            const auto nGeomFieldCount = poLayerDefn->GetGeomFieldCount();
            for (int j = 0; j < nGeomFieldCount; ++j)
            {
                if (poLayerDefn->GetGeomFieldDefn(j)->GetSpatialRef() ==
                    nullptr)
                {
                    bExitLoop = true;
                    break;
                }
            }
        }
        if (bExitLoop)
        {
            CPLDebug("GML",
                     "Geometry fields without known CRS have been detected. "
                     "You may want to specify the FORCE_SRS_DETECTION open "
                     "option to YES.");
        }
    }

    return true;
}

/************************************************************************/
/*                          BuildJointClassFromXSD()                    */
/************************************************************************/

void OGRGMLDataSource::BuildJointClassFromXSD()
{
    CPLString osJointClassName = "join";
    for (int i = 0; i < poReader->GetClassCount(); i++)
    {
        osJointClassName += "_";
        osJointClassName += poReader->GetClass(i)->GetName();
    }
    GMLFeatureClass *poJointClass = new GMLFeatureClass(osJointClassName);
    poJointClass->SetElementName("Tuple");
    for (int i = 0; i < poReader->GetClassCount(); i++)
    {
        GMLFeatureClass *poClass = poReader->GetClass(i);

        {
            CPLString osPropertyName;
            osPropertyName.Printf("%s.%s", poClass->GetName(), "gml_id");
            GMLPropertyDefn *poNewProperty =
                new GMLPropertyDefn(osPropertyName);
            CPLString osSrcElement;
            osSrcElement.Printf("member|%s@id", poClass->GetName());
            poNewProperty->SetSrcElement(osSrcElement);
            poNewProperty->SetType(GMLPT_String);
            poJointClass->AddProperty(poNewProperty);
        }

        for (int iField = 0; iField < poClass->GetPropertyCount(); iField++)
        {
            GMLPropertyDefn *poProperty = poClass->GetProperty(iField);
            CPLString osPropertyName;
            osPropertyName.Printf("%s.%s", poClass->GetName(),
                                  poProperty->GetName());
            GMLPropertyDefn *poNewProperty =
                new GMLPropertyDefn(osPropertyName);

            poNewProperty->SetType(poProperty->GetType());
            CPLString osSrcElement;
            osSrcElement.Printf("member|%s|%s", poClass->GetName(),
                                poProperty->GetSrcElement());
            poNewProperty->SetSrcElement(osSrcElement);
            poNewProperty->SetWidth(poProperty->GetWidth());
            poNewProperty->SetPrecision(poProperty->GetPrecision());
            poNewProperty->SetNullable(poProperty->IsNullable());

            poJointClass->AddProperty(poNewProperty);
        }
        for (int iField = 0; iField < poClass->GetGeometryPropertyCount();
             iField++)
        {
            GMLGeometryPropertyDefn *poProperty =
                poClass->GetGeometryProperty(iField);
            CPLString osPropertyName;
            osPropertyName.Printf("%s.%s", poClass->GetName(),
                                  poProperty->GetName());
            CPLString osSrcElement;
            osSrcElement.Printf("member|%s|%s", poClass->GetName(),
                                poProperty->GetSrcElement());
            GMLGeometryPropertyDefn *poNewProperty =
                new GMLGeometryPropertyDefn(osPropertyName, osSrcElement,
                                            poProperty->GetType(), -1,
                                            poProperty->IsNullable());
            poJointClass->AddGeometryProperty(poNewProperty);
        }
    }
    poJointClass->SetSchemaLocked(true);

    poReader->ClearClasses();
    poReader->AddClass(poJointClass);
}

/************************************************************************/
/*                   BuildJointClassFromScannedSchema()                 */
/************************************************************************/

void OGRGMLDataSource::BuildJointClassFromScannedSchema()
{
    // Make sure that all properties of a same base feature type are
    // consecutive. If not, reorder.
    std::vector<std::vector<GMLPropertyDefn *>> aapoProps;
    GMLFeatureClass *poClass = poReader->GetClass(0);
    CPLString osJointClassName = "join";

    for (int iField = 0; iField < poClass->GetPropertyCount(); iField++)
    {
        GMLPropertyDefn *poProp = poClass->GetProperty(iField);
        CPLString osPrefix(poProp->GetName());
        size_t iPos = osPrefix.find('.');
        if (iPos != std::string::npos)
            osPrefix.resize(iPos);
        int iSubClass = 0;  // Used after for.
        for (; iSubClass < static_cast<int>(aapoProps.size()); iSubClass++)
        {
            CPLString osPrefixClass(aapoProps[iSubClass][0]->GetName());
            iPos = osPrefixClass.find('.');
            if (iPos != std::string::npos)
                osPrefixClass.resize(iPos);
            if (osPrefix == osPrefixClass)
                break;
        }
        if (iSubClass == static_cast<int>(aapoProps.size()))
        {
            osJointClassName += "_";
            osJointClassName += osPrefix;
            aapoProps.push_back(std::vector<GMLPropertyDefn *>());
        }
        aapoProps[iSubClass].push_back(poProp);
    }
    poClass->SetElementName(poClass->GetName());
    poClass->SetName(osJointClassName);

    poClass->StealProperties();
    std::vector<std::pair<CPLString, std::vector<GMLGeometryPropertyDefn *>>>
        aapoGeomProps;
    for (int iSubClass = 0; iSubClass < static_cast<int>(aapoProps.size());
         iSubClass++)
    {
        CPLString osPrefixClass(aapoProps[iSubClass][0]->GetName());
        size_t iPos = osPrefixClass.find('.');
        if (iPos != std::string::npos)
            osPrefixClass.resize(iPos);
        aapoGeomProps.emplace_back(
            std::pair(osPrefixClass, std::vector<GMLGeometryPropertyDefn *>()));
        for (int iField = 0;
             iField < static_cast<int>(aapoProps[iSubClass].size()); iField++)
        {
            poClass->AddProperty(aapoProps[iSubClass][iField]);
        }
    }
    aapoProps.resize(0);

    // Reorder geometry fields too
    for (int iField = 0; iField < poClass->GetGeometryPropertyCount(); iField++)
    {
        GMLGeometryPropertyDefn *poProp = poClass->GetGeometryProperty(iField);
        CPLString osPrefix(poProp->GetName());
        size_t iPos = osPrefix.find('.');
        if (iPos != std::string::npos)
            osPrefix.resize(iPos);
        int iSubClass = 0;  // Used after for.
        for (; iSubClass < static_cast<int>(aapoGeomProps.size()); iSubClass++)
        {
            if (osPrefix == aapoGeomProps[iSubClass].first)
                break;
        }
        if (iSubClass == static_cast<int>(aapoGeomProps.size()))
            aapoGeomProps.emplace_back(
                std::pair(osPrefix, std::vector<GMLGeometryPropertyDefn *>()));
        aapoGeomProps[iSubClass].second.push_back(poProp);
    }
    poClass->StealGeometryProperties();
    for (int iSubClass = 0; iSubClass < static_cast<int>(aapoGeomProps.size());
         iSubClass++)
    {
        for (int iField = 0;
             iField < static_cast<int>(aapoGeomProps[iSubClass].second.size());
             iField++)
        {
            poClass->AddGeometryProperty(
                aapoGeomProps[iSubClass].second[iField]);
        }
    }
}

/************************************************************************/
/*                         TranslateGMLSchema()                         */
/************************************************************************/

OGRGMLLayer *OGRGMLDataSource::TranslateGMLSchema(GMLFeatureClass *poClass)

{
    // Create an empty layer.
    const char *pszSRSName = poClass->GetSRSName();
    OGRSpatialReference *poSRS = nullptr;
    if (pszSRSName)
    {
        poSRS = new OGRSpatialReference();
        poSRS->SetAxisMappingStrategy(m_bInvertAxisOrderIfLatLong
                                          ? OAMS_TRADITIONAL_GIS_ORDER
                                          : OAMS_AUTHORITY_COMPLIANT);
        if (poSRS->SetFromUserInput(
                pszSRSName,
                OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS_get()) !=
            OGRERR_NONE)
        {
            delete poSRS;
            poSRS = nullptr;
        }
    }
    else
    {
        pszSRSName = GetGlobalSRSName();

        if (pszSRSName && GML_IsLegitSRSName(pszSRSName))
        {
            poSRS = new OGRSpatialReference();
            poSRS->SetAxisMappingStrategy(m_bInvertAxisOrderIfLatLong
                                              ? OAMS_TRADITIONAL_GIS_ORDER
                                              : OAMS_AUTHORITY_COMPLIANT);
            if (poSRS->SetFromUserInput(
                    pszSRSName,
                    OGRSpatialReference::
                        SET_FROM_USER_INPUT_LIMITATIONS_get()) != OGRERR_NONE)
            {
                delete poSRS;
                poSRS = nullptr;
            }

            if (poSRS != nullptr && m_bInvertAxisOrderIfLatLong &&
                GML_IsSRSLatLongOrder(pszSRSName))
            {
                if (!poClass->HasExtents() && sBoundingRect.IsInit())
                {
                    poClass->SetExtents(sBoundingRect.MinY, sBoundingRect.MaxY,
                                        sBoundingRect.MinX, sBoundingRect.MaxX);
                }
            }
        }

        if (!poClass->HasExtents() && sBoundingRect.IsInit())
        {
            poClass->SetExtents(sBoundingRect.MinX, sBoundingRect.MaxX,
                                sBoundingRect.MinY, sBoundingRect.MaxY);
        }
    }

    // Report a COMPD_CS only if GML_REPORT_COMPD_CS is explicitly set to TRUE.
    if (poSRS != nullptr && poSRS->IsCompound())
    {
        const char *pszReportCompdCS =
            CPLGetConfigOption("GML_REPORT_COMPD_CS", nullptr);
        if (pszReportCompdCS == nullptr)
        {
            CPLDebug("GML", "Compound CRS detected but only horizontal part "
                            "will be reported. Set the GML_REPORT_COMPD_CS=YES "
                            "configuration option to get the Compound CRS");
            pszReportCompdCS = "FALSE";
        }
        if (!CPLTestBool(pszReportCompdCS))
        {
            OGR_SRSNode *poCOMPD_CS = poSRS->GetAttrNode("COMPD_CS");
            if (poCOMPD_CS != nullptr)
            {
                OGR_SRSNode *poCandidateRoot = poCOMPD_CS->GetNode("PROJCS");
                if (poCandidateRoot == nullptr)
                    poCandidateRoot = poCOMPD_CS->GetNode("GEOGCS");
                if (poCandidateRoot != nullptr)
                {
                    poSRS->SetRoot(poCandidateRoot->Clone());
                }
            }
        }
    }

    OGRGMLLayer *poLayer = new OGRGMLLayer(poClass->GetName(), false, this);

    // Added attributes (properties).
    if (bExposeGMLId)
    {
        OGRFieldDefn oField("gml_id", OFTString);
        oField.SetNullable(FALSE);
        poLayer->GetLayerDefn()->AddFieldDefn(&oField);
    }
    else if (bExposeFid)
    {
        OGRFieldDefn oField("fid", OFTString);
        oField.SetNullable(FALSE);
        poLayer->GetLayerDefn()->AddFieldDefn(&oField);
    }

    for (int iField = 0; iField < poClass->GetGeometryPropertyCount(); iField++)
    {
        GMLGeometryPropertyDefn *poProperty =
            poClass->GetGeometryProperty(iField);

        // Patch wrong .gfs file produced by earlier versions
        if (poProperty->GetType() == wkbPolyhedralSurface &&
            strcmp(poProperty->GetName(), "lod2Solid") == 0)
        {
            poProperty->SetType(wkbPolyhedralSurfaceZ);
        }

        OGRGeomFieldDefn oField(poProperty->GetName(), poProperty->GetType());
        if (poClass->GetGeometryPropertyCount() == 1 &&
            poClass->GetFeatureCount() == 0)
        {
            oField.SetType(wkbUnknown);
        }

        const auto &osSRSName = poProperty->GetSRSName();
        if (!osSRSName.empty())
        {
            OGRSpatialReference *poSRS2 = new OGRSpatialReference();
            poSRS2->SetAxisMappingStrategy(m_bInvertAxisOrderIfLatLong
                                               ? OAMS_TRADITIONAL_GIS_ORDER
                                               : OAMS_AUTHORITY_COMPLIANT);
            if (poSRS2->SetFromUserInput(
                    osSRSName.c_str(),
                    OGRSpatialReference::
                        SET_FROM_USER_INPUT_LIMITATIONS_get()) == OGRERR_NONE)
            {
                oField.SetSpatialRef(poSRS2);
            }
            poSRS2->Release();
        }
        else
        {
            oField.SetSpatialRef(poSRS);
        }
        oField.SetNullable(poProperty->IsNullable());
        oField.SetCoordinatePrecision(poProperty->GetCoordinatePrecision());
        poLayer->GetLayerDefn()->AddGeomFieldDefn(&oField);
    }

    if (poReader->GetClassCount() == 1)
    {
        int iInsertPos = 0;
        for (const auto &osElt : m_aosGMLExtraElements)
        {
            GMLPropertyDefn *poProperty =
                new GMLPropertyDefn(osElt.c_str(), osElt.c_str());
            poProperty->SetType(GMLPT_String);
            if (poClass->AddProperty(poProperty, iInsertPos) == iInsertPos)
                ++iInsertPos;
            else
                delete poProperty;
        }
    }

    for (int iField = 0; iField < poClass->GetPropertyCount(); iField++)
    {
        GMLPropertyDefn *poProperty = poClass->GetProperty(iField);
        OGRFieldSubType eSubType = poProperty->GetSubType();
        const OGRFieldType eFType =
            GML_GetOGRFieldType(poProperty->GetType(), eSubType);
        OGRFieldDefn oField(poProperty->GetName(), eFType);
        oField.SetSubType(eSubType);
        if (STARTS_WITH_CI(oField.GetNameRef(), "ogr:"))
            oField.SetName(poProperty->GetName() + 4);
        if (poProperty->GetWidth() > 0)
            oField.SetWidth(poProperty->GetWidth());
        if (poProperty->GetPrecision() > 0)
            oField.SetPrecision(poProperty->GetPrecision());
        if (!bEmptyAsNull)
            oField.SetNullable(poProperty->IsNullable());
        oField.SetUnique(poProperty->IsUnique());
        oField.SetComment(poProperty->GetDocumentation());
        poLayer->GetLayerDefn()->AddFieldDefn(&oField);
    }

    if (poSRS != nullptr)
        poSRS->Release();

    return poLayer;
}

/************************************************************************/
/*                         GetGlobalSRSName()                           */
/************************************************************************/

const char *OGRGMLDataSource::GetGlobalSRSName()
{
    if (poReader->CanUseGlobalSRSName() || bUseGlobalSRSName)
        return poReader->GetGlobalSRSName();
    else
        return nullptr;
}

/************************************************************************/
/*                               Create()                               */
/************************************************************************/

bool OGRGMLDataSource::Create(const char *pszFilename, char **papszOptions)

{
    if (fpOutput != nullptr || poReader != nullptr)
    {
        CPLAssert(false);
        return false;
    }

    if (strcmp(pszFilename, "/dev/stdout") == 0)
        pszFilename = "/vsistdout/";

    // Read options.
    CSLDestroy(papszCreateOptions);
    papszCreateOptions = CSLDuplicate(papszOptions);

    const char *pszFormat =
        CSLFetchNameValueDef(papszCreateOptions, "FORMAT", "GML3.2");
    bIsOutputGML3 = EQUAL(pszFormat, "GML3");
    bIsOutputGML3Deegree = EQUAL(pszFormat, "GML3Deegree");
    bIsOutputGML32 = EQUAL(pszFormat, "GML3.2");
    if (bIsOutputGML3Deegree || bIsOutputGML32)
        bIsOutputGML3 = true;

    eSRSNameFormat = (bIsOutputGML3) ? SRSNAME_OGC_URN : SRSNAME_SHORT;
    if (bIsOutputGML3)
    {
        const char *pszLongSRS =
            CSLFetchNameValue(papszCreateOptions, "GML3_LONGSRS");
        const char *pszSRSNameFormat =
            CSLFetchNameValue(papszCreateOptions, "SRSNAME_FORMAT");
        if (pszSRSNameFormat)
        {
            if (pszLongSRS)
            {
                CPLError(CE_Warning, CPLE_NotSupported,
                         "Both GML3_LONGSRS and SRSNAME_FORMAT specified. "
                         "Ignoring GML3_LONGSRS");
            }
            if (EQUAL(pszSRSNameFormat, "SHORT"))
                eSRSNameFormat = SRSNAME_SHORT;
            else if (EQUAL(pszSRSNameFormat, "OGC_URN"))
                eSRSNameFormat = SRSNAME_OGC_URN;
            else if (EQUAL(pszSRSNameFormat, "OGC_URL"))
                eSRSNameFormat = SRSNAME_OGC_URL;
            else
            {
                CPLError(CE_Warning, CPLE_NotSupported,
                         "Invalid value for SRSNAME_FORMAT. "
                         "Using SRSNAME_OGC_URN");
            }
        }
        else if (pszLongSRS && !CPLTestBool(pszLongSRS))
            eSRSNameFormat = SRSNAME_SHORT;
    }

    bWriteSpaceIndentation = CPLTestBool(
        CSLFetchNameValueDef(papszCreateOptions, "SPACE_INDENTATION", "YES"));

    // Create the output file.
    osFilename = pszFilename;
    SetDescription(pszFilename);

    if (strcmp(pszFilename, "/vsistdout/") == 0 ||
        STARTS_WITH(pszFilename, "/vsigzip/"))
    {
        fpOutput = VSIFOpenExL(pszFilename, "wb", true);
        bFpOutputIsNonSeekable = true;
        bFpOutputSingleFile = true;
    }
    else if (STARTS_WITH(pszFilename, "/vsizip/"))
    {
        if (EQUAL(CPLGetExtensionSafe(pszFilename).c_str(), "zip"))
        {
            SetDescription(
                CPLFormFilenameSafe(pszFilename, "out.gml", nullptr).c_str());
        }

        fpOutput = VSIFOpenExL(GetDescription(), "wb", true);
        bFpOutputIsNonSeekable = true;
    }
    else
        fpOutput = VSIFOpenExL(pszFilename, "wb+", true);
    if (fpOutput == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                 "Failed to create GML file %s: %s", pszFilename,
                 VSIGetLastErrorMsg());
        return false;
    }

    // Write out "standard" header.
    PrintLine(fpOutput, "%s", "<?xml version=\"1.0\" encoding=\"utf-8\" ?>");

    if (!bFpOutputIsNonSeekable)
        nSchemaInsertLocation = static_cast<int>(VSIFTellL(fpOutput));

    const char *pszPrefix = GetAppPrefix();
    const char *pszTargetNameSpace = CSLFetchNameValueDef(
        papszOptions, "TARGET_NAMESPACE", "http://ogr.maptools.org/");

    if (GMLFeatureCollection())
        PrintLine(fpOutput, "<gml:FeatureCollection");
    else if (RemoveAppPrefix())
        PrintLine(fpOutput, "<FeatureCollection");
    else
        PrintLine(fpOutput, "<%s:FeatureCollection", pszPrefix);

    if (IsGML32Output())
    {
        char *pszGMLId = CPLEscapeString(
            CSLFetchNameValueDef(papszOptions, "GML_ID", "aFeatureCollection"),
            -1, CPLES_XML);
        PrintLine(fpOutput, "     gml:id=\"%s\"", pszGMLId);
        CPLFree(pszGMLId);
    }

    // Write out schema info if provided in creation options.
    const char *pszSchemaURI = CSLFetchNameValue(papszOptions, "XSISCHEMAURI");
    const char *pszSchemaOpt = CSLFetchNameValue(papszOptions, "XSISCHEMA");

    if (pszSchemaURI != nullptr)
    {
        PrintLine(
            fpOutput,
            "     xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"");
        PrintLine(fpOutput, "     xsi:schemaLocation=\"%s\"", pszSchemaURI);
    }
    else if (pszSchemaOpt == nullptr || EQUAL(pszSchemaOpt, "EXTERNAL"))
    {
        char *pszBasename =
            CPLStrdup(CPLGetBasenameSafe(GetDescription()).c_str());

        PrintLine(
            fpOutput,
            "     xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"");
        PrintLine(fpOutput, "     xsi:schemaLocation=\"%s %s\"",
                  pszTargetNameSpace,
                  CPLResetExtensionSafe(pszBasename, "xsd").c_str());
        CPLFree(pszBasename);
    }

    if (RemoveAppPrefix())
        PrintLine(fpOutput, "     xmlns=\"%s\"", pszTargetNameSpace);
    else
        PrintLine(fpOutput, "     xmlns:%s=\"%s\"", pszPrefix,
                  pszTargetNameSpace);

    if (IsGML32Output())
        PrintLine(fpOutput, "%s",
                  "     xmlns:gml=\"http://www.opengis.net/gml/3.2\">");
    else
        PrintLine(fpOutput, "%s",
                  "     xmlns:gml=\"http://www.opengis.net/gml\">");

    return true;
}

/************************************************************************/
/*                         WriteTopElements()                           */
/************************************************************************/

void OGRGMLDataSource::WriteTopElements()
{
    const char *pszDescription = CSLFetchNameValueDef(
        papszCreateOptions, "DESCRIPTION", GetMetadataItem("DESCRIPTION"));
    if (pszDescription != nullptr)
    {
        if (bWriteSpaceIndentation)
            VSIFPrintfL(fpOutput, "  ");
        char *pszTmp = CPLEscapeString(pszDescription, -1, CPLES_XML);
        PrintLine(fpOutput, "<gml:description>%s</gml:description>", pszTmp);
        CPLFree(pszTmp);
    }

    const char *l_pszName = CSLFetchNameValueDef(papszCreateOptions, "NAME",
                                                 GetMetadataItem("NAME"));
    if (l_pszName != nullptr)
    {
        if (bWriteSpaceIndentation)
            VSIFPrintfL(fpOutput, "  ");
        char *pszTmp = CPLEscapeString(l_pszName, -1, CPLES_XML);
        PrintLine(fpOutput, "<gml:name>%s</gml:name>", pszTmp);
        CPLFree(pszTmp);
    }

    // Should we initialize an area to place the boundedBy element?
    // We will need to seek back to fill it in.
    nBoundedByLocation = -1;
    if (CPLFetchBool(papszCreateOptions, "BOUNDEDBY", true))
    {
        if (!bFpOutputIsNonSeekable)
        {
            nBoundedByLocation = static_cast<int>(VSIFTellL(fpOutput));

            if (nBoundedByLocation != -1)
                PrintLine(fpOutput, "%350s", "");
        }
        else
        {
            if (bWriteSpaceIndentation)
                VSIFPrintfL(fpOutput, "  ");
            if (IsGML3Output())
                PrintLine(fpOutput,
                          "<gml:boundedBy><gml:Null /></gml:boundedBy>");
            else
                PrintLine(fpOutput, "<gml:boundedBy><gml:null>missing</"
                                    "gml:null></gml:boundedBy>");
        }
    }
}

/************************************************************************/
/*                 DealWithOgrSchemaOpenOption()                        */
/************************************************************************/

bool OGRGMLDataSource::DealWithOgrSchemaOpenOption(
    const GDALOpenInfo *poOpenInfo)
{

    std::string osFieldsSchemaOverrideParam =
        CSLFetchNameValueDef(poOpenInfo->papszOpenOptions, "OGR_SCHEMA", "");

    if (!osFieldsSchemaOverrideParam.empty())
    {

        // GML driver does not support update at the moment so this will never happen
        if (poOpenInfo->eAccess == GA_Update)
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "OGR_SCHEMA open option is not supported in update mode.");
            return false;
        }

        OGRSchemaOverride osSchemaOverride;
        if (!osSchemaOverride.LoadFromJSON(osFieldsSchemaOverrideParam) ||
            !osSchemaOverride.IsValid())
        {
            return false;
        }

        const auto &oLayerOverrides = osSchemaOverride.GetLayerOverrides();
        for (const auto &oLayer : oLayerOverrides)
        {
            const auto &oLayerName = oLayer.first;
            const auto &oLayerFieldOverride = oLayer.second;
            const bool bIsFullOverride{oLayerFieldOverride.IsFullOverride()};
            std::vector<GMLPropertyDefn *> aoProperties;

            CPLDebug("GML", "Applying schema override for layer %s",
                     oLayerName.c_str());

            // Fail if the layer name does not exist
            const auto oClass = poReader->GetClass(oLayerName.c_str());
            if (oClass == nullptr)
            {
                CPLError(CE_Failure, CPLE_AppDefined, "Layer %s not found",
                         oLayerName.c_str());
                return false;
            }

            const auto &oLayerFields = oLayerFieldOverride.GetFieldOverrides();
            for (const auto &oFieldOverrideIt : oLayerFields)
            {
                const auto oProperty =
                    oClass->GetProperty(oFieldOverrideIt.first.c_str());
                if (oProperty == nullptr)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Field %s not found in layer %s",
                             oFieldOverrideIt.first.c_str(),
                             oLayerName.c_str());
                    return false;
                }

                const auto &oFieldOverride = oFieldOverrideIt.second;

                const OGRFieldSubType eSubType =
                    oFieldOverride.GetFieldSubType().value_or(OFSTNone);

                if (oFieldOverride.GetFieldSubType().has_value())
                {
                    oProperty->SetSubType(eSubType);
                }

                if (oFieldOverride.GetFieldType().has_value())
                {
                    oProperty->SetType(GML_FromOGRFieldType(
                        oFieldOverride.GetFieldType().value(), eSubType));
                }

                if (oFieldOverride.GetFieldName().has_value())
                {
                    oProperty->SetName(
                        oFieldOverride.GetFieldName().value().c_str());
                }

                if (oFieldOverride.GetFieldWidth().has_value())
                {
                    oProperty->SetWidth(oFieldOverride.GetFieldWidth().value());
                }

                if (oFieldOverride.GetFieldPrecision().has_value())
                {
                    oProperty->SetPrecision(
                        oFieldOverride.GetFieldPrecision().value());
                }

                if (bIsFullOverride)
                {
                    aoProperties.push_back(oProperty);
                }
            }

            // Remove fields not in the override
            if (bIsFullOverride &&
                aoProperties.size() !=
                    static_cast<size_t>(oClass->GetPropertyCount()))
            {
                for (int j = 0; j < oClass->GetPropertyCount(); ++j)
                {
                    const auto oProperty = oClass->GetProperty(j);
                    if (std::find(aoProperties.begin(), aoProperties.end(),
                                  oProperty) == aoProperties.end())
                    {
                        delete (oProperty);
                    }
                }

                oClass->StealProperties();

                for (const auto &oProperty : aoProperties)
                {
                    oClass->AddProperty(oProperty);
                }
            }
        }
    }
    return true;
}

/************************************************************************/
/*                         DeclareNewWriteSRS()                         */
/************************************************************************/

// Check that all SRS passed to ICreateLayer() and CreateGeomField()
// are the same (or all null)

void OGRGMLDataSource::DeclareNewWriteSRS(const OGRSpatialReference *poSRS)
{
    if (m_bWriteGlobalSRS)
    {
        if (!m_bWriteGlobalSRSInit)
        {
            m_bWriteGlobalSRSInit = true;
            if (poSRS)
            {
                m_poWriteGlobalSRS.reset(poSRS->Clone());
                m_poWriteGlobalSRS->SetAxisMappingStrategy(
                    OAMS_TRADITIONAL_GIS_ORDER);
            }
        }
        else
        {
            if (m_poWriteGlobalSRS)
            {
                const char *const apszOptions[] = {
                    "IGNORE_DATA_AXIS_TO_SRS_AXIS_MAPPING=YES", nullptr};
                if (!poSRS ||
                    !poSRS->IsSame(m_poWriteGlobalSRS.get(), apszOptions))
                {
                    m_bWriteGlobalSRS = false;
                }
            }
            else
            {
                if (poSRS)
                    m_bWriteGlobalSRS = false;
            }
        }
    }
}

/************************************************************************/
/*                           ICreateLayer()                             */
/************************************************************************/

OGRLayer *
OGRGMLDataSource::ICreateLayer(const char *pszLayerName,
                               const OGRGeomFieldDefn *poSrcGeomFieldDefn,
                               CSLConstList /*papszOptions*/)
{
    // Verify we are in update mode.
    if (fpOutput == nullptr)
    {
        CPLError(CE_Failure, CPLE_NoWriteAccess,
                 "Data source %s opened for read access.\n"
                 "New layer %s cannot be created.\n",
                 GetDescription(), pszLayerName);

        return nullptr;
    }

    const auto eType =
        poSrcGeomFieldDefn ? poSrcGeomFieldDefn->GetType() : wkbNone;
    const auto poSRS =
        poSrcGeomFieldDefn ? poSrcGeomFieldDefn->GetSpatialRef() : nullptr;

    // Ensure name is safe as an element name.
    char *pszCleanLayerName = CPLStrdup(pszLayerName);

    CPLCleanXMLElementName(pszCleanLayerName);
    if (strcmp(pszCleanLayerName, pszLayerName) != 0)
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Layer name '%s' adjusted to '%s' for XML validity.",
                 pszLayerName, pszCleanLayerName);
    }

    if (nLayers == 0)
    {
        WriteTopElements();
    }

    // Create the layer object.
    OGRGMLLayer *poLayer = new OGRGMLLayer(pszCleanLayerName, true, this);
    poLayer->GetLayerDefn()->SetGeomType(eType);
    if (eType != wkbNone)
    {
        auto poGeomFieldDefn = poLayer->GetLayerDefn()->GetGeomFieldDefn(0);
        const char *pszGeomFieldName = poSrcGeomFieldDefn->GetNameRef();
        if (!pszGeomFieldName || pszGeomFieldName[0] == 0)
            pszGeomFieldName = "geometryProperty";
        poGeomFieldDefn->SetName(pszGeomFieldName);
        poGeomFieldDefn->SetNullable(poSrcGeomFieldDefn->IsNullable());
        DeclareNewWriteSRS(poSRS);
        if (poSRS != nullptr)
        {
            auto poSRSClone = poSRS->Clone();
            poSRSClone->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            poGeomFieldDefn->SetSpatialRef(poSRSClone);
            poSRSClone->Dereference();
        }
        poGeomFieldDefn->SetCoordinatePrecision(
            poSrcGeomFieldDefn->GetCoordinatePrecision());
    }

    CPLFree(pszCleanLayerName);

    // Add layer to data source layer list.
    papoLayers = static_cast<OGRLayer **>(
        CPLRealloc(papoLayers, sizeof(OGRLayer *) * (nLayers + 1)));

    papoLayers[nLayers++] = poLayer;

    return poLayer;
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGRGMLDataSource::TestCapability(const char *pszCap)

{
    if (EQUAL(pszCap, ODsCCreateLayer))
        return TRUE;
    else if (EQUAL(pszCap, ODsCCreateGeomFieldAfterCreateLayer))
        return TRUE;
    else if (EQUAL(pszCap, ODsCCurveGeometries))
        return bIsOutputGML3;
    else if (EQUAL(pszCap, ODsCZGeometries))
        return TRUE;
    else if (EQUAL(pszCap, ODsCRandomLayerWrite))
        return TRUE;
    else
        return FALSE;
}

/************************************************************************/
/*                              GetLayer()                              */
/************************************************************************/

OGRLayer *OGRGMLDataSource::GetLayer(int iLayer)

{
    if (iLayer < 0 || iLayer >= nLayers)
        return nullptr;
    else
        return papoLayers[iLayer];
}

/************************************************************************/
/*                            GrowExtents()                             */
/************************************************************************/

void OGRGMLDataSource::GrowExtents(OGREnvelope3D *psGeomBounds,
                                   int nCoordDimension)

{
    sBoundingRect.Merge(*psGeomBounds);
    if (nCoordDimension == 3)
        bBBOX3D = true;
}

/************************************************************************/
/*                            InsertHeader()                            */
/*                                                                      */
/*      This method is used to update boundedby info for a              */
/*      dataset, and insert schema descriptions depending on            */
/*      selection options in effect.                                    */
/************************************************************************/

void OGRGMLDataSource::InsertHeader()

{
    int nSchemaStart = 0;

    if (bFpOutputSingleFile)
        return;

    // Do we want to write the schema within the GML instance doc
    // or to a separate file?  For now we only support external.
    const char *pszSchemaURI =
        CSLFetchNameValue(papszCreateOptions, "XSISCHEMAURI");
    const char *pszSchemaOpt =
        CSLFetchNameValue(papszCreateOptions, "XSISCHEMA");

    const bool bGMLFeatureCollection = GMLFeatureCollection();

    if (pszSchemaURI != nullptr)
        return;

    VSILFILE *fpSchema = nullptr;
    if (pszSchemaOpt == nullptr || EQUAL(pszSchemaOpt, "EXTERNAL"))
    {
        const std::string l_osXSDFilename =
            CPLResetExtensionSafe(GetDescription(), "xsd");

        fpSchema = VSIFOpenL(l_osXSDFilename.c_str(), "wt");
        if (fpSchema == nullptr)
        {
            CPLError(CE_Failure, CPLE_OpenFailed,
                     "Failed to open file %.500s for schema output.",
                     l_osXSDFilename.c_str());
            return;
        }
        PrintLine(fpSchema, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    }
    else if (EQUAL(pszSchemaOpt, "INTERNAL"))
    {
        if (fpOutput == nullptr)
            return;
        nSchemaStart = static_cast<int>(VSIFTellL(fpOutput));
        fpSchema = fpOutput;
    }
    else
    {
        return;
    }

    // Write the schema section at the end of the file.  Once
    // complete, we will read it back in, and then move the whole
    // file "down" enough to insert the schema at the beginning.

    // Detect if there are fields of List types.
    bool bHasListFields = false;

    const int nLayerCount = OGRGMLDataSource::GetLayerCount();
    for (int iLayer = 0; !bHasListFields && iLayer < nLayerCount; iLayer++)
    {
        OGRFeatureDefn *poFDefn = papoLayers[iLayer]->GetLayerDefn();
        for (int iField = 0;
             !bHasListFields && iField < poFDefn->GetFieldCount(); iField++)
        {
            OGRFieldDefn *poFieldDefn = poFDefn->GetFieldDefn(iField);

            if (poFieldDefn->GetType() == OFTIntegerList ||
                poFieldDefn->GetType() == OFTInteger64List ||
                poFieldDefn->GetType() == OFTRealList ||
                poFieldDefn->GetType() == OFTStringList)
            {
                bHasListFields = true;
            }
        }
    }

    // Emit the start of the schema section.
    const char *pszPrefix = GetAppPrefix();
    if (pszPrefix[0] == '\0')
        pszPrefix = "ogr";
    const char *pszTargetNameSpace = CSLFetchNameValueDef(
        papszCreateOptions, "TARGET_NAMESPACE", "http://ogr.maptools.org/");

    if (IsGML3Output())
    {
        PrintLine(fpSchema, "<xs:schema ");
        PrintLine(fpSchema, "    targetNamespace=\"%s\"", pszTargetNameSpace);
        PrintLine(fpSchema, "    xmlns:%s=\"%s\"", pszPrefix,
                  pszTargetNameSpace);
        PrintLine(fpSchema,
                  "    xmlns:xs=\"http://www.w3.org/2001/XMLSchema\"");
        if (IsGML32Output())
        {
            PrintLine(fpSchema,
                      "    xmlns:gml=\"http://www.opengis.net/gml/3.2\"");
            if (!bGMLFeatureCollection)
            {
                PrintLine(
                    fpSchema,
                    "    xmlns:gmlsf=\"http://www.opengis.net/gmlsf/2.0\"");
            }
        }
        else
        {
            PrintLine(fpSchema, "    xmlns:gml=\"http://www.opengis.net/gml\"");
            if (!IsGML3DeegreeOutput() && !bGMLFeatureCollection)
            {
                PrintLine(fpSchema,
                          "    xmlns:gmlsf=\"http://www.opengis.net/gmlsf\"");
            }
        }
        PrintLine(fpSchema, "    elementFormDefault=\"qualified\"");
        PrintLine(fpSchema, "    version=\"1.0\">");

        if (IsGML32Output())
        {
            if (!bGMLFeatureCollection)
            {
                PrintLine(fpSchema, "<xs:annotation>");
                PrintLine(fpSchema, "  <xs:appinfo "
                                    "source=\"http://schemas.opengis.net/"
                                    "gmlsfProfile/2.0/gmlsfLevels.xsd\">");
                PrintLine(
                    fpSchema,
                    "    <gmlsf:ComplianceLevel>%d</gmlsf:ComplianceLevel>",
                    (bHasListFields) ? 1 : 0);
                PrintLine(fpSchema, "  </xs:appinfo>");
                PrintLine(fpSchema, "</xs:annotation>");
            }

            PrintLine(fpSchema,
                      "<xs:import namespace=\"http://www.opengis.net/gml/3.2\" "
                      "schemaLocation=\"http://schemas.opengis.net/gml/3.2.1/"
                      "gml.xsd\"/>");
            if (!bGMLFeatureCollection)
            {
                PrintLine(
                    fpSchema,
                    "<xs:import namespace=\"http://www.opengis.net/gmlsf/2.0\" "
                    "schemaLocation=\"http://schemas.opengis.net/gmlsfProfile/"
                    "2.0/gmlsfLevels.xsd\"/>");
            }
        }
        else
        {
            if (!IsGML3DeegreeOutput() && !bGMLFeatureCollection)
            {
                PrintLine(fpSchema, "<xs:annotation>");
                PrintLine(fpSchema,
                          "  <xs:appinfo "
                          "source=\"http://schemas.opengis.net/gml/3.1.1/"
                          "profiles/gmlsfProfile/1.0.0/gmlsfLevels.xsd\">");
                PrintLine(
                    fpSchema,
                    "    <gmlsf:ComplianceLevel>%d</gmlsf:ComplianceLevel>",
                    (bHasListFields) ? 1 : 0);
                PrintLine(fpSchema,
                          "    "
                          "<gmlsf:GMLProfileSchema>http://schemas.opengis.net/"
                          "gml/3.1.1/profiles/gmlsfProfile/1.0.0/gmlsf.xsd</"
                          "gmlsf:GMLProfileSchema>");
                PrintLine(fpSchema, "  </xs:appinfo>");
                PrintLine(fpSchema, "</xs:annotation>");
            }

            PrintLine(fpSchema,
                      "<xs:import namespace=\"http://www.opengis.net/gml\" "
                      "schemaLocation=\"http://schemas.opengis.net/gml/3.1.1/"
                      "base/gml.xsd\"/>");
            if (!IsGML3DeegreeOutput() && !bGMLFeatureCollection)
            {
                PrintLine(
                    fpSchema,
                    "<xs:import namespace=\"http://www.opengis.net/gmlsf\" "
                    "schemaLocation=\"http://schemas.opengis.net/gml/3.1.1/"
                    "profiles/gmlsfProfile/1.0.0/gmlsfLevels.xsd\"/>");
            }
        }
    }
    else
    {
        PrintLine(fpSchema,
                  "<xs:schema targetNamespace=\"%s\" xmlns:%s=\"%s\" "
                  "xmlns:xs=\"http://www.w3.org/2001/XMLSchema\" "
                  "xmlns:gml=\"http://www.opengis.net/gml\" "
                  "elementFormDefault=\"qualified\" version=\"1.0\">",
                  pszTargetNameSpace, pszPrefix, pszTargetNameSpace);

        PrintLine(fpSchema,
                  "<xs:import namespace=\"http://www.opengis.net/gml\" "
                  "schemaLocation=\"http://schemas.opengis.net/gml/2.1.2/"
                  "feature.xsd\"/>");
    }

    // Define the FeatureCollection element
    if (!bGMLFeatureCollection)
    {
        bool bHasUniqueConstraints = false;
        for (int iLayer = 0; (iLayer < nLayerCount) && !bHasUniqueConstraints;
             iLayer++)
        {
            OGRFeatureDefn *poFDefn = papoLayers[iLayer]->GetLayerDefn();
            const int nFieldCount = poFDefn->GetFieldCount();
            for (int iField = 0;
                 (iField < nFieldCount) && !bHasUniqueConstraints; iField++)
            {
                const OGRFieldDefn *poFieldDefn = poFDefn->GetFieldDefn(iField);
                if (poFieldDefn->IsUnique())
                    bHasUniqueConstraints = true;
            }
        }

        const char *pszFeatureMemberPrefix = pszPrefix;
        if (IsGML3Output())
        {
            if (IsGML32Output())
            {
                // GML Simple Features profile v2.0 mentions gml:AbstractGML as
                // substitutionGroup but using gml:AbstractFeature makes it
                // usablable by GMLJP2 v2.
                PrintLine(fpSchema,
                          "<xs:element name=\"FeatureCollection\" "
                          "type=\"%s:FeatureCollectionType\" "
                          "substitutionGroup=\"gml:AbstractFeature\"%s>",
                          pszPrefix, bHasUniqueConstraints ? "" : "/");
            }
            else if (IsGML3DeegreeOutput())
            {
                PrintLine(fpSchema,
                          "<xs:element name=\"FeatureCollection\" "
                          "type=\"%s:FeatureCollectionType\" "
                          "substitutionGroup=\"gml:_FeatureCollection\"%s>",
                          pszPrefix, bHasUniqueConstraints ? "" : "/");
            }
            else
            {
                PrintLine(fpSchema,
                          "<xs:element name=\"FeatureCollection\" "
                          "type=\"%s:FeatureCollectionType\" "
                          "substitutionGroup=\"gml:_GML\"%s>",
                          pszPrefix, bHasUniqueConstraints ? "" : "/");
            }
        }
        else
        {
            pszFeatureMemberPrefix = "gml";
            PrintLine(fpSchema,
                      "<xs:element name=\"FeatureCollection\" "
                      "type=\"%s:FeatureCollectionType\" "
                      "substitutionGroup=\"gml:_FeatureCollection\"%s>",
                      pszPrefix, bHasUniqueConstraints ? "" : "/");
        }

        if (bHasUniqueConstraints)
        {
            for (int iLayer = 0; iLayer < nLayerCount; iLayer++)
            {
                OGRFeatureDefn *poFDefn = papoLayers[iLayer]->GetLayerDefn();
                const int nFieldCount = poFDefn->GetFieldCount();
                for (int iField = 0; iField < nFieldCount; iField++)
                {
                    const OGRFieldDefn *poFieldDefn =
                        poFDefn->GetFieldDefn(iField);
                    if (poFieldDefn->IsUnique())
                    {
                        PrintLine(
                            fpSchema,
                            "  <xs:unique name=\"uniqueConstraint_%s_%s\">",
                            poFDefn->GetName(), poFieldDefn->GetNameRef());
                        PrintLine(fpSchema,
                                  "    <xs:selector "
                                  "xpath=\"%s:featureMember/%s:%s\"/>",
                                  pszFeatureMemberPrefix, pszPrefix,
                                  poFDefn->GetName());
                        PrintLine(fpSchema, "    <xs:field xpath=\"%s:%s\"/>",
                                  pszPrefix, poFieldDefn->GetNameRef());
                        PrintLine(fpSchema, "  </xs:unique>");
                    }
                }
            }
            PrintLine(fpSchema, "</xs:element>");
        }
    }

    // Define the FeatureCollectionType
    if (IsGML3Output() && !bGMLFeatureCollection)
    {
        PrintLine(fpSchema, "<xs:complexType name=\"FeatureCollectionType\">");
        PrintLine(fpSchema, "  <xs:complexContent>");
        if (IsGML3DeegreeOutput())
        {
            PrintLine(fpSchema, "    <xs:extension "
                                "base=\"gml:AbstractFeatureCollectionType\">");
            PrintLine(fpSchema, "      <xs:sequence>");
            PrintLine(fpSchema, "        <xs:element name=\"featureMember\" "
                                "minOccurs=\"0\" maxOccurs=\"unbounded\">");
        }
        else
        {
            PrintLine(fpSchema,
                      "    <xs:extension base=\"gml:AbstractFeatureType\">");
            PrintLine(
                fpSchema,
                "      <xs:sequence minOccurs=\"0\" maxOccurs=\"unbounded\">");
            PrintLine(fpSchema, "        <xs:element name=\"featureMember\">");
        }
        PrintLine(fpSchema, "          <xs:complexType>");
        if (IsGML32Output())
        {
            PrintLine(fpSchema, "            <xs:complexContent>");
            PrintLine(fpSchema, "              <xs:extension "
                                "base=\"gml:AbstractFeatureMemberType\">");
            PrintLine(fpSchema, "                <xs:sequence>");
            PrintLine(
                fpSchema,
                "                  <xs:element ref=\"gml:AbstractFeature\"/>");
            PrintLine(fpSchema, "                </xs:sequence>");
            PrintLine(fpSchema, "              </xs:extension>");
            PrintLine(fpSchema, "            </xs:complexContent>");
        }
        else
        {
            PrintLine(fpSchema, "            <xs:sequence>");
            PrintLine(fpSchema,
                      "              <xs:element ref=\"gml:_Feature\"/>");
            PrintLine(fpSchema, "            </xs:sequence>");
        }
        PrintLine(fpSchema, "          </xs:complexType>");
        PrintLine(fpSchema, "        </xs:element>");
        PrintLine(fpSchema, "      </xs:sequence>");
        PrintLine(fpSchema, "    </xs:extension>");
        PrintLine(fpSchema, "  </xs:complexContent>");
        PrintLine(fpSchema, "</xs:complexType>");
    }
    else if (!bGMLFeatureCollection)
    {
        PrintLine(fpSchema, "<xs:complexType name=\"FeatureCollectionType\">");
        PrintLine(fpSchema, "  <xs:complexContent>");
        PrintLine(
            fpSchema,
            "    <xs:extension base=\"gml:AbstractFeatureCollectionType\">");
        PrintLine(fpSchema, "      <xs:attribute name=\"lockId\" "
                            "type=\"xs:string\" use=\"optional\"/>");
        PrintLine(fpSchema, "      <xs:attribute name=\"scope\" "
                            "type=\"xs:string\" use=\"optional\"/>");
        PrintLine(fpSchema, "    </xs:extension>");
        PrintLine(fpSchema, "  </xs:complexContent>");
        PrintLine(fpSchema, "</xs:complexType>");
    }

    // Define the schema for each layer.
    for (int iLayer = 0; iLayer < nLayerCount; iLayer++)
    {
        OGRFeatureDefn *poFDefn = papoLayers[iLayer]->GetLayerDefn();

        // Emit initial stuff for a feature type.
        if (IsGML32Output())
        {
            PrintLine(fpSchema,
                      "<xs:element name=\"%s\" type=\"%s:%s_Type\" "
                      "substitutionGroup=\"gml:AbstractFeature\"/>",
                      poFDefn->GetName(), pszPrefix, poFDefn->GetName());
        }
        else
        {
            PrintLine(fpSchema,
                      "<xs:element name=\"%s\" type=\"%s:%s_Type\" "
                      "substitutionGroup=\"gml:_Feature\"/>",
                      poFDefn->GetName(), pszPrefix, poFDefn->GetName());
        }

        PrintLine(fpSchema, "<xs:complexType name=\"%s_Type\">",
                  poFDefn->GetName());
        PrintLine(fpSchema, "  <xs:complexContent>");
        PrintLine(fpSchema,
                  "    <xs:extension base=\"gml:AbstractFeatureType\">");
        PrintLine(fpSchema, "      <xs:sequence>");

        for (int iGeomField = 0; iGeomField < poFDefn->GetGeomFieldCount();
             iGeomField++)
        {
            OGRGeomFieldDefn *poFieldDefn =
                poFDefn->GetGeomFieldDefn(iGeomField);

            // Define the geometry attribute.
            const char *pszGeometryTypeName = "GeometryPropertyType";
            const char *pszGeomTypeComment = "";
            OGRwkbGeometryType eGType = wkbFlatten(poFieldDefn->GetType());
            switch (eGType)
            {
                case wkbPoint:
                    pszGeometryTypeName = "PointPropertyType";
                    break;
                case wkbLineString:
                case wkbCircularString:
                case wkbCompoundCurve:
                    if (IsGML3Output())
                    {
                        if (eGType == wkbLineString)
                            pszGeomTypeComment =
                                " <!-- restricted to LineString -->";
                        else if (eGType == wkbCircularString)
                            pszGeomTypeComment =
                                " <!-- contains CircularString -->";
                        else if (eGType == wkbCompoundCurve)
                            pszGeomTypeComment =
                                " <!-- contains CompoundCurve -->";
                        pszGeometryTypeName = "CurvePropertyType";
                    }
                    else
                        pszGeometryTypeName = "LineStringPropertyType";
                    break;
                case wkbPolygon:
                case wkbCurvePolygon:
                    if (IsGML3Output())
                    {
                        if (eGType == wkbPolygon)
                            pszGeomTypeComment =
                                " <!-- restricted to Polygon -->";
                        else if (eGType == wkbCurvePolygon)
                            pszGeomTypeComment =
                                " <!-- contains CurvePolygon -->";
                        pszGeometryTypeName = "SurfacePropertyType";
                    }
                    else
                        pszGeometryTypeName = "PolygonPropertyType";
                    break;
                case wkbMultiPoint:
                    pszGeometryTypeName = "MultiPointPropertyType";
                    break;
                case wkbMultiLineString:
                case wkbMultiCurve:
                    if (IsGML3Output())
                    {
                        if (eGType == wkbMultiLineString)
                            pszGeomTypeComment =
                                " <!-- restricted to MultiLineString -->";
                        else if (eGType == wkbMultiCurve)
                            pszGeomTypeComment =
                                " <!-- contains non-linear MultiCurve -->";
                        pszGeometryTypeName = "MultiCurvePropertyType";
                    }
                    else
                        pszGeometryTypeName = "MultiLineStringPropertyType";
                    break;
                case wkbMultiPolygon:
                case wkbMultiSurface:
                    if (IsGML3Output())
                    {
                        if (eGType == wkbMultiPolygon)
                            pszGeomTypeComment =
                                " <!-- restricted to MultiPolygon -->";
                        else if (eGType == wkbMultiSurface)
                            pszGeomTypeComment =
                                " <!-- contains non-linear MultiSurface -->";
                        pszGeometryTypeName = "MultiSurfacePropertyType";
                    }
                    else
                        pszGeometryTypeName = "MultiPolygonPropertyType";
                    break;
                case wkbGeometryCollection:
                    pszGeometryTypeName = "MultiGeometryPropertyType";
                    break;
                default:
                    break;
            }

            const auto poSRS = poFieldDefn->GetSpatialRef();
            std::string osSRSNameComment;
            if (poSRS)
            {
                bool bCoordSwap = false;
                char *pszSRSName =
                    GML_GetSRSName(poSRS, GetSRSNameFormat(), &bCoordSwap);
                if (pszSRSName[0])
                {
                    osSRSNameComment = "<!--";
                    osSRSNameComment += pszSRSName;
                    osSRSNameComment += " -->";
                }
                CPLFree(pszSRSName);
            }

            int nMinOccurs = poFieldDefn->IsNullable() ? 0 : 1;
            const auto &oCoordPrec = poFieldDefn->GetCoordinatePrecision();
            if (oCoordPrec.dfXYResolution ==
                    OGRGeomCoordinatePrecision::UNKNOWN &&
                oCoordPrec.dfZResolution == OGRGeomCoordinatePrecision::UNKNOWN)
            {
                PrintLine(
                    fpSchema,
                    "        <xs:element name=\"%s\" type=\"gml:%s\" "
                    "nillable=\"true\" minOccurs=\"%d\" maxOccurs=\"1\"/>%s%s",
                    poFieldDefn->GetNameRef(), pszGeometryTypeName, nMinOccurs,
                    pszGeomTypeComment, osSRSNameComment.c_str());
            }
            else
            {
                PrintLine(fpSchema,
                          "        <xs:element name=\"%s\" type=\"gml:%s\" "
                          "nillable=\"true\" minOccurs=\"%d\" maxOccurs=\"1\">",
                          poFieldDefn->GetNameRef(), pszGeometryTypeName,
                          nMinOccurs);
                PrintLine(fpSchema, "          <xs:annotation>");
                PrintLine(fpSchema, "            <xs:appinfo "
                                    "source=\"http://ogr.maptools.org/\">");
                if (oCoordPrec.dfXYResolution !=
                    OGRGeomCoordinatePrecision::UNKNOWN)
                {
                    PrintLine(fpSchema,
                              "              "
                              "<ogr:xy_coordinate_resolution>%g</"
                              "ogr:xy_coordinate_resolution>",
                              oCoordPrec.dfXYResolution);
                }
                if (oCoordPrec.dfZResolution !=
                    OGRGeomCoordinatePrecision::UNKNOWN)
                {
                    PrintLine(fpSchema,
                              "              "
                              "<ogr:z_coordinate_resolution>%g</"
                              "ogr:z_coordinate_resolution>",
                              oCoordPrec.dfZResolution);
                }
                PrintLine(fpSchema, "            </xs:appinfo>");
                PrintLine(fpSchema, "          </xs:annotation>");
                PrintLine(fpSchema, "        </xs:element>%s%s",
                          pszGeomTypeComment, osSRSNameComment.c_str());
            }
        }

        // Emit each of the attributes.
        for (int iField = 0; iField < poFDefn->GetFieldCount(); iField++)
        {
            OGRFieldDefn *poFieldDefn = poFDefn->GetFieldDefn(iField);

            if (IsGML3Output() &&
                strcmp(poFieldDefn->GetNameRef(), "gml_id") == 0)
                continue;
            else if (!IsGML3Output() &&
                     strcmp(poFieldDefn->GetNameRef(), "fid") == 0)
                continue;

            const auto AddComment = [this, fpSchema, poFieldDefn]()
            {
                if (!poFieldDefn->GetComment().empty())
                {
                    char *pszComment = CPLEscapeString(
                        poFieldDefn->GetComment().c_str(), -1, CPLES_XML);
                    PrintLine(fpSchema,
                              "          "
                              "<xs:annotation><xs:documentation>%s</"
                              "xs:documentation></xs:annotation>",
                              pszComment);
                    CPLFree(pszComment);
                }
            };

            int nMinOccurs = poFieldDefn->IsNullable() ? 0 : 1;
            const OGRFieldType eType = poFieldDefn->GetType();
            if (eType == OFTInteger || eType == OFTIntegerList)
            {
                int nWidth =
                    poFieldDefn->GetWidth() > 0 ? poFieldDefn->GetWidth() : 16;

                PrintLine(fpSchema,
                          "        <xs:element name=\"%s\" nillable=\"true\" "
                          "minOccurs=\"%d\" maxOccurs=\"%s\">",
                          poFieldDefn->GetNameRef(), nMinOccurs,
                          eType == OFTIntegerList ? "unbounded" : "1");
                AddComment();
                PrintLine(fpSchema, "          <xs:simpleType>");
                if (poFieldDefn->GetSubType() == OFSTBoolean)
                {
                    PrintLine(
                        fpSchema,
                        "            <xs:restriction base=\"xs:boolean\">");
                }
                else if (poFieldDefn->GetSubType() == OFSTInt16)
                {
                    PrintLine(fpSchema,
                              "            <xs:restriction base=\"xs:short\">");
                }
                else
                {
                    PrintLine(
                        fpSchema,
                        "            <xs:restriction base=\"xs:integer\">");
                    PrintLine(fpSchema,
                              "              <xs:totalDigits value=\"%d\"/>",
                              nWidth);
                }
                PrintLine(fpSchema, "            </xs:restriction>");
                PrintLine(fpSchema, "          </xs:simpleType>");
                PrintLine(fpSchema, "        </xs:element>");
            }
            else if (eType == OFTInteger64 || eType == OFTInteger64List)
            {
                int nWidth =
                    poFieldDefn->GetWidth() > 0 ? poFieldDefn->GetWidth() : 16;

                PrintLine(fpSchema,
                          "        <xs:element name=\"%s\" nillable=\"true\" "
                          "minOccurs=\"%d\" maxOccurs=\"%s\">",
                          poFieldDefn->GetNameRef(), nMinOccurs,
                          eType == OFTInteger64List ? "unbounded" : "1");
                AddComment();
                PrintLine(fpSchema, "          <xs:simpleType>");
                if (poFieldDefn->GetSubType() == OFSTBoolean)
                {
                    PrintLine(
                        fpSchema,
                        "            <xs:restriction base=\"xs:boolean\">");
                }
                else if (poFieldDefn->GetSubType() == OFSTInt16)
                {
                    PrintLine(fpSchema,
                              "            <xs:restriction base=\"xs:short\">");
                }
                else
                {
                    PrintLine(fpSchema,
                              "            <xs:restriction base=\"xs:long\">");
                    PrintLine(fpSchema,
                              "              <xs:totalDigits value=\"%d\"/>",
                              nWidth);
                }
                PrintLine(fpSchema, "            </xs:restriction>");
                PrintLine(fpSchema, "          </xs:simpleType>");
                PrintLine(fpSchema, "        </xs:element>");
            }
            else if (eType == OFTReal || eType == OFTRealList)
            {
                int nWidth, nDecimals;

                nWidth = poFieldDefn->GetWidth();
                nDecimals = poFieldDefn->GetPrecision();

                PrintLine(fpSchema,
                          "        <xs:element name=\"%s\" nillable=\"true\" "
                          "minOccurs=\"%d\" maxOccurs=\"%s\">",
                          poFieldDefn->GetNameRef(), nMinOccurs,
                          eType == OFTRealList ? "unbounded" : "1");
                AddComment();
                PrintLine(fpSchema, "          <xs:simpleType>");
                if (poFieldDefn->GetSubType() == OFSTFloat32)
                    PrintLine(fpSchema,
                              "            <xs:restriction base=\"xs:float\">");
                else
                    PrintLine(
                        fpSchema,
                        "            <xs:restriction base=\"xs:decimal\">");
                if (nWidth > 0)
                {
                    PrintLine(fpSchema,
                              "              <xs:totalDigits value=\"%d\"/>",
                              nWidth);
                    PrintLine(fpSchema,
                              "              <xs:fractionDigits value=\"%d\"/>",
                              nDecimals);
                }
                PrintLine(fpSchema, "            </xs:restriction>");
                PrintLine(fpSchema, "          </xs:simpleType>");
                PrintLine(fpSchema, "        </xs:element>");
            }
            else if (eType == OFTString || eType == OFTStringList)
            {
                PrintLine(fpSchema,
                          "        <xs:element name=\"%s\" nillable=\"true\" "
                          "minOccurs=\"%d\" maxOccurs=\"%s\">",
                          poFieldDefn->GetNameRef(), nMinOccurs,
                          eType == OFTStringList ? "unbounded" : "1");
                AddComment();
                PrintLine(fpSchema, "          <xs:simpleType>");
                PrintLine(fpSchema,
                          "            <xs:restriction base=\"xs:string\">");
                if (poFieldDefn->GetWidth() != 0)
                {
                    PrintLine(fpSchema,
                              "              <xs:maxLength value=\"%d\"/>",
                              poFieldDefn->GetWidth());
                }
                PrintLine(fpSchema, "            </xs:restriction>");
                PrintLine(fpSchema, "          </xs:simpleType>");
                PrintLine(fpSchema, "        </xs:element>");
            }
            else if (eType == OFTDate)
            {
                PrintLine(fpSchema,
                          "        <xs:element name=\"%s\" nillable=\"true\" "
                          "minOccurs=\"%d\" maxOccurs=\"1\" type=\"xs:date\">",
                          poFieldDefn->GetNameRef(), nMinOccurs);
                AddComment();
                PrintLine(fpSchema, "        </xs:element>");
            }
            else if (eType == OFTTime)
            {
                PrintLine(fpSchema,
                          "        <xs:element name=\"%s\" nillable=\"true\" "
                          "minOccurs=\"%d\" maxOccurs=\"1\" type=\"xs:time\">",
                          poFieldDefn->GetNameRef(), nMinOccurs);
                AddComment();
                PrintLine(fpSchema, "        </xs:element>");
            }
            else if (eType == OFTDateTime)
            {
                PrintLine(
                    fpSchema,
                    "        <xs:element name=\"%s\" nillable=\"true\" "
                    "minOccurs=\"%d\" maxOccurs=\"1\" type=\"xs:dateTime\">",
                    poFieldDefn->GetNameRef(), nMinOccurs);
                AddComment();
                PrintLine(fpSchema, "        </xs:element>");
            }
            else
            {
                // TODO.
            }
        }  // Next field.

        // Finish off feature type.
        PrintLine(fpSchema, "      </xs:sequence>");
        PrintLine(fpSchema, "    </xs:extension>");
        PrintLine(fpSchema, "  </xs:complexContent>");
        PrintLine(fpSchema, "</xs:complexType>");
    }  // Next layer.

    PrintLine(fpSchema, "</xs:schema>");

    // Move schema to the start of the file.
    if (fpSchema == fpOutput)
    {
        // Read the schema into memory.
        int nSchemaSize = static_cast<int>(VSIFTellL(fpSchema) - nSchemaStart);
        char *pszSchema = static_cast<char *>(CPLMalloc(nSchemaSize + 1));

        VSIFSeekL(fpSchema, nSchemaStart, SEEK_SET);

        VSIFReadL(pszSchema, 1, nSchemaSize, fpSchema);
        pszSchema[nSchemaSize] = '\0';

        // Move file data down by "schema size" bytes from after <?xml> header
        // so we have room insert the schema.  Move in pretty big chunks.
        int nChunkSize = std::min(nSchemaStart - nSchemaInsertLocation, 250000);
        char *pszChunk = static_cast<char *>(CPLMalloc(nChunkSize));

        for (int nEndOfUnmovedData = nSchemaStart;
             nEndOfUnmovedData > nSchemaInsertLocation;)
        {
            const int nBytesToMove =
                std::min(nChunkSize, nEndOfUnmovedData - nSchemaInsertLocation);

            VSIFSeekL(fpSchema, nEndOfUnmovedData - nBytesToMove, SEEK_SET);
            VSIFReadL(pszChunk, 1, nBytesToMove, fpSchema);
            VSIFSeekL(fpSchema, nEndOfUnmovedData - nBytesToMove + nSchemaSize,
                      SEEK_SET);
            VSIFWriteL(pszChunk, 1, nBytesToMove, fpSchema);

            nEndOfUnmovedData -= nBytesToMove;
        }

        CPLFree(pszChunk);

        // Write the schema in the opened slot.
        VSIFSeekL(fpSchema, nSchemaInsertLocation, SEEK_SET);
        VSIFWriteL(pszSchema, 1, nSchemaSize, fpSchema);

        VSIFSeekL(fpSchema, 0, SEEK_END);

        nBoundedByLocation += nSchemaSize;

        CPLFree(pszSchema);
    }
    else
    {
        // Close external schema files.
        VSIFCloseL(fpSchema);
    }
}

/************************************************************************/
/*                            PrintLine()                               */
/************************************************************************/

void OGRGMLDataSource::PrintLine(VSILFILE *fp, const char *fmt, ...)
{
    CPLString osWork;
    va_list args;

    va_start(args, fmt);
    osWork.vPrintf(fmt, args);
    va_end(args);

#ifdef _WIN32
    constexpr const char *pszEOL = "\r\n";
#else
    constexpr const char *pszEOL = "\n";
#endif

    if (VSIFWriteL(osWork.data(), osWork.size(), 1, fp) != 1 ||
        VSIFWriteL(pszEOL, strlen(pszEOL), 1, fp) != 1)
    {
        m_bWriteError = true;
        ReportError(CE_Failure, CPLE_FileIO, "Could not write line %s",
                    osWork.c_str());
    }
}

/************************************************************************/
/*                     OGRGMLSingleFeatureLayer                         */
/************************************************************************/

class OGRGMLSingleFeatureLayer final : public OGRLayer
{
  private:
    const int nVal;
    OGRFeatureDefn *poFeatureDefn = nullptr;
    int iNextShapeId = 0;

    CPL_DISALLOW_COPY_ASSIGN(OGRGMLSingleFeatureLayer)

  public:
    explicit OGRGMLSingleFeatureLayer(int nVal);

    virtual ~OGRGMLSingleFeatureLayer()
    {
        poFeatureDefn->Release();
    }

    virtual void ResetReading() override
    {
        iNextShapeId = 0;
    }

    virtual OGRFeature *GetNextFeature() override;

    virtual OGRFeatureDefn *GetLayerDefn() override
    {
        return poFeatureDefn;
    }

    virtual int TestCapability(const char *) override
    {
        return FALSE;
    }
};

/************************************************************************/
/*                      OGRGMLSingleFeatureLayer()                      */
/************************************************************************/

OGRGMLSingleFeatureLayer::OGRGMLSingleFeatureLayer(int nValIn)
    : nVal(nValIn), poFeatureDefn(new OGRFeatureDefn("SELECT")), iNextShapeId(0)
{
    poFeatureDefn->Reference();
    OGRFieldDefn oField("Validates", OFTInteger);
    poFeatureDefn->AddFieldDefn(&oField);
}

/************************************************************************/
/*                           GetNextFeature()                           */
/************************************************************************/

OGRFeature *OGRGMLSingleFeatureLayer::GetNextFeature()
{
    if (iNextShapeId != 0)
        return nullptr;

    OGRFeature *poFeature = new OGRFeature(poFeatureDefn);
    poFeature->SetField(0, nVal);
    poFeature->SetFID(iNextShapeId++);
    return poFeature;
}

/************************************************************************/
/*                            ExecuteSQL()                              */
/************************************************************************/

OGRLayer *OGRGMLDataSource::ExecuteSQL(const char *pszSQLCommand,
                                       OGRGeometry *poSpatialFilter,
                                       const char *pszDialect)
{
    if (poReader != nullptr && EQUAL(pszSQLCommand, "SELECT ValidateSchema()"))
    {
        bool bIsValid = false;
        if (!osXSDFilename.empty())
        {
            CPLErrorReset();
            bIsValid =
                CPL_TO_BOOL(CPLValidateXML(osFilename, osXSDFilename, nullptr));
        }
        return new OGRGMLSingleFeatureLayer(bIsValid);
    }

    return GDALDataset::ExecuteSQL(pszSQLCommand, poSpatialFilter, pszDialect);
}

/************************************************************************/
/*                          ReleaseResultSet()                          */
/************************************************************************/

void OGRGMLDataSource::ReleaseResultSet(OGRLayer *poResultsSet)
{
    delete poResultsSet;
}

/************************************************************************/
/*                      FindAndParseTopElements()                       */
/************************************************************************/

void OGRGMLDataSource::FindAndParseTopElements(VSILFILE *fp)
{
    // Build a shortened XML file that contain only the global
    // boundedBy element, so as to be able to parse it easily.

    char szStartTag[128];
    char *pszXML = static_cast<char *>(CPLMalloc(8192 + 128 + 3 + 1));
    VSIFSeekL(fp, 0, SEEK_SET);
    int nRead = static_cast<int>(VSIFReadL(pszXML, 1, 8192, fp));
    pszXML[nRead] = 0;

    const char *pszStartTag = strchr(pszXML, '<');
    if (pszStartTag != nullptr)
    {
        while (pszStartTag != nullptr && pszStartTag[1] == '?')
            pszStartTag = strchr(pszStartTag + 1, '<');

        if (pszStartTag != nullptr)
        {
            pszStartTag++;
            const char *pszEndTag = nullptr;
            for (const char *pszIter = pszStartTag; *pszIter != '\0'; pszIter++)
            {
                if (isspace(static_cast<unsigned char>(*pszIter)) ||
                    *pszIter == '>')
                {
                    pszEndTag = pszIter;
                    break;
                }
            }
            if (pszEndTag != nullptr && pszEndTag - pszStartTag < 128)
            {
                memcpy(szStartTag, pszStartTag, pszEndTag - pszStartTag);
                szStartTag[pszEndTag - pszStartTag] = '\0';
            }
            else
                pszStartTag = nullptr;
        }
    }

    const char *pszFeatureMember = strstr(pszXML, "<gml:featureMember");
    if (pszFeatureMember == nullptr)
        pszFeatureMember = strstr(pszXML, ":featureMember>");
    if (pszFeatureMember == nullptr)
        pszFeatureMember = strstr(pszXML, "<wfs:member>");

    // Is it a standalone geometry ?
    if (pszFeatureMember == nullptr && pszStartTag != nullptr)
    {
        const char *pszElement = szStartTag;
        const char *pszColon = strchr(pszElement, ':');
        if (pszColon)
            pszElement = pszColon + 1;
        if (OGRGMLIsGeometryElement(pszElement))
        {
            VSIFSeekL(fp, 0, SEEK_END);
            const auto nLen = VSIFTellL(fp);
            if (nLen < 10 * 1024 * 1024U)
            {
                VSIFSeekL(fp, 0, SEEK_SET);
                std::string osBuffer;
                try
                {
                    osBuffer.resize(static_cast<size_t>(nLen));
                    VSIFReadL(&osBuffer[0], 1, osBuffer.size(), fp);
                }
                catch (const std::exception &)
                {
                }
                CPLPushErrorHandler(CPLQuietErrorHandler);
                CPLXMLNode *psTree = CPLParseXMLString(osBuffer.data());
                CPLPopErrorHandler();
                CPLErrorReset();
                if (psTree)
                {
                    m_poStandaloneGeom.reset(GML2OGRGeometry_XMLNode(
                        psTree, false, 0, 0, false, true, false));

                    if (m_poStandaloneGeom)
                    {
                        for (CPLXMLNode *psCur = psTree; psCur;
                             psCur = psCur->psNext)
                        {
                            if (psCur->eType == CXT_Element &&
                                strcmp(psCur->pszValue, szStartTag) == 0)
                            {
                                const char *pszSRSName =
                                    CPLGetXMLValue(psCur, "srsName", nullptr);
                                if (pszSRSName)
                                {
                                    m_oStandaloneGeomSRS.SetFromUserInput(
                                        pszSRSName,
                                        OGRSpatialReference::
                                            SET_FROM_USER_INPUT_LIMITATIONS_get());
                                    m_oStandaloneGeomSRS.SetAxisMappingStrategy(
                                        OAMS_TRADITIONAL_GIS_ORDER);
                                    if (GML_IsSRSLatLongOrder(pszSRSName))
                                        m_poStandaloneGeom->swapXY();
                                }
                                break;
                            }
                        }
                    }
                    CPLDestroyXMLNode(psTree);
                }
            }
        }
    }

    const char *pszDescription = strstr(pszXML, "<gml:description>");
    if (pszDescription &&
        (pszFeatureMember == nullptr || pszDescription < pszFeatureMember))
    {
        pszDescription += strlen("<gml:description>");
        const char *pszEndDescription =
            strstr(pszDescription, "</gml:description>");
        if (pszEndDescription)
        {
            CPLString osTmp(pszDescription);
            osTmp.resize(pszEndDescription - pszDescription);
            char *pszTmp = CPLUnescapeString(osTmp, nullptr, CPLES_XML);
            if (pszTmp)
                SetMetadataItem("DESCRIPTION", pszTmp);
            CPLFree(pszTmp);
        }
    }

    const char *l_pszName = strstr(pszXML, "<gml:name");
    if (l_pszName)
        l_pszName = strchr(l_pszName, '>');
    if (l_pszName &&
        (pszFeatureMember == nullptr || l_pszName < pszFeatureMember))
    {
        l_pszName++;
        const char *pszEndName = strstr(l_pszName, "</gml:name>");
        if (pszEndName)
        {
            CPLString osTmp(l_pszName);
            osTmp.resize(pszEndName - l_pszName);
            char *pszTmp = CPLUnescapeString(osTmp, nullptr, CPLES_XML);
            if (pszTmp)
                SetMetadataItem("NAME", pszTmp);
            CPLFree(pszTmp);
        }
    }

    // Detect a few fields in gml: namespace inside features
    if (pszFeatureMember)
    {
        if (strstr(pszFeatureMember, "<gml:description>"))
            m_aosGMLExtraElements.push_back("description");
        if (strstr(pszFeatureMember, "<gml:identifier>") ||
            strstr(pszFeatureMember, "<gml:identifier "))
            m_aosGMLExtraElements.push_back("identifier");
        if (strstr(pszFeatureMember, "<gml:name>") ||
            strstr(pszFeatureMember, "<gml:name "))
            m_aosGMLExtraElements.push_back("name");
    }

    char *pszEndBoundedBy = strstr(pszXML, "</wfs:boundedBy>");
    bool bWFSBoundedBy = false;
    if (pszEndBoundedBy != nullptr)
        bWFSBoundedBy = true;
    else
        pszEndBoundedBy = strstr(pszXML, "</gml:boundedBy>");
    if (pszStartTag != nullptr && pszEndBoundedBy != nullptr)
    {
        char szSRSName[128] = {};

        // Find a srsName somewhere for some WFS 2.0 documents that have not it
        // set at the <wfs:boundedBy> element. e.g.
        // http://geoserv.weichand.de:8080/geoserver/wfs?SERVICE=WFS&REQUEST=GetFeature&VERSION=2.0.0&TYPENAME=bvv:gmd_ex
        if (bIsWFS)
        {
            ExtractSRSName(pszXML, szSRSName, sizeof(szSRSName));
        }

        pszEndBoundedBy[strlen("</gml:boundedBy>")] = '\0';
        strcat(pszXML, "</");
        strcat(pszXML, szStartTag);
        strcat(pszXML, ">");

        CPLPushErrorHandler(CPLQuietErrorHandler);
        CPLXMLNode *psXML = CPLParseXMLString(pszXML);
        CPLPopErrorHandler();
        CPLErrorReset();
        if (psXML != nullptr)
        {
            CPLXMLNode *psBoundedBy = nullptr;
            CPLXMLNode *psIter = psXML;
            while (psIter != nullptr)
            {
                psBoundedBy = CPLGetXMLNode(
                    psIter, bWFSBoundedBy ? "wfs:boundedBy" : "gml:boundedBy");
                if (psBoundedBy != nullptr)
                    break;
                psIter = psIter->psNext;
            }

            const char *pszLowerCorner = nullptr;
            const char *pszUpperCorner = nullptr;
            const char *pszSRSName = nullptr;
            if (psBoundedBy != nullptr)
            {
                CPLXMLNode *psEnvelope =
                    CPLGetXMLNode(psBoundedBy, "gml:Envelope");
                if (psEnvelope)
                {
                    pszSRSName = CPLGetXMLValue(psEnvelope, "srsName", nullptr);
                    pszLowerCorner =
                        CPLGetXMLValue(psEnvelope, "gml:lowerCorner", nullptr);
                    pszUpperCorner =
                        CPLGetXMLValue(psEnvelope, "gml:upperCorner", nullptr);
                }
            }

            if (bIsWFS && pszSRSName == nullptr && pszLowerCorner != nullptr &&
                pszUpperCorner != nullptr && szSRSName[0] != '\0')
            {
                pszSRSName = szSRSName;
            }

            if (pszSRSName != nullptr && pszLowerCorner != nullptr &&
                pszUpperCorner != nullptr)
            {
                char **papszLC = CSLTokenizeString(pszLowerCorner);
                char **papszUC = CSLTokenizeString(pszUpperCorner);
                if (CSLCount(papszLC) >= 2 && CSLCount(papszUC) >= 2)
                {
                    CPLDebug("GML", "Global SRS = %s", pszSRSName);

                    if (STARTS_WITH(pszSRSName,
                                    "http://www.opengis.net/gml/srs/epsg.xml#"))
                    {
                        std::string osWork;
                        osWork.assign("EPSG:", 5);
                        osWork.append(pszSRSName + 40);
                        poReader->SetGlobalSRSName(osWork.c_str());
                    }
                    else
                    {
                        poReader->SetGlobalSRSName(pszSRSName);
                    }

                    const double dfMinX = CPLAtofM(papszLC[0]);
                    const double dfMinY = CPLAtofM(papszLC[1]);
                    const double dfMaxX = CPLAtofM(papszUC[0]);
                    const double dfMaxY = CPLAtofM(papszUC[1]);

                    SetExtents(dfMinX, dfMinY, dfMaxX, dfMaxY);
                }
                CSLDestroy(papszLC);
                CSLDestroy(papszUC);
            }

            CPLDestroyXMLNode(psXML);
        }
    }

    CPLFree(pszXML);
}

/************************************************************************/
/*                             SetExtents()                             */
/************************************************************************/

void OGRGMLDataSource::SetExtents(double dfMinX, double dfMinY, double dfMaxX,
                                  double dfMaxY)
{
    sBoundingRect.MinX = dfMinX;
    sBoundingRect.MinY = dfMinY;
    sBoundingRect.MaxX = dfMaxX;
    sBoundingRect.MaxY = dfMaxY;
}

/************************************************************************/
/*                             GetAppPrefix()                           */
/************************************************************************/

const char *OGRGMLDataSource::GetAppPrefix() const
{
    return CSLFetchNameValueDef(papszCreateOptions, "PREFIX", "ogr");
}

/************************************************************************/
/*                            RemoveAppPrefix()                         */
/************************************************************************/

bool OGRGMLDataSource::RemoveAppPrefix() const
{
    if (CPLTestBool(
            CSLFetchNameValueDef(papszCreateOptions, "STRIP_PREFIX", "FALSE")))
        return true;
    const char *pszPrefix = GetAppPrefix();
    return pszPrefix[0] == '\0';
}

/************************************************************************/
/*                        WriteFeatureBoundedBy()                       */
/************************************************************************/

bool OGRGMLDataSource::WriteFeatureBoundedBy() const
{
    return CPLTestBool(CSLFetchNameValueDef(
        papszCreateOptions, "WRITE_FEATURE_BOUNDED_BY", "TRUE"));
}

/************************************************************************/
/*                          GetSRSDimensionLoc()                        */
/************************************************************************/

const char *OGRGMLDataSource::GetSRSDimensionLoc() const
{
    return CSLFetchNameValue(papszCreateOptions, "SRSDIMENSION_LOC");
}

/************************************************************************/
/*                        GMLFeatureCollection()                     */
/************************************************************************/

bool OGRGMLDataSource::GMLFeatureCollection() const
{
    return IsGML3Output() &&
           CPLFetchBool(papszCreateOptions, "GML_FEATURE_COLLECTION", false);
}
