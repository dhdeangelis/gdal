/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Private definitions for OGR/PostgreSQL driver.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2000, Frank Warmerdam
 * Copyright (c) 2008-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef OGR_PG_H_INCLUDED
#define OGR_PG_H_INCLUDED

#include "ogrsf_frmts.h"
#include "libpq-fe.h"
#include "cpl_string.h"

#include "ogrpgutility.h"
#include "ogr_pgdump.h"

#include <map>
#include <optional>
#include <vector>

/* These are the OIDs for some builtin types, as returned by PQftype(). */
/* They were copied from pg_type.h in src/include/catalog/pg_type.h */

#define BOOLOID 16
#define BYTEAOID 17
#define CHAROID 18
#define NAMEOID 19
#define INT8OID 20
#define INT2OID 21
#define INT2VECTOROID 22
#define INT4OID 23
#define REGPROCOID 24
#define TEXTOID 25
#define OIDOID 26
#define TIDOID 27
#define XIDOID 28
#define CIDOID 29
#define OIDVECTOROID 30
#define JSONOID 114
#define FLOAT4OID 700
#define FLOAT8OID 701
#define BOOLARRAYOID 1000
#define INT2ARRAYOID 1005
#define INT4ARRAYOID 1007
#define TEXTARRAYOID 1009
#define BPCHARARRAYOID 1014
#define VARCHARARRAYOID 1015
#define INT8ARRAYOID 1016
#define FLOAT4ARRAYOID 1021
#define FLOAT8ARRAYOID 1022
#define BPCHAROID 1042
#define VARCHAROID 1043
#define DATEOID 1082
#define TIMEOID 1083
#define TIMESTAMPOID 1114
#define TIMESTAMPTZOID 1184
#define NUMERICOID 1700
#define NUMERICARRAYOID 1231
#define UUIDOID 2950
#define JSONBOID 3802

CPLString OGRPGEscapeString(void *hPGConn, const char *pszStrValue,
                            int nMaxLengthUnused = -1,
                            const char *pszTableName = "",
                            const char *pszFieldName = "");
CPLString OGRPGEscapeColumnName(const char *pszColumnName);

#define UNDETERMINED_SRID                                                      \
    -2 /* Special value when we haven't yet looked for SRID */

class OGRPGDataSource;
class OGRPGLayer;

typedef enum
{
    GEOM_TYPE_UNKNOWN = 0,
    GEOM_TYPE_GEOMETRY = 1,
    GEOM_TYPE_GEOGRAPHY = 2,
    GEOM_TYPE_WKB = 3
} PostgisType;

typedef struct
{
    char *pszName;
    char *pszGeomType;
    int GeometryTypeFlags;
    int nSRID;
    PostgisType ePostgisType;
    int bNullable;
} PGGeomColumnDesc;

/************************************************************************/
/*                         OGRPGGeomFieldDefn                           */
/************************************************************************/

class OGRPGGeomFieldDefn final : public OGRGeomFieldDefn
{
    OGRPGGeomFieldDefn(const OGRPGGeomFieldDefn &) = delete;
    OGRPGGeomFieldDefn &operator=(const OGRPGGeomFieldDefn &) = delete;

  protected:
    OGRPGLayer *poLayer;

  public:
    OGRPGGeomFieldDefn(OGRPGLayer *poLayerIn, const char *pszFieldName)
        : OGRGeomFieldDefn(pszFieldName, wkbUnknown), poLayer(poLayerIn),
          nSRSId(UNDETERMINED_SRID), GeometryTypeFlags(0),
          ePostgisType(GEOM_TYPE_UNKNOWN)
    {
    }

    virtual const OGRSpatialReference *GetSpatialRef() const override;

    void UnsetLayer()
    {
        poLayer = nullptr;
    }

    mutable int nSRSId;
    mutable int GeometryTypeFlags;
    mutable PostgisType ePostgisType;
};

