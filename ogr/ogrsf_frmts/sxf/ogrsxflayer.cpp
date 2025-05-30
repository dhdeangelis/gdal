/******************************************************************************
 *
 * Project:  SXF Translator
 * Purpose:  Definition of classes for OGR SXF Layers.
 * Author:   Ben Ahmed Daho Ali, bidandou(at)yahoo(dot)fr
 *           Dmitry Baryshnikov, polimax@mail.ru
 *           Alexandr Lisovenko, alexander.lisovenko@gmail.com
 *
 ******************************************************************************
 * Copyright (c) 2011, Ben Ahmed Daho Ali
 * Copyright (c) 2013, NextGIS
 * Copyright (c) 2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "ogr_sxf.h"
#include "cpl_conv.h"
#include "cpl_string.h"
#include "ogr_p.h"
#include "ogr_srs_api.h"
#include "cpl_multiproc.h"

/************************************************************************/
/*                        OGRSXFLayer()                                 */
/************************************************************************/

OGRSXFLayer::OGRSXFLayer(VSILFILE *fp, CPLMutex **hIOMutex, GByte nID,
                         const char *pszLayerName, int nVer,
                         const SXFMapDescription &sxfMapDesc)
    : OGRLayer(), poFeatureDefn(new OGRFeatureDefn(pszLayerName)), fpSXF(fp),
      nLayerID(nID), stSXFMapDescription(sxfMapDesc), m_nSXFFormatVer(nVer),
      sFIDColumn_("ogc_fid"), m_hIOMutex(hIOMutex),
      m_dfCoeff(sxfMapDesc.nResolution == 0
                    ? 0.0
                    : sxfMapDesc.dfScale / sxfMapDesc.nResolution)
{
    stSXFMapDescription.pSpatRef->Reference();
    oNextIt = mnRecordDesc.begin();
    SetDescription(poFeatureDefn->GetName());
    poFeatureDefn->Reference();

    poFeatureDefn->SetGeomType(wkbUnknown);
    if (poFeatureDefn->GetGeomFieldCount() != 0)
        poFeatureDefn->GetGeomFieldDefn(0)->SetSpatialRef(
            stSXFMapDescription.pSpatRef);

    OGRFieldDefn oFIDField(sFIDColumn_, OFTInteger);
    poFeatureDefn->AddFieldDefn(&oFIDField);

    OGRFieldDefn oClCodeField("CLCODE", OFTInteger);
    oClCodeField.SetWidth(10);
    poFeatureDefn->AddFieldDefn(&oClCodeField);

    OGRFieldDefn oClNameField("CLNAME", OFTString);
    oClNameField.SetWidth(32);
    poFeatureDefn->AddFieldDefn(&oClNameField);

    OGRFieldDefn oNumField("OBJECTNUMB", OFTInteger);
    oNumField.SetWidth(10);
    poFeatureDefn->AddFieldDefn(&oNumField);

    OGRFieldDefn oAngField("ANGLE", OFTReal);
    poFeatureDefn->AddFieldDefn(&oAngField);

    OGRFieldDefn oTextField("TEXT", OFTString);
    oTextField.SetWidth(255);
    poFeatureDefn->AddFieldDefn(&oTextField);
}

/************************************************************************/
/*                         ~OGRSXFLayer()                               */
/************************************************************************/

OGRSXFLayer::~OGRSXFLayer()
{
    stSXFMapDescription.pSpatRef->Release();
    poFeatureDefn->Release();
}

/************************************************************************/
/*                AddClassifyCode(unsigned nClassCode)                  */
/* Add layer supported classify codes. Only records with this code can  */
/* be in layer                                                          */
/************************************************************************/

void OGRSXFLayer::AddClassifyCode(unsigned nClassCode, const char *szName)
{
    if (szName != nullptr)
    {
        mnClassificators[nClassCode] = CPLString(szName);
    }
    else
    {
        mnClassificators[nClassCode] = CPLString().Printf("%d", nClassCode);
    }
}

/************************************************************************/
/*                           AddRecord()                                */
/************************************************************************/

bool OGRSXFLayer::AddRecord(long nFID, unsigned nClassCode,
                            vsi_l_offset nOffset, bool bHasSemantic,
                            size_t nSemanticsSize)
{
    if (mnClassificators.find(nClassCode) != mnClassificators.end() ||
        EQUAL(GetName(), "Not_Classified"))
    {
        mnRecordDesc[nFID] = nOffset;
        // Add additional semantics (attribute fields).
        if (bHasSemantic)
        {
            size_t offset = 0;

            while (offset < nSemanticsSize)
            {
                SXFRecordAttributeInfo stAttrInfo;
                bool bAddField = false;
                size_t nCurrOff = 0;
                int nReadObj =
                    static_cast<int>(VSIFReadL(&stAttrInfo, 4, 1, fpSXF));
                if (nReadObj == 1)
                {
                    CPL_LSBPTR16(&(stAttrInfo.nCode));
                    CPLString oFieldName;
                    if (snAttributeCodes.find(stAttrInfo.nCode) ==
                        snAttributeCodes.end())
                    {
                        bAddField = true;
                        snAttributeCodes.insert(stAttrInfo.nCode);
                        oFieldName.Printf("SC_%d", stAttrInfo.nCode);
                    }

                    SXFRecordAttributeType eType =
                        (SXFRecordAttributeType)stAttrInfo.nType;

                    offset += 4;

                    switch (eType)  // TODO: set field type form RSC as here
                                    // sometimes we have the codes and string
                                    // values can be get from RSC by this code
                    {
                        case SXF_RAT_ASCIIZ_DOS:
                        {
                            if (bAddField)
                            {
                                OGRFieldDefn oField(oFieldName, OFTString);
                                oField.SetWidth(255);
                                poFeatureDefn->AddFieldDefn(&oField);
                            }
                            offset += stAttrInfo.nScale + 1;
                            nCurrOff = stAttrInfo.nScale + 1;
                            break;
                        }
                        case SXF_RAT_ONEBYTE:
                        {
                            if (bAddField)
                            {
                                OGRFieldDefn oField(oFieldName, OFTReal);
                                poFeatureDefn->AddFieldDefn(&oField);
                            }
                            offset += 1;
                            nCurrOff = 1;
                            break;
                        }
                        case SXF_RAT_TWOBYTE:
                        {
                            if (bAddField)
                            {
                                OGRFieldDefn oField(oFieldName, OFTReal);
                                poFeatureDefn->AddFieldDefn(&oField);
                            }
                            offset += 2;
                            nCurrOff = 2;
                            break;
                        }
                        case SXF_RAT_FOURBYTE:
                        {
                            if (bAddField)
                            {
                                OGRFieldDefn oField(oFieldName, OFTReal);
                                poFeatureDefn->AddFieldDefn(&oField);
                            }
                            offset += 4;
                            nCurrOff = 4;
                            break;
                        }
                        case SXF_RAT_EIGHTBYTE:
                        {
                            if (bAddField)
                            {
                                OGRFieldDefn oField(oFieldName, OFTReal);
                                poFeatureDefn->AddFieldDefn(&oField);
                            }
                            offset += 8;
                            nCurrOff = 8;
                            break;
                        }
                        case SXF_RAT_ANSI_WIN:
                        {
                            if (bAddField)
                            {
                                OGRFieldDefn oField(oFieldName, OFTString);
                                oField.SetWidth(255);
                                poFeatureDefn->AddFieldDefn(&oField);
                            }
                            unsigned nLen = unsigned(stAttrInfo.nScale) + 1;
                            offset += nLen;
                            nCurrOff = nLen;
                            break;
                        }
                        case SXF_RAT_UNICODE:
                        {
                            if (bAddField)
                            {
                                OGRFieldDefn oField(oFieldName, OFTString);
                                oField.SetWidth(255);
                                poFeatureDefn->AddFieldDefn(&oField);
                            }
                            unsigned nLen =
                                (unsigned(stAttrInfo.nScale) + 1) * 2;
                            offset += nLen;
                            nCurrOff = nLen;
                            break;
                        }
                        case SXF_RAT_BIGTEXT:
                        {
                            if (bAddField)
                            {
                                OGRFieldDefn oField(oFieldName, OFTString);
                                oField.SetWidth(1024);
                                poFeatureDefn->AddFieldDefn(&oField);
                            }
                            GUInt32 scale2 = 0;
                            VSIFReadL(&scale2, sizeof(GUInt32), 1, fpSXF);
                            CPL_LSBPTR32(&scale2);

                            offset += scale2;
                            nCurrOff = scale2;
                            break;
                        }
                        default:
                            break;
                    }
                }
                if (nCurrOff == 0)
                    break;
                VSIFSeekL(fpSXF, nCurrOff, SEEK_CUR);
            }
        }
        return true;
    }

    return false;
}

