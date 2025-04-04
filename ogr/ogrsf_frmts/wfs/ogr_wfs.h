/******************************************************************************
 *
 * Project:  WFS Translator
 * Purpose:  Definition of classes for OGR WFS driver.
 * Author:   Even Rouault, even dot rouault at spatialys.com
 *
 ******************************************************************************
 * Copyright (c) 2010-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef OGR_WFS_H_INCLUDED
#define OGR_WFS_H_INCLUDED

#include <vector>
#include <set>
#include <map>

#include "cpl_minixml.h"
#include "ogrsf_frmts.h"
#include "gmlfeature.h"
#include "cpl_http.h"
#include "ogr_swq.h"

const CPLXMLNode *WFSFindNode(const CPLXMLNode *psXML, const char *pszRootName);

const char *FindSubStringInsensitive(const char *pszStr, const char *pszSubStr);

CPLString WFS_EscapeURL(const char *pszURL);
CPLString WFS_DecodeURL(const CPLString &osSrc);

class OGRWFSSortDesc
{
  public:
    CPLString osColumn;
    bool bAsc;

    OGRWFSSortDesc(const CPLString &osColumnIn, int bAscIn)
        : osColumn(osColumnIn), bAsc(CPL_TO_BOOL(bAscIn))
    {
    }
};

/************************************************************************/
/*                             OGRWFSLayer                              */
/************************************************************************/

class OGRWFSDataSource;

class OGRWFSLayer final : public OGRLayer
{
    OGRWFSDataSource *poDS;

    OGRFeatureDefn *poFeatureDefn;
    bool bGotApproximateLayerDefn;
    GMLFeatureClass *poGMLFeatureClass;

    int bAxisOrderAlreadyInverted;
    OGRSpatialReference *m_poSRS;
    std::string m_osSRSName{};

    char *pszBaseURL;
    char *pszName;
    char *pszNS;
    char *pszNSVal;

    bool bStreamingDS;
    GDALDataset *poBaseDS;
    OGRLayer *poBaseLayer;
    bool bHasFetched;
    bool bReloadNeeded;

    CPLString osGeometryColumnName;
    OGRwkbGeometryType eGeomType;
    GIntBig nFeatures;
    GIntBig m_nNumberMatched = -1;
    bool m_bHasReadAtLeastOneFeatureInThisPage = false;
    bool bCountFeaturesInGetNextFeature;

    int CanRunGetFeatureCountAndGetExtentTogether();

    CPLString MakeGetFeatureURL(int nMaxFeatures, int bRequestHits);
    bool MustRetryIfNonCompliantServer(const char *pszServerAnswer);
    GDALDataset *FetchGetFeature(int nMaxFeatures);
    OGRFeatureDefn *DescribeFeatureType();
    GIntBig ExecuteGetFeatureResultTypeHits();

    OGREnvelope m_oWGS84Extents{};
    OGREnvelope m_oExtents{};

    OGRGeometry *poFetchedFilterGeom;

    CPLString osSQLWhere;
    CPLString osWFSWhere;

    CPLString osTargetNamespace;
    CPLString GetDescribeFeatureTypeURL(int bWithNS);

    int nExpectedInserts;
    CPLString osGlobalInsert;
    std::vector<CPLString> aosFIDList;

    bool bInTransaction;

    CPLString GetPostHeader();

    bool bUseFeatureIdAtLayerLevel;

    bool bPagingActive;
    int nPagingStartIndex;
    int nFeatureRead;

    OGRFeatureDefn *BuildLayerDefnFromFeatureClass(GMLFeatureClass *poClass);

    char *pszRequiredOutputFormat;

    std::vector<OGRWFSSortDesc> aoSortColumns;

    std::vector<std::string> m_aosSupportedCRSList{};
    OGRLayer::GetSupportedSRSListRetType m_apoSupportedCRSList{};

    std::string m_osTmpDir{};

  public:
    OGRWFSLayer(OGRWFSDataSource *poDS, OGRSpatialReference *poSRS,
                int bAxisOrderAlreadyInverted, const char *pszBaseURL,
                const char *pszName, const char *pszNS, const char *pszNSVal);

    virtual ~OGRWFSLayer();

    OGRWFSLayer *Clone();

    const char *GetName() override
    {
        return pszName;
    }

    virtual void ResetReading() override;
    virtual OGRFeature *GetNextFeature() override;
    virtual OGRFeature *GetFeature(GIntBig nFID) override;

    virtual OGRFeatureDefn *GetLayerDefn() override;

    virtual int TestCapability(const char *) override;

    OGRErr ISetSpatialFilter(int iGeomField,
                             const OGRGeometry *poGeom) override;

    virtual OGRErr SetAttributeFilter(const char *) override;

    virtual GIntBig GetFeatureCount(int bForce = TRUE) override;

    void SetExtents(double dfMinX, double dfMinY, double dfMaxX, double dfMaxY);
    void SetWGS84Extents(double dfMinX, double dfMinY, double dfMaxX,
                         double dfMaxY);
    virtual OGRErr IGetExtent(int iGeomField, OGREnvelope *psExtent,
                              bool bForce) override;