/************************************************************************/
/*                          OGRPGFeatureDefn                            */
/************************************************************************/

class OGRPGFeatureDefn CPL_NON_FINAL : public OGRFeatureDefn
{
  public:
    explicit OGRPGFeatureDefn(const char *pszName = nullptr)
        : OGRFeatureDefn(pszName)
    {
        SetGeomType(wkbNone);
    }

    virtual void UnsetLayer();

    OGRPGGeomFieldDefn *GetGeomFieldDefn(int i) override
    {
        return cpl::down_cast<OGRPGGeomFieldDefn *>(
            OGRFeatureDefn::GetGeomFieldDefn(i));
    }

    const OGRPGGeomFieldDefn *GetGeomFieldDefn(int i) const override
    {
        return cpl::down_cast<const OGRPGGeomFieldDefn *>(
            OGRFeatureDefn::GetGeomFieldDefn(i));
    }
};

/************************************************************************/
/*                            OGRPGLayer                                */
/************************************************************************/

class OGRPGLayer CPL_NON_FINAL : public OGRLayer
{
    OGRPGLayer(const OGRPGLayer &) = delete;
    OGRPGLayer &operator=(const OGRPGLayer &) = delete;

  protected:
    OGRPGFeatureDefn *poFeatureDefn = nullptr;

    int nCursorPage = 0;
    GIntBig iNextShapeId = 0;

    static char *GeometryToBYTEA(const OGRGeometry *, int nPostGISMajor,
                                 int nPostGISMinor);
    static GByte *BYTEAToGByteArray(const char *pszBytea, int *pnLength);
    static OGRGeometry *BYTEAToGeometry(const char *);
    Oid GeometryToOID(OGRGeometry *);
    OGRGeometry *OIDToGeometry(Oid);

    OGRPGDataSource *poDS = nullptr;

    char *pszQueryStatement = nullptr;

    char *pszCursorName = nullptr;
    PGresult *hCursorResult = nullptr;
    int bInvalidated = false;

    int nResultOffset = 0;

    int bWkbAsOid = false;

    char *pszFIDColumn = nullptr;

    int bCanUseBinaryCursor = true;
    int *m_panMapFieldNameToIndex = nullptr;
    int *m_panMapFieldNameToGeomIndex = nullptr;

    int ParsePGDate(const char *, OGRField *);

    void SetInitialQueryCursor();
    void CloseCursor();

    virtual CPLString GetFromClauseForGetExtent() = 0;
    OGRErr RunGetExtentRequest(OGREnvelope &sExtent, int bForce,
                               const std::string &osCommand, int bErrorAsDebug);
    OGRErr RunGetExtent3DRequest(OGREnvelope3D &sExtent3D,
                                 const std::string &osCommand,
                                 int bErrorAsDebug);
    static void CreateMapFromFieldNameToIndex(PGresult *hResult,
                                              OGRFeatureDefn *poFeatureDefn,
                                              int *&panMapFieldNameToIndex,
                                              int *&panMapFieldNameToGeomIndex);

    int ReadResultDefinition(PGresult *hInitialResultIn);

    OGRFeature *RecordToFeature(PGresult *hResult,
                                const int *panMapFieldNameToIndex,
                                const int *panMapFieldNameToGeomIndex,
                                int iRecord);
    OGRFeature *GetNextRawFeature();

  public:
    OGRPGLayer();
    virtual ~OGRPGLayer();

    virtual void ResetReading() override;

    virtual OGRPGFeatureDefn *GetLayerDefn() override
    {
        return poFeatureDefn;
    }

    OGRErr IGetExtent(int iGeomField, OGREnvelope *psExtent,
                      bool bForce) override;

    OGRErr IGetExtent3D(int iGeomField, OGREnvelope3D *psExtent3D,
                        bool bForce) override;

    virtual OGRErr StartTransaction() override;
    virtual OGRErr CommitTransaction() override;
    virtual OGRErr RollbackTransaction() override;