/************************************************************************/
/*                           SetNextByIndex()                           */
/************************************************************************/

OGRErr OGRSXFLayer::SetNextByIndex(GIntBig nIndex)
{
    if (nIndex < 0 || nIndex > (long)mnRecordDesc.size())
        return OGRERR_FAILURE;

    oNextIt = mnRecordDesc.begin();
    std::advance(oNextIt, static_cast<size_t>(nIndex));

    return OGRERR_NONE;
}

/************************************************************************/
/*                             GetFeature()                             */
/************************************************************************/

OGRFeature *OGRSXFLayer::GetFeature(GIntBig nFID)
{
    const auto IT = mnRecordDesc.find(static_cast<long>(nFID));
    if (IT != mnRecordDesc.end())
    {
        VSIFSeekL(fpSXF, IT->second, SEEK_SET);
        OGRFeature *poFeature = GetNextRawFeature(IT->first);
        if (poFeature != nullptr && poFeature->GetGeometryRef() != nullptr &&
            GetSpatialRef() != nullptr)
        {
            poFeature->GetGeometryRef()->assignSpatialReference(
                GetSpatialRef());
        }
        return poFeature;
    }

    return nullptr;
}

/************************************************************************/
/*                           GetSpatialRef()                            */
/************************************************************************/

OGRSpatialReference *OGRSXFLayer::GetSpatialRef()
{
    return stSXFMapDescription.pSpatRef;
}

/************************************************************************/
/*                            IGetExtent()                              */
/************************************************************************/

OGRErr OGRSXFLayer::IGetExtent(int iGeomField, OGREnvelope *psExtent,
                               bool bForce)
{
    if (bForce)
    {
        return OGRLayer::IGetExtent(iGeomField, psExtent, bForce);
    }
    else
    {
        psExtent->MinX = stSXFMapDescription.Env.MinX;
        psExtent->MaxX = stSXFMapDescription.Env.MaxX;
        psExtent->MinY = stSXFMapDescription.Env.MinY;
        psExtent->MaxY = stSXFMapDescription.Env.MaxY;

        return OGRERR_NONE;
    }
}

/************************************************************************/
/*                          GetFeatureCount()                           */
/************************************************************************/

GIntBig OGRSXFLayer::GetFeatureCount(int bForce)
{
    if (m_poFilterGeom == nullptr && m_poAttrQuery == nullptr)
        return static_cast<int>(mnRecordDesc.size());
    else
        return OGRLayer::GetFeatureCount(bForce);
}

/************************************************************************/
/*                            ResetReading()                            */
/************************************************************************/

void OGRSXFLayer::ResetReading()

{
    oNextIt = mnRecordDesc.begin();
}

/************************************************************************/
/*                           GetNextFeature()                           */
/************************************************************************/