    virtual OGRErr ICreateFeature(OGRFeature *poFeature) override;
    virtual OGRErr ISetFeature(OGRFeature *poFeature) override;
    virtual OGRErr DeleteFeature(GIntBig nFID) override;

    virtual OGRErr StartTransaction() override;
    virtual OGRErr CommitTransaction() override;
    virtual OGRErr RollbackTransaction() override;

    virtual OGRErr SetIgnoredFields(CSLConstList papszFields) override;

    int HasLayerDefn()
    {
        return poFeatureDefn != nullptr;
    }

    OGRFeatureDefn *ParseSchema(const CPLXMLNode *psSchema);
    OGRFeatureDefn *BuildLayerDefn(OGRFeatureDefn *poSrcFDefn = nullptr);

    OGRErr DeleteFromFilter(const std::string &osOGCFilter);

    const std::vector<CPLString> &GetLastInsertedFIDList()
    {
        return aosFIDList;
    }

    const char *GetShortName();

    void SetRequiredOutputFormat(const char *pszRequiredOutputFormatIn);

    const char *GetRequiredOutputFormat()
    {
        return pszRequiredOutputFormat;
    }

    void SetOrderBy(const std::vector<OGRWFSSortDesc> &aoSortColumnsIn);

    bool HasGotApproximateLayerDefn()
    {
        GetLayerDefn();
        return bGotApproximateLayerDefn;
    }

    const char *GetNamespacePrefix()
    {
        return pszNS;
    }

    const char *GetNamespaceName()
    {
        return pszNSVal;
    }

    void SetSupportedSRSList(
        std::vector<std::string> &&aosSupportedCRSList,
        OGRLayer::GetSupportedSRSListRetType &&apoSupportedCRSList)
    {
        m_aosSupportedCRSList = std::move(aosSupportedCRSList);
        m_apoSupportedCRSList = std::move(apoSupportedCRSList);
    }

    const OGRLayer::GetSupportedSRSListRetType &
    GetSupportedSRSList(int /*iGeomField*/) override
    {
        return m_apoSupportedCRSList;
    }

    OGRErr SetActiveSRS(int iGeomField,
                        const OGRSpatialReference *poSRS) override;

    const std::string &GetTmpDir() const
    {
        return m_osTmpDir;
    }
};

/************************************************************************/
/*                          OGRWFSJoinLayer                             */
/************************************************************************/

class OGRWFSJoinLayer final : public OGRLayer
{
    OGRWFSDataSource *poDS;
    OGRFeatureDefn *poFeatureDefn;

    CPLString osGlobalFilter;
    CPLString osSortBy;
    int bDistinct;
    std::set<CPLString> aoSetMD5;

    std::vector<OGRWFSLayer *> apoLayers;

    GDALDataset *poBaseDS;
    OGRLayer *poBaseLayer;
    bool bReloadNeeded;
    bool bHasFetched;

    bool bPagingActive;
    int nPagingStartIndex;
    int nFeatureRead;
    int nFeatureCountRequested;

    std::vector<CPLString> aoSrcFieldNames;
    std::vector<CPLString> aoSrcGeomFieldNames;

    CPLString osFeatureTypes;

    std::string m_osTmpDir{};

    OGRWFSJoinLayer(OGRWFSDataSource *poDS, const swq_select *psSelectInfo,
                    const CPLString &osGlobalFilter);
    CPLString MakeGetFeatureURL(int bRequestHits = FALSE);
    GDALDataset *FetchGetFeature();
    GIntBig ExecuteGetFeatureResultTypeHits();

  public:
    static OGRWFSJoinLayer *Build(OGRWFSDataSource *poDS,
                                  const swq_select *psSelectInfo);
    virtual ~OGRWFSJoinLayer();

    virtual void ResetReading() override;
    virtual OGRFeature *GetNextFeature() override;

    virtual OGRFeatureDefn *GetLayerDefn() override;

    virtual int TestCapability(const char *) override;

    virtual GIntBig GetFeatureCount(int bForce = TRUE) override;

    OGRErr ISetSpatialFilter(int iGeomField,
                             const OGRGeometry *poGeom) override;

    virtual OGRErr SetAttributeFilter(const char *) override;
};

/************************************************************************/
/*                           OGRWFSDataSource                           */
/************************************************************************/

class OGRWFSDataSource final : public GDALDataset
{
    bool bRewriteFile;
    CPLXMLNode *psFileXML;

    OGRWFSLayer **papoLayers;
    int nLayers;
    std::map<OGRLayer *, OGRLayer *> oMap;

    bool bUpdate;

    bool bGetFeatureSupportHits;
    CPLString osVersion;
    bool bNeedNAMESPACE;
    bool bHasMinOperators;
    bool bHasNullCheck;
    bool bPropertyIsNotEqualToSupported;
    bool bUseFeatureId;
    bool bGmlObjectIdNeedsGMLPrefix;
    bool bRequiresEnvelopeSpatialFilter;
    static bool DetectRequiresEnvelopeSpatialFilter(const CPLXMLNode *psRoot);