    void InvalidateCursor();

    virtual const char *GetFIDColumn() override;

    virtual OGRErr SetNextByIndex(GIntBig nIndex) override;

    OGRPGDataSource *GetDS()
    {
        return poDS;
    }

    GDALDataset *GetDataset() override;

    virtual void ResolveSRID(const OGRPGGeomFieldDefn *poGFldDefn) = 0;
};

/************************************************************************/
/*                           OGRPGTableLayer                            */
/************************************************************************/

class OGRPGTableLayer final : public OGRPGLayer
{
    OGRPGTableLayer(const OGRPGTableLayer &) = delete;
    OGRPGTableLayer &operator=(const OGRPGTableLayer &) = delete;

    static constexpr int USE_COPY_UNSET = -10;

    int bUpdateAccess = false;

    void BuildWhere();
    CPLString BuildFields();
    void BuildFullQueryStatement();

    char *pszTableName = nullptr;
    char *pszSchemaName = nullptr;
    char *m_pszTableDescription = nullptr;
    CPLString osForcedDescription{};
    bool m_bMetadataLoaded = false;
    bool m_bMetadataModified = false;
    char *pszSqlTableName = nullptr;
    int bTableDefinitionValid = -1;

    CPLString osPrimaryKey{};

    int bGeometryInformationSet = false;

    /* Name of the parent table with the geometry definition if it is a derived
     * table or NULL */
    char *pszSqlGeomParentTableName = nullptr;

    char *pszGeomColForced = nullptr;

    CPLString osQuery{};
    CPLString osWHERE{};

    int bLaunderColumnNames = true;
    bool m_bUTF8ToASCII = false;
    int bPreservePrecision = true;
    int bUseCopy = USE_COPY_UNSET;
    int bCopyActive = false;
    bool bFIDColumnInCopyFields = false;
    int bFirstInsertion = true;

    OGRErr CreateFeatureViaCopy(OGRFeature *poFeature);
    OGRErr CreateFeatureViaInsert(OGRFeature *poFeature);
    CPLString BuildCopyFields();

    int bHasWarnedIncompatibleGeom = false;
    void CheckGeomTypeCompatibility(int iGeomField, OGRGeometry *poGeom);

    int bRetrieveFID = true;
    int bSkipConflicts = false;
    int bHasWarnedAlreadySetFID = false;

    char **papszOverrideColumnTypes = nullptr;
    int nForcedSRSId = UNDETERMINED_SRID;
    int nForcedGeometryTypeFlags = -1;
    bool bCreateSpatialIndexFlag = true;
    CPLString osSpatialIndexType = "GIST";
    int bInResetReading = false;

    int bAutoFIDOnCreateViaCopy = false;
    int bUseCopyByDefault = false;
    bool bNeedToUpdateSequence = false;

    int bDeferredCreation = false;
    CPLString osCreateTable{};
    std::vector<std::string> m_aosDeferredCommentOnColumns{};

    int iFIDAsRegularColumnIndex = -1;

    CPLString m_osFirstGeometryFieldName{};

    std::string m_osLCOGeomType{};

    virtual CPLString GetFromClauseForGetExtent() override
    {
        return pszSqlTableName;
    }

    OGRErr RunAddGeometryColumn(const OGRPGGeomFieldDefn *poGeomField);
    OGRErr RunCreateSpatialIndex(const OGRPGGeomFieldDefn *poGeomField,
                                 int nIdx);

    void UpdateSequenceIfNeeded();

    void LoadMetadata();
    void SerializeMetadata();

  public:
    OGRPGTableLayer(OGRPGDataSource *, CPLString &osCurrentSchema,
                    const char *pszTableName, const char *pszSchemaName,
                    const char *pszDescriptionIn, const char *pszGeomColForced,
                    int bUpdate);
    virtual ~OGRPGTableLayer();

    void SetGeometryInformation(PGGeomColumnDesc *pasDesc, int nGeomFieldCount);