OGRFeature *OGRSXFLayer::GetNextFeature()
{
    CPLMutexHolderD(m_hIOMutex);
    while (oNextIt != mnRecordDesc.end())
    {
        VSIFSeekL(fpSXF, oNextIt->second, SEEK_SET);
        OGRFeature *poFeature = GetNextRawFeature(oNextIt->first);

        ++oNextIt;

        if (poFeature == nullptr)
            continue;

        if ((m_poFilterGeom == nullptr ||
             FilterGeometry(poFeature->GetGeometryRef())) &&
            (m_poAttrQuery == nullptr || m_poAttrQuery->Evaluate(poFeature)))
        {
            if (poFeature->GetGeometryRef() != nullptr &&
                GetSpatialRef() != nullptr)
            {
                poFeature->GetGeometryRef()->assignSpatialReference(
                    GetSpatialRef());
            }

            return poFeature;
        }

        delete poFeature;
    }
    return nullptr;
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGRSXFLayer::TestCapability(const char *pszCap)

{
    if (EQUAL(pszCap, OLCStringsAsUTF8) &&
        CPLCanRecode("test", "CP1251", CPL_ENC_UTF8) &&
        CPLCanRecode("test", "KOI8-R", CPL_ENC_UTF8))
        return TRUE;
    else if (EQUAL(pszCap, OLCRandomRead))
        return TRUE;
    else if (EQUAL(pszCap, OLCFastFeatureCount))
        return TRUE;
    else if (EQUAL(pszCap, OLCFastGetExtent))
        return TRUE;
    else if (EQUAL(pszCap, OLCFastSetNextByIndex))
        return TRUE;
    else if (EQUAL(pszCap, OLCZGeometries))
        return TRUE;

    return FALSE;
}

/************************************************************************/
/*                                TranslateXYH()                        */
/************************************************************************/
/****
 * TODO : Take into account information given in the passport
 * like unit of measurement, type and dimensions (integer, float, double) of
 * coordinate, the vector format, etc.
 */

GUInt32 OGRSXFLayer::TranslateXYH(const SXFRecordDescription &certifInfo,
                                  const char *psBuff, GUInt32 nBufLen,
                                  double *dfX, double *dfY, double *dfH)
{
    // Xp, Yp(м) = Xo, Yo(м) + (Xd, Yd / R * S), (1)

    int offset = 0;
    switch (certifInfo.eValType)
    {
        case SXF_VT_SHORT:
        {
            if (nBufLen < 4)
                return 0;
            GInt16 x = 0;
            GInt16 y = 0;
            memcpy(&y, psBuff, 2);
            CPL_LSBPTR16(&y);
            memcpy(&x, psBuff + 2, 2);
            CPL_LSBPTR16(&x);

            if (stSXFMapDescription.bIsRealCoordinates)
            {
                *dfX = (double)x;
                *dfY = (double)y;
            }
            else
            {
                if (m_nSXFFormatVer == 3)
                {
                    *dfX = stSXFMapDescription.dfXOr + (double)x * m_dfCoeff;
                    *dfY = stSXFMapDescription.dfYOr + (double)y * m_dfCoeff;
                }
                else if (m_nSXFFormatVer == 4)
                {
                    // TODO: check on real data
                    *dfX = stSXFMapDescription.dfXOr + (double)x * m_dfCoeff;
                    *dfY = stSXFMapDescription.dfYOr + (double)y * m_dfCoeff;
                }
            }

            offset += 4;

            if (dfH != nullptr)
            {
                if (nBufLen < 4 + 4)
                    return 0;
                float h = 0.0f;
                memcpy(&h, psBuff + 4, 4);  // H always in float
                CPL_LSBPTR32(&h);
                *dfH = (double)h;

                offset += 4;
            }
        }
        break;
        case SXF_VT_FLOAT:
        {
            if (nBufLen < 8)
                return 0;
            float y = 0.0f;
            memcpy(&y, psBuff, 4);
            CPL_LSBPTR32(&y);
            float x = 0.0f;
            memcpy(&x, psBuff + 4, 4);
            CPL_LSBPTR32(&x);

            if (stSXFMapDescription.bIsRealCoordinates)
            {
                *dfX = (double)x;
                *dfY = (double)y;
            }
            else
            {
                *dfX = stSXFMapDescription.dfXOr + (double)x * m_dfCoeff;
                *dfY = stSXFMapDescription.dfYOr + (double)y * m_dfCoeff;
            }

            offset += 8;

            if (dfH != nullptr)
            {
                if (nBufLen < 8 + 4)
                    return 0;
                float h = 0.0f;
                memcpy(&h, psBuff + 8, 4);  // H always in float
                CPL_LSBPTR32(&h);
                *dfH = (double)h;

                offset += 4;
            }
        }
        break;
        case SXF_VT_INT:
        {
            if (nBufLen < 8)
                return 0;
            GInt32 x, y;
            memcpy(&y, psBuff, 4);
            CPL_LSBPTR32(&y);
            memcpy(&x, psBuff + 4, 4);
            CPL_LSBPTR32(&x);

            if (stSXFMapDescription.bIsRealCoordinates)
            {
                *dfX = (double)x;
                *dfY = (double)y;
            }
            else
            {
                // TODO: check on real data
                if (m_nSXFFormatVer == 3)
                {
                    *dfX = stSXFMapDescription.dfXOr + (double)x * m_dfCoeff;
                    *dfY = stSXFMapDescription.dfYOr + (double)y * m_dfCoeff;
                }
                else if (m_nSXFFormatVer == 4)
                {
                    *dfX = stSXFMapDescription.dfXOr + (double)x * m_dfCoeff;
                    *dfY = stSXFMapDescription.dfYOr + (double)y * m_dfCoeff;
                }
            }
            offset += 8;

            if (dfH != nullptr)
            {
                if (nBufLen < 8 + 4)
                    return 0;
                float h = 0.0f;
                memcpy(&h, psBuff + 8, 4);  // H always in float
                CPL_LSBPTR32(&h);
                *dfH = (double)h;

                offset += 4;
            }
        }
        break;
        case SXF_VT_DOUBLE:
        {
            if (nBufLen < 16)
                return 0;
            double x = 0.0;
            double y = 0.0;
            memcpy(&y, psBuff, 8);
            CPL_LSBPTR64(&y);
            memcpy(&x, psBuff + 8, 8);
            CPL_LSBPTR64(&x);

            if (stSXFMapDescription.bIsRealCoordinates)
            {
                *dfX = x;
                *dfY = y;
            }
            else
            {
                *dfX = stSXFMapDescription.dfXOr + x * m_dfCoeff;
                *dfY = stSXFMapDescription.dfYOr + y * m_dfCoeff;
            }

            offset += 16;

            if (dfH != nullptr)
            {
                if (nBufLen < 16 + 8)
                    return 0;
                double h = 0.0;
                memcpy(&h, psBuff + 16, 8);  // H in double
                CPL_LSBPTR64(&h);
                *dfH = (double)h;

                offset += 8;
            }
        }
        break;
    };

    return offset;
}

/************************************************************************/
/*                         GetNextRawFeature()                          */
/************************************************************************/

OGRFeature *OGRSXFLayer::GetNextRawFeature(long nFID)
{
    SXFRecordHeader stRecordHeader;
    int nObjectRead = static_cast<int>(
        VSIFReadL(&stRecordHeader, sizeof(SXFRecordHeader), 1, fpSXF));

    if (nObjectRead != 1)
    {
        CPLError(CE_Failure, CPLE_FileIO, "SXF. Read record failed.");
        return nullptr;
    }
    CPL_LSBPTR32(&(stRecordHeader.nID));
    if (stRecordHeader.nID != IDSXFOBJ)
    {
        CPLError(CE_Failure, CPLE_FileIO, "SXF. Read record failed.");
        return nullptr;
    }
    CPL_LSBPTR32(&(stRecordHeader.nFullLength));
    CPL_LSBPTR32(&(stRecordHeader.nGeometryLength));
    CPL_LSBPTR32(&(stRecordHeader.nClassifyCode));
    CPL_LSBPTR16(&(stRecordHeader.anGroup[0]));
    CPL_LSBPTR16(&(stRecordHeader.anGroup[1]));
    CPL_LSBPTR32(&(stRecordHeader.nPointCount));
    CPL_LSBPTR16(&(stRecordHeader.nSubObjectCount));
    CPL_LSBPTR16(&(stRecordHeader.nPointCountSmall));

    SXFGeometryType eGeomType = SXF_GT_Unknown;
    GByte code = 0;

    if (m_nSXFFormatVer == 3)
    {
        if (CHECK_BIT(stRecordHeader.nRef[2], 3))
        {
            if (CHECK_BIT(stRecordHeader.nRef[2], 4))
            {
                code = 0x22;
                stRecordHeader.nSubObjectCount = 0;
            }
            else
            {
                code = 0x21;
                stRecordHeader.nSubObjectCount = 0;
            }
        }
        else
        {
            code = stRecordHeader.nRef[0] & 3;  // get first 2 bits
        }
    }
    else if (m_nSXFFormatVer == 4)
    {
        if (CHECK_BIT(stRecordHeader.nRef[2], 5))
        {
            stRecordHeader.nSubObjectCount = 0;
        }

        // check if vector
        code = stRecordHeader.nRef[0] & 15;  // get first 4 bits
        if (code == 0x04)                    // xxxx0100
        {
            code = 0x21;
            stRecordHeader.nSubObjectCount = 0;
            // if (CHECK_BIT(stRecordHeader.nRef[2], 5))
            //{
            //     code = 0x22;
            //     stRecordHeader.nSubObjectCount = 0;
            // }
            // else
            //{
            //     code = 0x21;
            //     stRecordHeader.nSubObjectCount = 0;
            // }
            // if (CHECK_BIT(stRecordHeader.nRef[2], 4))
            //{
            // }
            // else
            //{
            // }
        }
    }

    if (code == 0x00)  // xxxx0000
        eGeomType = SXF_GT_Line;
    else if (code == 0x01)  // xxxx0001
        eGeomType = SXF_GT_Polygon;
    else if (code == 0x02)  // xxxx0010
        eGeomType = SXF_GT_Point;
    else if (code == 0x03)  // xxxx0011
        eGeomType = SXF_GT_Text;
#ifdef not_possible_given_above_code /* see below code too if re-enabling this \
                                      */
    // beginning 4.0
    else if (code == 0x04)  // xxxx0100
    {
        CPLError(CE_Warning, CPLE_NotSupported, "SXF. Not support type.");
        eGeomType = SXF_GT_Vector;
    }
#endif
    else if (code == 0x05)  // xxxx0101
        eGeomType = SXF_GT_TextTemplate;
    else if (code == 0x21)
        eGeomType = SXF_GT_VectorAngle;
    else if (code == 0x22)
        eGeomType = SXF_GT_VectorScaled;

    bool bHasAttributes = CHECK_BIT(stRecordHeader.nRef[1], 1);
    bool bHasRefVector = CHECK_BIT(stRecordHeader.nRef[1], 3);
    if (bHasRefVector == true)
        CPLError(CE_Failure, CPLE_NotSupported,
                 "SXF. Parsing the vector of the tying not support.");

    SXFRecordDescription stCertInfo;
    if (stRecordHeader.nPointCountSmall == 65535)
    {
        stCertInfo.nPointCount = stRecordHeader.nPointCount;
    }
    else
    {
        stCertInfo.nPointCount = stRecordHeader.nPointCountSmall;
    }
    stCertInfo.nSubObjectCount = stRecordHeader.nSubObjectCount;

    bool bFloatType(false), bBigType(false);
    bool b3D(true);
    if (m_nSXFFormatVer == 3)
    {
        b3D = CHECK_BIT(stRecordHeader.nRef[2], 1);
        bFloatType = CHECK_BIT(stRecordHeader.nRef[2], 2);
        bBigType = CHECK_BIT(stRecordHeader.nRef[1], 2);
        stCertInfo.bHasTextSign = CHECK_BIT(stRecordHeader.nRef[2], 5);
    }
    else if (m_nSXFFormatVer == 4)
    {
        b3D = CHECK_BIT(stRecordHeader.nRef[2], 1);
        bFloatType = CHECK_BIT(stRecordHeader.nRef[2], 2);
        bBigType = CHECK_BIT(stRecordHeader.nRef[1], 2);
        stCertInfo.bHasTextSign = CHECK_BIT(stRecordHeader.nRef[2], 3);
    }
    // Else trouble.

    if (b3D)  // xxxxxx1x
        stCertInfo.bDim = 1;
    else
        stCertInfo.bDim = 0;

    if (bFloatType)
    {
        if (bBigType)
        {
            stCertInfo.eValType = SXF_VT_DOUBLE;
        }
        else
        {
            stCertInfo.eValType = SXF_VT_FLOAT;
        }
    }
    else
    {
        if (bBigType)
        {
            stCertInfo.eValType = SXF_VT_INT;
        }
        else
        {
            stCertInfo.eValType = SXF_VT_SHORT;
        }
    }

    stCertInfo.bFormat = CHECK_BIT(stRecordHeader.nRef[2], 0);
    stCertInfo.eGeomType = eGeomType;

    OGRFeature *poFeature = nullptr;
    if (stRecordHeader.nGeometryLength > 100 * 1024 * 1024)
        return nullptr;
    char *recordCertifBuf =
        (char *)VSI_MALLOC_VERBOSE(stRecordHeader.nGeometryLength);
    if (recordCertifBuf == nullptr)
        return nullptr;
    nObjectRead = static_cast<int>(
        VSIFReadL(recordCertifBuf, stRecordHeader.nGeometryLength, 1, fpSXF));
    if (nObjectRead != 1)
    {
        CPLError(CE_Failure, CPLE_FileIO, "SXF. Read geometry failed.");
        CPLFree(recordCertifBuf);
        return nullptr;
    }

    if (eGeomType == SXF_GT_Point)
        poFeature = TranslatePoint(stCertInfo, recordCertifBuf,
                                   stRecordHeader.nGeometryLength);
    else if (eGeomType == SXF_GT_Line || eGeomType == SXF_GT_VectorScaled)
        poFeature = TranslateLine(stCertInfo, recordCertifBuf,
                                  stRecordHeader.nGeometryLength);
    else if (eGeomType == SXF_GT_Polygon)
        poFeature = TranslatePolygon(stCertInfo, recordCertifBuf,
                                     stRecordHeader.nGeometryLength);
    else if (eGeomType == SXF_GT_Text)
        poFeature = TranslateText(stCertInfo, recordCertifBuf,
                                  stRecordHeader.nGeometryLength);
    else if (eGeomType == SXF_GT_VectorAngle)
    {
        poFeature = TranslateVetorAngle(stCertInfo, recordCertifBuf,
                                        stRecordHeader.nGeometryLength);
    }
#ifdef not_possible_given_above_code
    else if (eGeomType == SXF_GT_Vector)
    {
        CPLError(CE_Warning, CPLE_NotSupported,
                 "SXF. Geometry type Vector do not support.");
        CPLFree(recordCertifBuf);
        return NULL;
    }
#endif
    else if (eGeomType == SXF_GT_TextTemplate)  // TODO realize this
    {
        CPLError(CE_Warning, CPLE_NotSupported,
                 "SXF. Geometry type Text Template do not support.");
        CPLFree(recordCertifBuf);
        return nullptr;
    }
    else
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "SXF. Unsupported geometry type.");
        CPLFree(recordCertifBuf);
        return nullptr;
    }

    if (poFeature == nullptr)
    {
        CPLFree(recordCertifBuf);
        return nullptr;
    }

    poFeature->SetField(sFIDColumn_, (int)nFID);

    poFeature->SetField("CLCODE", (int)stRecordHeader.nClassifyCode);

    CPLString szName = mnClassificators[stRecordHeader.nClassifyCode];

    if (szName.empty())
    {
        szName.Printf("%d", stRecordHeader.nClassifyCode);
    }
    poFeature->SetField("CLNAME", szName);

    poFeature->SetField("OBJECTNUMB", stRecordHeader.nSubObjectCount);

    if (bHasAttributes)
    {
        if (stRecordHeader.nFullLength < 32 ||
            stRecordHeader.nGeometryLength > stRecordHeader.nFullLength - 32)
        {
            CPLFree(recordCertifBuf);
            delete poFeature;
            return nullptr;
        }
        size_t nSemanticsSize =
            stRecordHeader.nFullLength - 32 - stRecordHeader.nGeometryLength;
        if (nSemanticsSize > 1024 * 1024)
        {
            CPLFree(recordCertifBuf);
            delete poFeature;
            return nullptr;
        }
        char *psSemanticsdBuf = (char *)VSI_MALLOC_VERBOSE(nSemanticsSize);
        if (psSemanticsdBuf == nullptr)
        {
            CPLFree(recordCertifBuf);
            delete poFeature;
            return nullptr;
        }
        char *psSemanticsdBufOrig = psSemanticsdBuf;
        nObjectRead = static_cast<int>(
            VSIFReadL(psSemanticsdBuf, nSemanticsSize, 1, fpSXF));
        if (nObjectRead == 1)
        {
            size_t offset = 0;
            double nVal = 0;

            while (offset + sizeof(SXFRecordAttributeInfo) < nSemanticsSize)
            {
                char *psSemanticsdBufBeg = psSemanticsdBuf + offset;
                SXFRecordAttributeInfo stAttInfo =
                    *reinterpret_cast<SXFRecordAttributeInfo *>(
                        psSemanticsdBufBeg);
                CPL_LSBPTR16(&(stAttInfo.nCode));
                offset += 4;

                CPLString oFieldName;
                oFieldName.Printf("SC_%d", stAttInfo.nCode);

                CPLString oFieldValue;

                SXFRecordAttributeType eType =
                    (SXFRecordAttributeType)stAttInfo.nType;

                switch (eType)
                {
                    case SXF_RAT_ASCIIZ_DOS:
                    {
                        unsigned nLen = unsigned(stAttInfo.nScale) + 1;
                        if (nLen > nSemanticsSize ||
                            nSemanticsSize - nLen < offset)
                        {
                            nSemanticsSize = 0;
                            break;
                        }
                        char *value = (char *)CPLMalloc(nLen);
                        memcpy(value, psSemanticsdBuf + offset, nLen);
                        value[nLen - 1] = 0;
                        char *pszRecoded =
                            CPLRecode(value, "CP866", CPL_ENC_UTF8);
                        poFeature->SetField(oFieldName, pszRecoded);
                        CPLFree(pszRecoded);
                        CPLFree(value);

                        offset += stAttInfo.nScale + 1;
                        break;
                    }
                    case SXF_RAT_ONEBYTE:
                    {
                        if (offset + sizeof(GByte) > nSemanticsSize)
                        {
                            nSemanticsSize = 0;
                            break;
                        }
                        GByte nTmpVal = 0;
                        memcpy(&nTmpVal, psSemanticsdBuf + offset,
                               sizeof(GByte));
                        nVal = double(nTmpVal) *
                               pow(10.0, (double)stAttInfo.nScale);

                        poFeature->SetField(oFieldName, nVal);
                        offset += 1;
                        break;
                    }
                    case SXF_RAT_TWOBYTE:
                    {
                        if (offset + sizeof(GInt16) > nSemanticsSize)
                        {
                            nSemanticsSize = 0;
                            break;
                        }
                        GInt16 nTmpVal = 0;
                        memcpy(&nTmpVal, psSemanticsdBuf + offset,
                               sizeof(GInt16));
                        nVal = double(CPL_LSBWORD16(nTmpVal)) *
                               pow(10.0, (double)stAttInfo.nScale);

                        poFeature->SetField(oFieldName, nVal);
                        offset += 2;
                        break;
                    }
                    case SXF_RAT_FOURBYTE:
                    {
                        if (offset + sizeof(GInt32) > nSemanticsSize)
                        {
                            nSemanticsSize = 0;
                            break;
                        }
                        GInt32 nTmpVal = 0;
                        memcpy(&nTmpVal, psSemanticsdBuf + offset,
                               sizeof(GInt32));
                        nVal = double(CPL_LSBWORD32(nTmpVal)) *
                               pow(10.0, (double)stAttInfo.nScale);

                        poFeature->SetField(oFieldName, nVal);
                        offset += 4;
                        break;
                    }
                    case SXF_RAT_EIGHTBYTE:
                    {
                        if (offset + sizeof(double) > nSemanticsSize)
                        {
                            nSemanticsSize = 0;
                            break;
                        }
                        double dfTmpVal = 0.0;
                        memcpy(&dfTmpVal, psSemanticsdBuf + offset,
                               sizeof(double));
                        CPL_LSBPTR64(&dfTmpVal);
                        const double d =
                            dfTmpVal * pow(10.0, (double)stAttInfo.nScale);

                        poFeature->SetField(oFieldName, d);

                        offset += 8;
                        break;
                    }
                    case SXF_RAT_ANSI_WIN:
                    {
                        unsigned nLen = unsigned(stAttInfo.nScale) + 1;
                        if (nLen > nSemanticsSize ||
                            nSemanticsSize - nLen < offset)
                        {
                            nSemanticsSize = 0;
                            break;
                        }
                        char *value = (char *)CPLMalloc(nLen);
                        memcpy(value, psSemanticsdBuf + offset, nLen);
                        value[nLen - 1] = 0;
                        char *pszRecoded =
                            CPLRecode(value, "CP1251", CPL_ENC_UTF8);
                        poFeature->SetField(oFieldName, pszRecoded);
                        CPLFree(pszRecoded);
                        CPLFree(value);

                        offset += nLen;
                        break;
                    }
                    case SXF_RAT_UNICODE:
                    {
                        uint64_t nLen64 =
                            (static_cast<uint64_t>(stAttInfo.nScale) + 1) * 2;
                        unsigned nLen = static_cast<unsigned>(nLen64);
                        if (/* nLen < 2 || */ nLen64 > nSemanticsSize ||
                            nSemanticsSize - nLen < offset)
                        {
                            nSemanticsSize = 0;
                            break;
                        }
                        char *value = (char *)CPLMalloc(nLen);
                        memcpy(value, psSemanticsdBuf + offset, nLen - 2);
                        value[nLen - 1] = 0;
                        value[nLen - 2] = 0;
                        char *dst = (char *)CPLMalloc(nLen);
                        int nCount = 0;
                        for (int i = 0; (unsigned)i < nLen; i += 2)
                        {
                            unsigned char ucs = value[i];

                            if (ucs < 0x80U)
                            {
                                dst[nCount++] = ucs;
                            }
                            else
                            {
                                dst[nCount++] = 0xc0 | (ucs >> 6);
                                dst[nCount++] = 0x80 | (ucs & 0x3F);
                            }
                        }

                        poFeature->SetField(oFieldName, dst);
                        CPLFree(dst);
                        CPLFree(value);

                        offset += nLen;
                        break;
                    }
                    case SXF_RAT_BIGTEXT:
                    {
                        if (offset + sizeof(GUInt32) > nSemanticsSize)
                        {
                            nSemanticsSize = 0;
                            break;
                        }
                        GUInt32 scale2 = 0;
                        memcpy(&scale2, psSemanticsdBuf + offset,
                               sizeof(GUInt32));
                        CPL_LSBPTR32(&scale2);
                        /* FIXME add ?: offset += sizeof(GUInt32); */
                        if (scale2 > nSemanticsSize - 1 ||
                            nSemanticsSize - (scale2 + 1) < offset)
                        {
                            nSemanticsSize = 0;
                            break;
                        }

                        char *value = (char *)CPLMalloc(scale2 + 1);
                        memcpy(value, psSemanticsdBuf + offset, scale2 + 1);
                        value[scale2] = 0;
                        char *pszRecoded =
                            CPLRecode(value, CPL_ENC_UTF16, CPL_ENC_UTF8);
                        poFeature->SetField(oFieldName, pszRecoded);
                        CPLFree(pszRecoded);
                        CPLFree(value);

                        offset += scale2;
                        break;
                    }
                    default:
                        CPLFree(recordCertifBuf);
                        CPLFree(psSemanticsdBufOrig);
                        delete poFeature;
                        return nullptr;
                }
            }
        }
        CPLFree(psSemanticsdBufOrig);
    }

    poFeature->SetFID(nFID);

    CPLFree(recordCertifBuf);

    return poFeature;
}