    bool bTransactionSupport;
    char **papszIdGenMethods;
    bool DetectTransactionSupport(const CPLXMLNode *psRoot);

    CPLString osBaseURL;
    CPLString osPostTransactionURL;

    CPLXMLNode *LoadFromFile(const char *pszFilename);

    bool bUseHttp10;

    char **papszHttpOptions;

    bool bPagingAllowed;
    int nPageSize;
    int nBaseStartIndex;
    bool DetectSupportPagingWFS2(const CPLXMLNode *psGetCapabilitiesResponse,
                                 const CPLXMLNode *psConfigurationRoot);

    bool bStandardJoinsWFS2;
    bool DetectSupportStandardJoinsWFS2(const CPLXMLNode *psRoot);

    bool bLoadMultipleLayerDefn;
    std::set<CPLString> aoSetAlreadyTriedLayers;

    CPLString osLayerMetadataCSV;
    CPLString osLayerMetadataTmpFileName;
    GDALDataset *poLayerMetadataDS;
    OGRLayer *poLayerMetadataLayer;

    CPLString osGetCapabilities;
    const char *apszGetCapabilities[2];
    GDALDataset *poLayerGetCapabilitiesDS;
    OGRLayer *poLayerGetCapabilitiesLayer;

    bool bKeepLayerNamePrefix;

    bool bEmptyAsNull;

    bool bInvertAxisOrderIfLatLong;
    CPLString osConsiderEPSGAsURN;
    bool bExposeGMLId;

    CPLHTTPResult *SendGetCapabilities(const char *pszBaseURL,
                                       CPLString &osTypeName);

    int GetLayerIndex(const char *pszName);

  public:
    OGRWFSDataSource();
    virtual ~OGRWFSDataSource();

    int Open(const char *pszFilename, int bUpdate,
             CSLConstList papszOpenOptions);

    virtual int GetLayerCount() override
    {
        return nLayers;
    }

    virtual OGRLayer *GetLayer(int) override;
    virtual OGRLayer *GetLayerByName(const char *pszLayerName) override;

    virtual OGRLayer *ExecuteSQL(const char *pszSQLCommand,
                                 OGRGeometry *poSpatialFilter,
                                 const char *pszDialect) override;
    virtual void ReleaseResultSet(OGRLayer *poResultsSet) override;

    bool UpdateMode() const
    {
        return bUpdate;
    }

    bool SupportTransactions() const
    {
        return bTransactionSupport;
    }

    void DisableSupportHits()
    {
        bGetFeatureSupportHits = false;
    }

    bool GetFeatureSupportHits() const
    {
        return bGetFeatureSupportHits;
    }

    const char *GetVersion()
    {
        return osVersion.c_str();
    }

    bool IsOldDeegree(const char *pszErrorString);

    bool GetNeedNAMESPACE() const
    {
        return bNeedNAMESPACE;
    }

    bool HasMinOperators() const
    {
        return bHasMinOperators;
    }

    bool HasNullCheck() const
    {
        return bHasNullCheck;
    }

    bool UseFeatureId() const
    {
        return bUseFeatureId;
    }

    bool RequiresEnvelopeSpatialFilter() const
    {
        return bRequiresEnvelopeSpatialFilter;
    }

    void SetGmlObjectIdNeedsGMLPrefix()
    {
        bGmlObjectIdNeedsGMLPrefix = true;
    }

    int DoesGmlObjectIdNeedGMLPrefix() const
    {
        return bGmlObjectIdNeedsGMLPrefix;
    }

    void SetPropertyIsNotEqualToUnSupported()
    {
        bPropertyIsNotEqualToSupported = false;
    }

    bool PropertyIsNotEqualToSupported() const
    {
        return bPropertyIsNotEqualToSupported;
    }

    CPLString GetPostTransactionURL();

    void SaveLayerSchema(const char *pszLayerName, const CPLXMLNode *psSchema);

    CPLHTTPResult *HTTPFetch(const char *pszURL, char **papszOptions);

    bool IsPagingAllowed() const
    {
        return bPagingAllowed;
    }

    int GetPageSize() const
    {
        return nPageSize;
    }

    int GetBaseStartIndex() const
    {
        return nBaseStartIndex;
    }

    void LoadMultipleLayerDefn(const char *pszLayerName, char *pszNS,
                               char *pszNSVal);

    bool GetKeepLayerNamePrefix() const
    {
        return bKeepLayerNamePrefix;
    }

    const CPLString &GetBaseURL()
    {
        return osBaseURL;
    }

    bool IsEmptyAsNull() const
    {
        return bEmptyAsNull;
    }

    bool InvertAxisOrderIfLatLong() const
    {
        return bInvertAxisOrderIfLatLong;
    }

    const CPLString &GetConsiderEPSGAsURN() const
    {
        return osConsiderEPSGAsURN;
    }

    bool ExposeGMLId() const
    {
        return bExposeGMLId;
    }

    virtual char **GetMetadataDomainList() override;
    virtual char **GetMetadata(const char *pszDomain = "") override;
};

#endif /* ndef OGR_WFS_H_INCLUDED */