    virtual OGRFeature *GetFeature(GIntBig nFeatureId) override;
    virtual void ResetReading() override;
    virtual OGRFeature *GetNextFeature() override;
    virtual GIntBig GetFeatureCount(int) override;

    OGRErr ISetSpatialFilter(int iGeomField,
                             const OGRGeometry *poGeom) override;

    virtual OGRErr SetAttributeFilter(const char *) override;

    virtual OGRErr ISetFeature(OGRFeature *poFeature) override;
    OGRErr IUpdateFeature(OGRFeature *poFeature, int nUpdatedFieldsCount,
                          const int *panUpdatedFieldsIdx,
                          int nUpdatedGeomFieldsCount,
                          const int *panUpdatedGeomFieldsIdx,
                          bool bUpdateStyleString) override;
    virtual OGRErr DeleteFeature(GIntBig nFID) override;
    virtual OGRErr ICreateFeature(OGRFeature *poFeature) override;

    virtual OGRErr CreateField(const OGRFieldDefn *poField,
                               int bApproxOK = TRUE) override;
    virtual OGRErr CreateGeomField(const OGRGeomFieldDefn *poGeomField,
                                   int bApproxOK = TRUE) override;
    virtual OGRErr DeleteField(int iField) override;
    virtual OGRErr AlterFieldDefn(int iField, OGRFieldDefn *poNewFieldDefn,
                                  int nFlags) override;
    virtual OGRErr
    AlterGeomFieldDefn(int iGeomFieldToAlter,
                       const OGRGeomFieldDefn *poNewGeomFieldDefn,
                       int nFlagsIn) override;

    virtual int TestCapability(const char *) override;

    OGRErr IGetExtent(int iGeomField, OGREnvelope *psExtent,
                      bool bForce) override;

    const char *GetTableName()
    {
        return pszTableName;
    }

    const char *GetSchemaName()
    {
        return pszSchemaName;
    }

    virtual const char *GetFIDColumn() override;

    virtual char **GetMetadataDomainList() override;
    virtual char **GetMetadata(const char *pszDomain = "") override;
    virtual const char *GetMetadataItem(const char *pszName,
                                        const char *pszDomain = "") override;
    virtual CPLErr SetMetadata(char **papszMD,
                               const char *pszDomain = "") override;
    virtual CPLErr SetMetadataItem(const char *pszName, const char *pszValue,
                                   const char *pszDomain = "") override;

    virtual OGRErr Rename(const char *pszNewName) override;

    OGRGeometryTypeCounter *GetGeometryTypes(int iGeomField, int nFlagsGGT,
                                             int &nEntryCountOut,
                                             GDALProgressFunc pfnProgress,
                                             void *pProgressData) override;

    int FindFieldIndex(const char *pszFieldName, int bExactMatch) override;

    // follow methods are not base class overrides
    void SetLaunderFlag(int bFlag)
    {
        bLaunderColumnNames = bFlag;
    }

    void SetUTF8ToASCIIFlag(bool bFlag)
    {
        m_bUTF8ToASCII = bFlag;
    }

    void SetPrecisionFlag(int bFlag)
    {
        bPreservePrecision = bFlag;
    }

    void SetOverrideColumnTypes(const char *pszOverrideColumnTypes);

    OGRErr StartCopy();
    OGRErr EndCopy();

    int ReadTableDefinition();

    int HasGeometryInformation()
    {
        return bGeometryInformationSet;
    }

    void SetTableDefinition(const char *pszFIDColumnName,
                            const char *pszGFldName, OGRwkbGeometryType eType,
                            const char *pszGeomType, int nSRSId,
                            int GeometryTypeFlags);

    void SetForcedSRSId(int nForcedSRSIdIn)
    {
        nForcedSRSId = nForcedSRSIdIn;
    }

    void SetForcedGeometryTypeFlags(int GeometryTypeFlagsIn)
    {
        nForcedGeometryTypeFlags = GeometryTypeFlagsIn;
    }