/************************************************************************/
/*                         TranslatePoint   ()                          */
/************************************************************************/

OGRFeature *OGRSXFLayer::TranslatePoint(const SXFRecordDescription &certifInfo,
                                        const char *psRecordBuf,
                                        GUInt32 nBufLen)
{
    double dfX = 1.0;
    double dfY = 1.0;
    double dfZ = 0.0;
    GUInt32 nOffset = 0;
    GUInt32 nDelta = 0;

    if (certifInfo.bDim == 1)
    {
        nDelta =
            TranslateXYH(certifInfo, psRecordBuf, nBufLen, &dfX, &dfY, &dfZ);
    }
    else
    {
        nDelta = TranslateXYH(certifInfo, psRecordBuf, nBufLen, &dfX, &dfY);
    }

    if (nDelta == 0)
        return nullptr;
    nOffset += nDelta;

    // OGRFeatureDefn *fd = poFeatureDefn->Clone();
    // fd->SetGeomType( wkbMultiPoint );
    //   OGRFeature *poFeature = new OGRFeature(fd);
    OGRFeature *poFeature = new OGRFeature(poFeatureDefn);
    OGRMultiPoint *poMPt = new OGRMultiPoint();

    poMPt->addGeometryDirectly(new OGRPoint(dfX, dfY, dfZ));

    /*---------------------- Reading SubObjects
     * --------------------------------*/

    for (int count = 0; count < certifInfo.nSubObjectCount; count++)
    {
        if (nOffset + 4 > nBufLen)
            break;

        GUInt16 nSubObj = 0;
        memcpy(&nSubObj, psRecordBuf + nOffset, 2);
        CPL_LSBPTR16(&nSubObj);

        GUInt16 nCoords = 0;
        memcpy(&nCoords, psRecordBuf + nOffset + 2, 2);
        CPL_LSBPTR16(&nCoords);

        nOffset += 4;

        for (int i = 0; i < nCoords; i++)
        {
            const char *psCoords = psRecordBuf + nOffset;

            if (certifInfo.bDim == 1)
            {
                nDelta = TranslateXYH(certifInfo, psCoords, nBufLen - nOffset,
                                      &dfX, &dfY, &dfZ);
            }
            else
            {
                dfZ = 0.0;
                nDelta = TranslateXYH(certifInfo, psCoords, nBufLen - nOffset,
                                      &dfX, &dfY);
            }

            if (nDelta == 0)
                break;
            nOffset += nDelta;

            poMPt->addGeometryDirectly(new OGRPoint(dfX, dfY, dfZ));
        }
    }

    /*****
     * TODO :
     *          - Translate graphics
     *          - Translate 3D vector
     */

    poFeature->SetGeometryDirectly(poMPt);

    return poFeature;
}

/************************************************************************/
/*                         TranslateLine    ()                          */
/************************************************************************/

OGRFeature *OGRSXFLayer::TranslateLine(const SXFRecordDescription &certifInfo,
                                       const char *psRecordBuf, GUInt32 nBufLen)
{
    double dfX = 1.0;
    double dfY = 1.0;
    double dfZ = 0.0;
    GUInt32 nOffset = 0;

    // OGRFeatureDefn *fd = poFeatureDefn->Clone();
    // fd->SetGeomType( wkbMultiLineString );
    //   OGRFeature *poFeature = new OGRFeature(fd);

    OGRFeature *poFeature = new OGRFeature(poFeatureDefn);
    OGRMultiLineString *poMLS = new OGRMultiLineString();

    /*---------------------- Reading Primary Line
     * --------------------------------*/

    OGRLineString *poLS = new OGRLineString();

    for (GUInt32 count = 0; count < certifInfo.nPointCount; count++)
    {
        const char *psCoords = psRecordBuf + nOffset;

        GInt32 nDelta;
        if (certifInfo.bDim == 1)
        {
            nDelta = TranslateXYH(certifInfo, psCoords, nBufLen - nOffset, &dfX,
                                  &dfY, &dfZ);
        }
        else
        {
            dfZ = 0.0;
            nDelta = TranslateXYH(certifInfo, psCoords, nBufLen - nOffset, &dfX,
                                  &dfY);
        }

        if (nDelta == 0)
            break;
        nOffset += nDelta;

        poLS->addPoint(dfX, dfY, dfZ);
    }

    poMLS->addGeometry(poLS);

    /*---------------------- Reading Sub Lines
     * --------------------------------*/

    for (GUInt16 count = 0; count < certifInfo.nSubObjectCount; count++)
    {
        poLS->empty();

        if (nOffset + 4 > nBufLen)
            break;

        GUInt16 nSubObj = 0;
        memcpy(&nSubObj, psRecordBuf + nOffset, 2);
        CPL_LSBPTR16(&nSubObj);

        GUInt16 nCoords = 0;
        memcpy(&nCoords, psRecordBuf + nOffset + 2, 2);
        CPL_LSBPTR16(&nCoords);

        nOffset += 4;

        for (GUInt16 i = 0; i < nCoords; i++)
        {
            const char *psCoords = psRecordBuf + nOffset;
            GInt32 nDelta;
            if (certifInfo.bDim == 1)
            {
                nDelta = TranslateXYH(certifInfo, psCoords, nBufLen - nOffset,
                                      &dfX, &dfY, &dfZ);
            }
            else
            {
                dfZ = 0.0;
                nDelta = TranslateXYH(certifInfo, psCoords, nBufLen - nOffset,
                                      &dfX, &dfY);
            }

            if (nDelta == 0)
                break;
            nOffset += nDelta;

            poLS->addPoint(dfX, dfY, dfZ);
        }

        poMLS->addGeometry(poLS);
    }  // for

    delete poLS;
    poFeature->SetGeometryDirectly(poMLS);

    /*****
     * TODO :
     *          - Translate graphics
     *          - Translate 3D vector
     */

    return poFeature;
}