    void SetCreateSpatialIndex(bool bFlag, const char *pszSpatialIndexType)
    {
        bCreateSpatialIndexFlag = bFlag;
        osSpatialIndexType = pszSpatialIndexType;
    }

    void SetForcedDescription(const char *pszDescriptionIn);

    void AllowAutoFIDOnCreateViaCopy()
    {
        bAutoFIDOnCreateViaCopy = TRUE;
    }

    void SetUseCopy()
    {
        bUseCopy = TRUE;
        bUseCopyByDefault = TRUE;
    }

    void SetDeferredCreation(int bDeferredCreationIn,
                             const std::string &osCreateTable);
    OGRErr RunDeferredCreationIfNecessary();

    virtual void ResolveSRID(const OGRPGGeomFieldDefn *poGFldDefn) override;
};

/************************************************************************/
/*                           OGRPGResultLayer                           */
/************************************************************************/

class OGRPGResultLayer final : public OGRPGLayer
{
    OGRPGResultLayer(const OGRPGResultLayer &) = delete;
    OGRPGResultLayer &operator=(const OGRPGResultLayer &) = delete;

    void BuildFullQueryStatement();

    char *pszRawStatement = nullptr;

    char *pszGeomTableName = nullptr;
    char *pszGeomTableSchemaName = nullptr;

    CPLString osWHERE{};

    virtual CPLString GetFromClauseForGetExtent() override
    {
        CPLString osStr("(");
        osStr += pszRawStatement;
        osStr += ")";
        return osStr;
    }

  public:
    OGRPGResultLayer(OGRPGDataSource *, const char *pszRawStatement,
                     PGresult *hInitialResult);
    virtual ~OGRPGResultLayer();

    virtual void ResetReading() override;
    virtual GIntBig GetFeatureCount(int) override;

    OGRErr ISetSpatialFilter(int iGeomField,
                             const OGRGeometry *poGeom) override;

    virtual int TestCapability(const char *) override;

    virtual OGRFeature *GetNextFeature() override;

    virtual void ResolveSRID(const OGRPGGeomFieldDefn *poGFldDefn) override;
};

/************************************************************************/
/*                           OGRPGDataSource                            */
/************************************************************************/

class OGRPGDataSource final : public GDALDataset
{
    OGRPGDataSource(const OGRPGDataSource &) = delete;
    OGRPGDataSource &operator=(const OGRPGDataSource &) = delete;

    typedef struct
    {
        int nMajor;
        int nMinor;
        int nRelease;
    } PGver;

    OGRPGTableLayer **papoLayers = nullptr;
    int nLayers = 0;

    bool m_bUTF8ClientEncoding = false;

    int bDSUpdate = false;
    int bHavePostGIS = false;
    int bHaveGeography = false;

    bool bUserTransactionActive = false;
    int bSavePointActive = false;
    int nSoftTransactionLevel = 0;

    PGconn *hPGConn = nullptr;

    OGRErr DeleteLayer(int iLayer) override;

    Oid nGeometryOID = static_cast<Oid>(0);
    Oid nGeographyOID = static_cast<Oid>(0);

    // We maintain a list of known SRID to reduce the number of trips to
    // the database to get SRSes.
    std::map<int,
             std::unique_ptr<OGRSpatialReference, OGRSpatialReferenceReleaser>>
        m_oSRSCache{};

    OGRPGTableLayer *poLayerInCopyMode = nullptr;

    static void OGRPGDecodeVersionString(PGver *psVersion, const char *pszVer);

    CPLString osCurrentSchema{};
    CPLString GetCurrentSchema();

    // Actual value will be auto-detected if PostGIS >= 2.0 detected.
    int nUndefinedSRID = -1;

    char *pszForcedTables = nullptr;
    char **papszSchemaList = nullptr;
    int bHasLoadTables = false;
    CPLString osActiveSchema{};
    int bListAllTables = false;
    bool m_bSkipViews = false;