/************************************************************************/
/*                       TranslateVetorAngle()                          */
/************************************************************************/

OGRFeature *
OGRSXFLayer::TranslateVetorAngle(const SXFRecordDescription &certifInfo,
                                 const char *psRecordBuf, GUInt32 nBufLen)
{
    if (certifInfo.nPointCount != 2)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "SXF. The vector object should have 2 points, but not.");
        return nullptr;
    }

    GUInt32 nOffset = 0;

    OGRFeature *poFeature = new OGRFeature(poFeatureDefn);
    OGRPoint *poPT = new OGRPoint();

    /*---------------------- Reading Primary Line
     * --------------------------------*/

    OGRLineString *poLS = new OGRLineString();

    for (GUInt32 count = 0; count < certifInfo.nPointCount; count++)
    {
        const char *psCoords = psRecordBuf + nOffset;

        double dfX = 1.0;
        double dfY = 1.0;
        double dfZ = 0.0;
        GInt32 nDelta;
        if (certifInfo.bDim == 1)
        {
            nDelta = TranslateXYH(certifInfo, psCoords, nBufLen - nOffset, &dfX,
                                  &dfY, &dfZ);
        }
        else
        {
            dfZ = 0.0;
            nDelta = TranslateXYH(certifInfo, psCoords, nBufLen - nOffset, &dfX,
                                  &dfY);
        }
        if (nDelta == 0)
            break;
        nOffset += nDelta;

        poLS->addPoint(dfX, dfY, dfZ);
    }

    poLS->StartPoint(poPT);

    OGRPoint *poAngPT = new OGRPoint();
    poLS->EndPoint(poAngPT);

    const double xDiff = poPT->getX() - poAngPT->getX();
    const double yDiff = poPT->getY() - poAngPT->getY();
    double dfAngle = atan2(xDiff, yDiff) * TO_DEGREES - 90;
    if (dfAngle < 0)
        dfAngle += 360;

    poFeature->SetGeometryDirectly(poPT);
    poFeature->SetField("ANGLE", dfAngle);

    delete poAngPT;
    delete poLS;

    return poFeature;
}

/************************************************************************/
/*                         TranslatePolygon ()                          */
/************************************************************************/

OGRFeature *
OGRSXFLayer::TranslatePolygon(const SXFRecordDescription &certifInfo,
                              const char *psRecordBuf, GUInt32 nBufLen)
{
    double dfX = 1.0;
    double dfY = 1.0;
    double dfZ = 0.0;
    GUInt32 nOffset = 0;
    GUInt32 nDelta = 0;

    OGRFeature *poFeature = new OGRFeature(poFeatureDefn);
    OGRPolygon *poPoly = new OGRPolygon();
    OGRLineString *poLS = new OGRLineString();

    /*---------------------- Reading Primary Polygon
     * --------------------------------*/
    for (GUInt32 count = 0; count < certifInfo.nPointCount; count++)
    {
        const char *psBuf = psRecordBuf + nOffset;
        if (certifInfo.bDim == 1)
        {
            nDelta = TranslateXYH(certifInfo, psBuf, nBufLen - nOffset, &dfX,
                                  &dfY, &dfZ);
        }
        else
        {
            dfZ = 0.0;
            nDelta =
                TranslateXYH(certifInfo, psBuf, nBufLen - nOffset, &dfX, &dfY);
        }

        if (nDelta == 0)
            break;
        nOffset += nDelta;
        poLS->addPoint(dfX, dfY, dfZ);
    }  // for

    OGRLinearRing *poLR = new OGRLinearRing();
    poLR->addSubLineString(poLS, 0);

    poPoly->addRingDirectly(poLR);

    /*---------------------- Reading Sub Lines
     * --------------------------------*/

    for (int count = 0; count < certifInfo.nSubObjectCount; count++)
    {
        poLS->empty();

        if (nOffset + 4 > nBufLen)
            break;

        GUInt16 nSubObj = 0;
        memcpy(&nSubObj, psRecordBuf + nOffset, 2);
        CPL_LSBPTR16(&nSubObj);

        GUInt16 nCoords = 0;
        memcpy(&nCoords, psRecordBuf + nOffset + 2, 2);
        CPL_LSBPTR16(&nCoords);

        // TODO: Is this really what the buffer size should be?
        if (nCoords * nDelta != nBufLen - nOffset + 2 - 6)
        {
            CPLError(CE_Warning, CPLE_FileIO,
                     "SXF raw feature size incorrect.  "
                     "%d %d",
                     nCoords * nDelta, nBufLen - nOffset + 2 - 6);
            // TODO: How best to gracefully exit and report an issue?
            // break; or cleanup and return NULL?
        }

        nOffset += 4;

        for (int i = 0; i < nCoords; i++)
        {
            const char *psCoords = psRecordBuf + nOffset;
            if (certifInfo.bDim == 1)
            {
                nDelta = TranslateXYH(certifInfo, psCoords, nBufLen - nOffset,
                                      &dfX, &dfY, &dfZ);
            }
            else
            {
                dfZ = 0.0;
                nDelta = TranslateXYH(certifInfo, psCoords, nBufLen - nOffset,
                                      &dfX, &dfY);
            }

            if (nDelta == 0)
                break;
            nOffset += nDelta;

            poLS->addPoint(dfX, dfY, dfZ);
        }

        poLR = new OGRLinearRing();
        poLR->addSubLineString(poLS, 0);

        poPoly->addRingDirectly(poLR);
    }  // for

    poFeature->SetGeometryDirectly(poPoly);  // poLS);
    delete poLS;

    /*****
     * TODO :
     *          - Translate graphics
     *          - Translate 3D vector
     */
    return poFeature;
}