    bool m_bOgrSystemTablesMetadataTableExistenceTested = false;
    bool m_bOgrSystemTablesMetadataTableFound = false;

    bool m_bCreateMetadataTableIfNeededRun = false;
    bool m_bCreateMetadataTableIfNeededSuccess = false;

    bool m_bHasWritePermissionsOnMetadataTableRun = false;
    bool m_bHasWritePermissionsOnMetadataTableSuccess = false;

    void LoadTables();

    CPLString osDebugLastTransactionCommand{};
    OGRErr DoTransactionCommand(const char *pszCommand);

    OGRErr FlushSoftTransaction();

    OGRErr FlushCacheWithRet(bool bAtClosing);

    std::optional<std::string> FindSchema(const char *pszSchemaNameIn);

    bool IsSuperUser();
    bool OGRSystemTablesEventTriggerExists();

  public:
    PGver sPostgreSQLVersion = {0, 0, 0};
    PGver sPostGISVersion = {0, 0, 0};

    int bUseBinaryCursor = false;
    int bBinaryTimeFormatIsInt8 = false;

    bool m_bHasGeometryColumns = false;
    bool m_bHasSpatialRefSys = false;

    bool HavePostGIS() const
    {
        return bHavePostGIS;
    }

    int GetUndefinedSRID() const
    {
        return nUndefinedSRID;
    }

    bool IsUTF8ClientEncoding() const
    {
        return m_bUTF8ClientEncoding;
    }

  public:
    OGRPGDataSource();
    virtual ~OGRPGDataSource();

    PGconn *GetPGConn()
    {
        return hPGConn;
    }

    int FetchSRSId(const OGRSpatialReference *poSRS);
    const OGRSpatialReference *FetchSRS(int nSRSId);
    static OGRErr InitializeMetadataTables();

    int Open(const char *, int bUpdate, int bTestOpen, char **papszOpenOptions);
    OGRPGTableLayer *
    OpenTable(CPLString &osCurrentSchema, const char *pszTableName,
              const char *pszSchemaName, const char *pszDescription,
              const char *pszGeomColForced, int bUpdate, int bTestOpen);

    int GetLayerCount() override;
    OGRLayer *GetLayer(int) override;
    OGRLayer *GetLayerByName(const char *pszName) override;

    virtual CPLErr FlushCache(bool bAtClosing) override;

    OGRLayer *ICreateLayer(const char *pszName,
                           const OGRGeomFieldDefn *poGeomFieldDefn,
                           CSLConstList papszOptions) override;

    int TestCapability(const char *) override;

    virtual OGRErr StartTransaction(int bForce = FALSE) override;
    virtual OGRErr CommitTransaction() override;
    virtual OGRErr RollbackTransaction() override;

    OGRErr SoftStartTransaction();
    OGRErr SoftCommitTransaction();
    OGRErr SoftRollbackTransaction();

    Oid GetGeometryOID()
    {
        return nGeometryOID;
    }

    Oid GetGeographyOID()
    {
        return nGeographyOID;
    }

    virtual OGRLayer *ExecuteSQL(const char *pszSQLCommand,
                                 OGRGeometry *poSpatialFilter,
                                 const char *pszDialect) override;
    virtual OGRErr AbortSQL() override;
    virtual void ReleaseResultSet(OGRLayer *poLayer) override;

    virtual const char *GetMetadataItem(const char *pszKey,
                                        const char *pszDomain) override;

    int UseCopy();
    void StartCopy(OGRPGTableLayer *poPGLayer);
    OGRErr EndCopy();

    bool IsUserTransactionActive()
    {
        return bUserTransactionActive;
    }

    bool CreateMetadataTableIfNeeded();
    bool HasOgrSystemTablesMetadataTable();
    bool HasWritePermissionsOnMetadataTable();
};

#endif /* ndef OGR_PG_H_INCLUDED */