/************************************************************************/
/*                         TranslateText    ()                          */
/************************************************************************/
OGRFeature *OGRSXFLayer::TranslateText(const SXFRecordDescription &certifInfo,
                                       const char *psRecordBuf, GUInt32 nBufLen)
{
    double dfX = 1.0;
    double dfY = 1.0;
    double dfZ = 0.0;
    GUInt32 nOffset = 0;

    OGRFeature *poFeature = new OGRFeature(poFeatureDefn);
    OGRMultiLineString *poMLS = new OGRMultiLineString();

    /*---------------------- Reading Primary Line
     * --------------------------------*/

    OGRLineString *poLS = new OGRLineString();

    for (GUInt32 count = 0; count < certifInfo.nPointCount; count++)
    {
        const char *psCoords = psRecordBuf + nOffset;

        GUInt32 nDelta;
        if (certifInfo.bDim == 1)
        {
            nDelta = TranslateXYH(certifInfo, psCoords, nBufLen - nOffset, &dfX,
                                  &dfY, &dfZ);
        }
        else
        {
            dfZ = 0.0;
            nDelta = TranslateXYH(certifInfo, psCoords, nBufLen - nOffset, &dfX,
                                  &dfY);
        }

        if (nDelta == 0)
            break;
        nOffset += nDelta;

        poLS->addPoint(dfX, dfY, dfZ);
    }

    poMLS->addGeometry(poLS);

    /*------------------     READING TEXT VALUE
     * --------------------------------*/
    CPLString soText;

    if (certifInfo.bHasTextSign)
    {
        if (nOffset + 1 > nBufLen)
            return poFeature;
        const char *pszTxt = psRecordBuf + nOffset;
        GByte nTextL = (GByte)*pszTxt;
        if (nOffset + 1 + nTextL > nBufLen)
            return poFeature;

        char *pszTextBuf = (char *)CPLMalloc(nTextL + 1);

        strncpy(pszTextBuf, (pszTxt + 1), nTextL);
        pszTextBuf[nTextL] = '\0';

        // TODO: Check encoding from sxf
        char *pszRecoded = CPLRecode(pszTextBuf, "CP1251", CPL_ENC_UTF8);
        soText += pszRecoded;
        CPLFree(pszRecoded);

        CPLFree(pszTextBuf);

        nOffset += nTextL + 2;
    }

    /*---------------------- Reading Sub Lines
     * --------------------------------*/

    for (int count = 0; count < certifInfo.nSubObjectCount; count++)
    {
        poLS->empty();

        if (nOffset + 4 > nBufLen)
            break;

        GUInt16 nSubObj = 0;
        memcpy(&nSubObj, psRecordBuf + nOffset, 2);
        CPL_LSBPTR16(&nSubObj);

        GUInt16 nCoords = 0;
        memcpy(&nCoords, psRecordBuf + nOffset + 2, 2);
        CPL_LSBPTR16(&nCoords);

        nOffset += 4;

        for (int i = 0; i < nCoords; i++)
        {
            const char *psCoords = psRecordBuf + nOffset;
            GUInt32 nDelta;
            if (certifInfo.bDim == 1)
            {
                nDelta = TranslateXYH(certifInfo, psCoords, nBufLen - nOffset,
                                      &dfX, &dfY, &dfZ);
            }
            else
            {
                dfZ = 0.0;
                nDelta = TranslateXYH(certifInfo, psCoords, nBufLen - nOffset,
                                      &dfX, &dfY);
            }

            if (nDelta == 0)
                break;
            nOffset += nDelta;

            poLS->addPoint(dfX, dfY, dfZ);
        }

        poMLS->addGeometry(poLS);

        if (certifInfo.bHasTextSign)
        {
            if (nOffset + 1 > nBufLen)
                return poFeature;
            const char *pszTxt = psRecordBuf + nOffset;
            GByte nTextL = (GByte)*pszTxt;
            if (nOffset + 1 + nTextL > nBufLen)
                return poFeature;

            char *pszTextBuf = (char *)CPLMalloc(nTextL + 1);

            strncpy(pszTextBuf, (pszTxt + 1), nTextL);
            pszTextBuf[nTextL] = '\0';

            // TODO: Check encoding from sxf
            char *pszRecoded = CPLRecode(pszTextBuf, "CP1251", CPL_ENC_UTF8);
            soText += " " + CPLString(pszRecoded);
            CPLFree(pszRecoded);

            CPLFree(pszTextBuf);

            nOffset += nTextL + 2;
        }
    }  // for

    delete poLS;
    poFeature->SetGeometryDirectly(poMLS);

    poFeature->SetField("TEXT", soText);
    return poFeature;
}

const char *OGRSXFLayer::GetFIDColumn()
{
    return sFIDColumn_.c_str();
}
